# Minimal Reproducers

This folder tracks temporary minimal reproducers for last-known graphics driver issues. These files are intended to be easy to share with driver/runtime teams without requiring them to analyze or build the full ChameleonRT workload.

Reproducers in this folder should:

- Be standalone when practical.
- Include concise build/run instructions.
- Print enough diagnostics to locate hangs or failures in Release builds.
- Document the system and driver versions where the issue was last reproduced.
- Be removed or archived once the underlying driver/runtime issue is fixed and no longer relevant.

## `vulkan_oidn_timeline_semaphore_repro.cpp`

### Issue

This reproducer tracks a Vulkan/SYCL external timeline semaphore interop hang observed in the `RenderVulkan` timeline-semaphore path.

It removes the `OIDN` dependency and all external memory/kernel work. The current minimized repro keeps only the synchronization protocol:

1. Vulkan creates an exportable timeline semaphore.
2. SYCL imports the Vulkan timeline semaphore.
3. Vulkan submits an empty queue submission that signals timeline value `N`.
4. SYCL enqueues `ext_oneapi_wait_external_semaphore(sem, N)`.
5. SYCL enqueues `ext_oneapi_signal_external_semaphore(sem, N + 1)`.
6. Vulkan submits an empty queue submission waiting on timeline value `N + 1` and signaling a final `VkFence`.
7. CPU waits for the final Vulkan fence with a timeout.

The repro intentionally avoids OIDN, external memory sharing, kernels, ray tracing, shaders, swapchain work, and command buffers. It requires the SYCL Level Zero backend; OpenCL does not support this external semaphore path.

### Findings

Date: 2026-07-02

The hang reproduces with a pure imported Vulkan timeline semaphore and a default non-immediate Level Zero SYCL queue. External memory imports and SYCL kernels are not required.

Confirmed behavior:

- Level Zero backend is required.
- Vulkan timeline semaphore import works.
- The initial timeline value must be signaled by a Vulkan GPU queue submission to match `RenderVulkan` behavior; host-side `vkSignalSemaphore` was intentionally not used.
- No Vulkan command buffers are required; empty queue submissions are enough.
- No external memory sharing is required.
- No SYCL kernel is required; `wait_external_semaphore -> signal_external_semaphore` is enough.
- A SYCL queue using `sycl::ext::intel::property::queue::immediate_command_list{}` completes successfully and hides the issue.
- With the default non-immediate Level Zero SYCL queue, the async SYCL signal does not advance the Vulkan timeline semaphore to `N + 1`, causing the final Vulkan fence wait to time out.

Relevant output:

```text
[SYCL] Backend: level_zero
[Frame] render_done=1, sycl_done=2
[Vulkan] submit signal timeline 1
[Diagnostics] timeline after signal submit: 1
[SYCL] wait_external_semaphore(1)
[SYCL] signal_external_semaphore(2)
[Diagnostics] timeline after SYCL async signal enqueue: 1
[Vulkan] submit wait timeline 2, signal final fence
[Diagnostics] timeline after cmd1 submit: 1

[FAIL] TIMEOUT waiting for final Vulkan fence.
       Vulkan queue likely stuck waiting for timeline value 2.
       Current timeline value: 1
```

### Local build command used for validation

From `D:\Code\ChameleonRT\build\vs`:

```cmd
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
.\dpcpp\src\bin\clang++.exe -fsycl -std=c++17 -O2 ^
  -I%VULKAN_SDK%\Include ^
  ..\..\minimal_reproducers\vulkan_oidn_timeline_semaphore_repro.cpp ^
  -o vulkan_oidn_timeline_semaphore_repro.exe ^
  -L%VULKAN_SDK%\Lib -lvulkan-1
```

Runtime command used:

```powershell
$env:PATH="D:\Code\ChameleonRT\build\vs\dpcpp\src\bin;D:\Code\ChameleonRT\build\vs\Release;$env:PATH"
$env:ONEAPI_DEVICE_SELECTOR="level_zero:gpu"
D:\Code\ChameleonRT\build\vs\vulkan_oidn_timeline_semaphore_repro.exe
```

### System information from reproducing machine

OS:

- Microsoft Windows 11 Enterprise
- Version: `10.0.26100`
- Build: `26100`
- Architecture: `64-bit`

System:

- Manufacturer: Intel Corporation
- Model: Alder Lake Client Platform
- System type: x64-based PC
- Physical memory: 34,093,285,376 bytes

CPU:

- 12th Gen Intel(R) Core(TM) i9-12900K
- Cores: 16
- Logical processors: 24

GPU / Windows driver:

- Adapter: Intel(R) Graphics d ci-neo-038964 DCH-D RI
- Driver version: `32.0.101.9999`
- Driver date: `2026-06-15`
- PNP device ID: `PCI\VEN_8086&DEV_56A0&SUBSYS_10208086&REV_08\6&20F6E9F4&0&00080008`

Vulkan-reported GPU information:

- Device name: Intel(R) Arc(TM) A770 Graphics
- Device type: discrete GPU
- Vendor ID: `0x8086`
- Device ID: `0x56a0`
- Vulkan API version: `1.4.351`
- Vulkan instance version: `1.4.350`
- Vulkan SDK used locally: `C:\VulkanSDK\1.4.341.1`
- Vulkan driver version: `101.9999`
- Vulkan driver ID: `DRIVER_ID_INTEL_PROPRIETARY_WINDOWS`
- Vulkan driver name: Intel Corporation
- Vulkan driver info: `101.9999`
- Conformance version: `1.4.0.0`
- Device UUID: `8680a056-0800-0000-0300-000000000000`
- Driver UUID: `33322e30-2e31-3031-2e39-393939000000`

SYCL runtime used for validation:

- Staged ChameleonRT DPC++ runtime: `D:\Code\ChameleonRT\build\vs\dpcpp\src\bin`
- SYCL backend: Level Zero (`ONEAPI_DEVICE_SELECTOR=level_zero:gpu`)
