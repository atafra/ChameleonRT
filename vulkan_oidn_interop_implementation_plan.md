# Vulkan OIDN Interop Implementation Plan

## Goals

Improve the Vulkan OIDN interop implementation while preserving the ability to compare all three synchronization modes:

- `host_blocking`
- `timeline_semaphore`
- `binary_semaphore`

Keep `host_blocking` as the default mode so benchmark runs remain explicit and comparable. Assume `REPORT_RAY_STATS` is disabled for the relevant performance tests.

## Non-goals

- Do not remove any synchronization mode.
- Do not change the default Vulkan OIDN mode away from `host_blocking`.
- Do not optimize the ray-stats readback path as part of this work.
- Do not move code or generated artifacts into the build directory.

## Current Status and Findings

### Phase 1 completed

The first implementation phase has been completed and validated with a successful build.

Implemented changes:

- Preserved all three Vulkan OIDN synchronization modes.
- Changed the Vulkan default OIDN mode back to `host_blocking`.
- Simplified the Vulkan timeline semaphore path to use a single monotonic timeline counter.
- Updated the timeline path to follow the DXR-style sequence:
  1. Vulkan render signals `render_done_value`.
  2. OIDN waits for `render_done_value`.
  3. OIDN executes asynchronously.
  4. OIDN signals `denoise_done_value`.
  5. Vulkan tonemap waits for `denoise_done_value`.
- Removed the redundant render-side timeline wait and tonemap-side timeline signal.
- Aligned Vulkan timing semantics with DXR by treating ray tracing, denoise, and tonemap as serialized passes for one frame.
- Set `stats.passes_overlap = false` for all Vulkan OIDN modes.

Observed benchmark result after Phase 1:

- Overall Vulkan performance improved.
- `host_blocking` and `timeline_semaphore` improved by roughly the same amount.
- `timeline_semaphore` still does not show a clear performance gain over `host_blocking`.

Interpretation:

- The Phase 1 cleanup removed some unnecessary synchronization overhead or timing/path complexity that affected both modes.
- The lack of a timeline-specific gain suggests the remaining bottleneck is probably not the timeline value model itself.
- The next most likely causes are Vulkan external-memory ownership/cache synchronization, queue-family transfer behavior, OIDN/Vulkan memory visibility, or remaining host-side work around OIDN submission.

Next recommended phase:

- Implement explicit external ownership and memory synchronization for `accum_buffer` and `denoise_buffer`.
- Keep the change focused on correctness and visibility first, then benchmark whether timeline semaphore begins to separate from host blocking.

## Implementation Steps

### 1. Add explicit external ownership and memory synchronization

Add Vulkan barriers around buffers shared with OIDN/SYCL:

- `accum_buffer`, written by Vulkan ray tracing and read by OIDN.
- `denoise_buffer`, written by OIDN and read by Vulkan tonemap.

The render command buffer should release `accum_buffer` for external access after ray tracing completes. The tonemap command buffer should acquire `denoise_buffer` back from external access before dispatching tonemap.

Recommended direction:

- Use `srcQueueFamilyIndex = device->queue_index()` and `dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL` for Vulkan-to-OIDN release.
- Use `srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL` and `dstQueueFamilyIndex = device->queue_index()` for OIDN-to-Vulkan acquire.
- Use narrow stage/access masks where possible:
  - Ray tracing source stage: `VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR`
  - Ray tracing source access: `VK_ACCESS_SHADER_WRITE_BIT`
  - Tonemap destination stage: `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`
  - Tonemap destination access: `VK_ACCESS_SHADER_READ_BIT`

If validation or driver behavior requires it, evaluate `VK_QUEUE_FAMILY_FOREIGN_EXT` as a fallback for the external queue family.

### 2. Simplify timeline semaphore value management

Status: completed in Phase 1.

Refactor the timeline semaphore path to more closely match the DXR fence model.

Current Vulkan code uses separate value streams for render, OIDN, and tonemap. Replace this with a single monotonic timeline counter for OIDN interop.

Suggested model per frame:

1. Render submit signals `render_done_value`.
2. OIDN waits for `render_done_value`.
3. OIDN executes asynchronously.
4. OIDN signals `denoise_done_value`.
5. Tonemap submit waits for `denoise_done_value`.

The render submit should not need to wait on the previous timeline value if render work is already submitted in order on the same Vulkan queue and per-slot fences protect command-buffer reuse.

Add a member similar to:

- `uint64_t oidn_timeline_value = 1;`

Each async timeline frame reserves two values:

- `render_done_value = oidn_timeline_value++`
- `denoise_done_value = oidn_timeline_value++`

### 3. Keep binary semaphore mode, but make reuse safe

Preserve `binary_semaphore` as a benchmarkable mode, but avoid reusing the same binary semaphore pair across potentially overlapping frames.

Recommended approach:

- Allocate binary semaphore pairs per in-flight frame slot:
  - `render_ready_semaphore[MAX_FRAMES_IN_FLIGHT]`
  - `oidn_ready_semaphore[MAX_FRAMES_IN_FLIGHT]`
  - matching OIDN semaphore refs for each slot
- Use the current frame slot’s pair in `RenderVulkan::render()`.
- Destroy all per-slot semaphores in the destructor and during resize/reinitialization.

This preserves binary semaphore benchmarking while avoiding unsafe signal/wait reuse when `waitSemaphoreAsync()` and `signalSemaphoreAsync()` return before the external wait/signal has completed.

### 4. Keep host blocking as the default mode

Status: completed in Phase 1.

Update or preserve the default in `render_vulkan.h`:

- `OIDNInteropMode::HostBlocking`

The benchmark harness or command-line option should continue to select `timeline_semaphore` and `binary_semaphore` explicitly when comparing synchronization modes.

### 5. Align Vulkan timing semantics with DXR

Status: completed in Phase 1.

Because ray tracing, OIDN denoise, and tonemap operate on dependent resources for a single frame, they are serialized even when OIDN is submitted asynchronously.

Update Vulkan statistics to match DXR semantics:

- Estimate denoise time as the GPU-visible gap between ray tracing end and tonemap begin.
- Set `stats.passes_overlap = false` for all OIDN modes.

This avoids reporting misleading overlap for the async Vulkan paths.

### 6. Improve capability checks and fallback behavior

Replace broad assumptions about external semaphore support with explicit capability checks.

Recommended checks:

- Query timeline semaphore support through Vulkan feature/property queries.
- Query external semaphore support with `vkGetPhysicalDeviceExternalSemaphoreProperties`.
- Verify that the requested external handle type supports export/import for binary and timeline modes.
- After each OIDN import call, check `oidn_device.getError()`.

Fallback rules:

- If `timeline_semaphore` is requested but not supported, print a clear warning and fall back to `host_blocking`.
- If `binary_semaphore` is requested but not supported, print a clear warning and fall back to `host_blocking`.
- Keep `get_supported_oidn_interop_modes()` consistent with the actual probed capabilities.

### 7. Close or release exported handles after OIDN import

Match the DXR backend behavior by closing local exported handles after OIDN imports them.

Apply this to:

- exported buffer memory handles for `accum_buffer` and `denoise_buffer`
- exported timeline semaphore handles
- exported binary semaphore handles

On Windows, call `CloseHandle()` after successful import. On Linux, close file descriptors after successful import when ownership is not transferred by the API call.

### 8. Add optional diagnostics for Vulkan interop mode debugging

Add diagnostics similar in spirit to `ENABLE_DXR_FRAME_DIAGNOSTICS`, but keep them compile-time gated.

Useful events:

- selected OIDN interop mode
- semaphore creation/import success or fallback
- timeline values used per frame
- binary semaphore slot used per frame
- OIDN wait/execute/signal submission sequence
- Vulkan tonemap wait submission sequence

This should make future Vulkan-vs-DXR comparisons easier without affecting release benchmark results.

## Validation Plan

1. Build with `ENABLE_OIDN` enabled and `REPORT_RAY_STATS` disabled.
   - Phase 1 status: build succeeded.
2. Run a short benchmark or interactive render in each mode:
   - `host_blocking`
   - `timeline_semaphore`
   - `binary_semaphore`
3. Confirm each mode produces correct images.
4. Run with Vulkan validation enabled and check for synchronization or external handle warnings.
5. Compare frame-time behavior against DXR:
   - host blocking should remain the baseline.
   - timeline semaphore should reduce host-side synchronization overhead.
   - binary semaphore should remain functional and benchmarkable, even if slower than timeline.
   - Phase 1 finding: host blocking and timeline semaphore both improved, but timeline semaphore did not yet gain relative to host blocking.
6. Confirm the default Vulkan mode remains `host_blocking`.
   - Phase 1 status: confirmed in code.

## Expected Outcome

The Vulkan backend should remain suitable for three-way synchronization benchmarking while improving correctness and performance quality for the async OIDN paths. The timeline semaphore implementation should become closer to the DXR fence implementation, and binary semaphore mode should remain available without unsafe cross-frame semaphore reuse.
