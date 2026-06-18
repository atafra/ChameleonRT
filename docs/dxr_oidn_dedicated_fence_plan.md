# Implementation Plan - Dedicated Fence for DXR OIDN-SYCL Interop

Status: **IMPLEMENTED**
Branch: `gliktor-oidn2-easybuild`
Scope: `backends/dxr` only (DirectX Ray Tracing backend).

> Implementation note: all changes in section 3 are applied. The dedicated
> `oidn_fence` / `oidn_fence_value` were added, the OIDN semaphore is built on the
> dedicated fence, the DeviceAsync render path drives that fence, the handshake
> `fence` was reverted to `D3D12_FENCE_FLAG_NONE` (section 3.2 optional cleanup),
> and the three imported OIDN shared `HANDLE`s are now closed after import
> (section 3.3). Full `ENABLE_OIDN` build succeeds; `device_async` and
> `host_blocking` modes were verified at runtime.

## 1. Problem

The DXR backend uses a **single** `ID3D12Fence` (`fence`) and a single monotonic
counter (`fence_value`) for **two unrelated** synchronization roles:

1. **CPU<->GPU handshakes** - host-side waits used to reclaim in-flight frame
   slots, drain the queue, and gate framebuffer / stats readback.
2. **OIDN<->SYCL interop** - the same fence is shared with OIDN as an external
   semaphore, and the SYCL context asynchronously waits/signals on it.

Because both roles share one fence timeline, the host CPU and the SYCL context
can compete asynchronously for the same semaphore. This is a correctness and
performance hazard: host waits and SYCL signals/waits are interleaved on values
drawn from one counter, so a denoise interop step can perturb (or be perturbed
by) the CPU<->GPU handshake.

### 1.1 Where the coupling lives today

| File | Lines (approx) | Current behavior |
|---|---|---|
| `render_dxr.h` | 63-65 | `uint64_t fence_value`, `ComPtr<ID3D12Fence> fence`, `HANDLE fence_evt` - the one shared fence. |
| `render_dxr.h` | 99-107 | `#ifdef ENABLE_OIDN` members: `oidn_device`, `oidn_filter`, `oidn_semaphore`, interop mode flags. |
| `render_dxr.cpp` | 950-953 | `create_device_objects()` creates `fence` with `D3D12_FENCE_FLAG_SHARED` and `fence_evt`. |
| `render_dxr.cpp` | 327-333 | OIDN init does `CreateSharedHandle(fence.Get(), ...)` then `oidn_device.newSemaphore(D3D12Fence, handle)` - the semaphore is built on the **CPU/GPU fence**. |
| `render_dxr.cpp` | 724-773 | `render()` DeviceAsync path: borrows `oidn_fence_value = fence_value; fence_value += 2;` then `cmd_queue->Signal(fence, ...)`, `waitSemaphoreAsync`, `executeAsync`, `signalSemaphoreAsync`, `cmd_queue->Wait(fence, ...)` - all on the shared `fence`/counter. |
| `render_dxr.cpp` | 809-810 | Per-slot completion `slot_fence_value[slot] = fence_value++; cmd_queue->Signal(fence, ...)`. |
| `render_dxr.cpp` | 1476-1560 | `sync_gpu()` / `wait_for_fence_value()` host waits on `fence` via `SetEventOnCompletion` + `WaitForSingleObject`. |

Note: the OIDN buffer interop (shared `accum_buffer` / `denoise_buffer`) is
**not** affected and is out of scope. Only the **fence/semaphore** sharing
changes.

## 2. Goal

Introduce a **dedicated fence used exclusively for OIDN-SYCL semaphore sharing**,
while the existing `fence` keeps handling all CPU<->GPU handshakes. The two
timelines become fully independent, removing the async contention.

## 3. Proposed changes

### 3.1 New members (`render_dxr.h`, inside `#ifdef ENABLE_OIDN`)

- `Microsoft::WRL::ComPtr<ID3D12Fence> oidn_fence;`
- `uint64_t oidn_fence_value = 1;` (dedicated counter for the OIDN timeline).

No new `HANDLE`/event is needed: the host never CPU-waits on `oidn_fence`; only
the GPU queue and the SYCL context touch it.

### 3.2 Create the dedicated fence

- In OIDN setup (preferred: the `#ifdef ENABLE_OIDN` block in `initialize()`,
  near lines 281-349, before creating the semaphore), create
  `oidn_fence` with `D3D12_FENCE_FLAG_SHARED`.
- Optional cleanup: the existing `fence` no longer needs to be shared. Its
  `D3D12_FENCE_FLAG_SHARED` (line 952) may be reverted to
  `D3D12_FENCE_FLAG_NONE`. Keep this change minimal/optional to limit risk.

### 3.3 Build the semaphore on the dedicated fence

- Change the OIDN init (lines 327-333) so `CreateSharedHandle(...)` is called on
  `oidn_fence.Get()` instead of `fence.Get()`, and `newSemaphore(...)` consumes
  that handle.
- Close the returned shared `HANDLE` after `newSemaphore` (OIDN duplicates it);
  align with existing handle handling.

  Done: `win32_fence_handle` is now closed after `newSemaphore`. The two
  pre-existing buffer handles (`accum_buffer_handle`, `denoise_buffer_handle`)
  are also closed after their `newBuffer` imports, fixing the per-init handle
  leaks.

### 3.4 Drive the OIDN timeline in `render()` (DeviceAsync path, lines 724-773)

- Replace the borrow-from-`fence_value` logic with the dedicated counter:
  - `const uint64_t wait_val = oidn_fence_value; oidn_fence_value += 2;`
  - `cmd_queue->Signal(oidn_fence.Get(), wait_val);`
  - `oidn_device.waitSemaphoreAsync(oidn_semaphore, wait_val);`
  - `oidn_filter.executeAsync();`
  - `oidn_device.signalSemaphoreAsync(oidn_semaphore, wait_val + 1);`
  - `cmd_queue->Wait(oidn_fence.Get(), wait_val + 1);`
- Result: `fence_value` is no longer advanced by the denoise step, so the
  CPU<->GPU handshake timeline is untouched by OIDN.

### 3.5 Leave handshake paths unchanged

- `sync_gpu()`, `wait_for_fence_value()`, slot signaling (809-810), and the
  destructor's `sync_gpu()` / `CloseHandle(fence_evt)` continue to use the
  original `fence` / `fence_value` / `fence_evt`. No change required.

## 4. Edge cases / notes

- **Initial values:** `oidn_fence` starts at 0; `oidn_fence_value` starts at 1 so
  the first wait target is non-zero. Each denoise consumes two values
  (`wait_val`, `wait_val + 1`).
- **HostBlocking mode** (lines 713-718) does not use the semaphore and needs no
  change.
- **Build guards:** all new members/logic stay under `#ifdef ENABLE_OIDN`.
- **Lifetime:** `oidn_fence` is a `ComPtr` and frees automatically; nothing to
  add to the destructor.

## 5. Validation

- [x] Build with `ENABLE_OIDN` enabled (DXR backend) - full VS solution build
  succeeds.
- [x] Run `device_async` interop mode - no deadlock, correct denoised output.
- [x] Run `host_blocking` mode - no regression.
- [ ] If `ENABLE_DXR_FRAME_DIAGNOSTICS` is on, confirm the OIDN fence interactions
  are now reported on the dedicated fence and the handshake fence values are no
  longer perturbed by denoise frames. (Optional diagnostic-only check.)

## 6. Out of scope

- Vulkan / Metal backends.
- OIDN buffer (memory) sharing.
- Any change to interop-mode selection / capability detection logic.
