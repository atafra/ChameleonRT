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

This reproducer tracks a Vulkan/OIDN timeline semaphore interop hang observed in the `RenderVulkan` backend when using OIDN timeline semaphore synchronization.

It mirrors the synchronization/submission sequence used by `backends/vulkan/render_vulkan.cpp` for `OIDNInteropMode::TimelineSemaphore`:

1. Vulkan submits a `render_cmd_buf` and signals timeline semaphore value `N`.
2. OIDN/SYCL enqueues `waitSemaphoreAsync(sem, N)`.
3. OIDN/SYCL enqueues `executeAsync()`.
4. OIDN/SYCL enqueues `signalSemaphoreAsync(sem, N + 1)`.
5. Vulkan submits a `tonemap_cmd_buf` waiting on timeline semaphore value `N + 1`.
6. CPU waits for the final Vulkan fence with a timeout.

The repro intentionally avoids ray tracing, scene setup, shaders, and swapchain work. It keeps the real OIDN filter, exportable Vulkan buffers, imported OIDN buffers, imported Vulkan timeline semaphore, and the same external queue-family release/acquire barriers used by the backend.

### Last reproduced result

Date: 2026-07-02

The reproducer built and ran successfully, but timed out waiting for the final Vulkan fence. The Vulkan timeline semaphore reached value `1` after the Vulkan render submission, but OIDN's async signal did not advance it to value `2`.

Relevant output:

```text
[Frame] slot=0, render_done=1, denoise_done=2
[Vulkan queue] vkQueueSubmit(render_cmd_buf, signal timeline render_done) end
[Diagnostics] timeline semaphore current value after render submit return: 1
[OIDN/SYCL] oidn_device.waitSemaphoreAsync(timeline, render_done) end
[OIDN/SYCL] oidn_filter.executeAsync() end
[OIDN/SYCL] oidn_device.signalSemaphoreAsync(timeline, denoise_done) end
[Diagnostics] timeline semaphore current value after OIDN async signal enqueue: 1
[Vulkan queue] vkQueueSubmit(tonemap_cmd_buf, wait timeline denoise_done, signal final fence) end
[Diagnostics] timeline semaphore current value after tonemap submit return: 1
[CPU] vkWaitForFences(final_fence, timeout) begin

[FAIL] TIMEOUT waiting for final Vulkan fence.
       This means tonemap_cmd_buf did not complete.
       Most likely the Vulkan queue is stuck waiting for timeline value 2.
       Current Vulkan timeline semaphore value: 1
       Expected values: render_done=1, denoise_done=2
```

### Local build command used for validation

From `D:\Code\ChameleonRT`:

```cmd
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++14 /EHsc /O2 ^
  /Ibuild\vs\oidn\install\include ^
  /I%VULKAN_SDK%\Include ^
  minimal_reproducers\vulkan_oidn_timeline_semaphore_repro.cpp ^
  /Fe:build\vs\vulkan_oidn_timeline_semaphore_repro.exe ^
  /link /LIBPATH:build\vs\oidn\install\lib /LIBPATH:%VULKAN_SDK%\Lib ^
  OpenImageDenoise.lib vulkan-1.lib
```

Runtime DLL path used:

```powershell
$env:PATH="D:\Code\ChameleonRT\build\vs\oidn\install\bin;D:\Code\ChameleonRT\build\vs\Release;D:\Code\ChameleonRT\build\vs\Debug;$env:PATH"
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

OIDN build used for validation:

- `OpenImageDenoise.dll` product/file version: `2.5.1-devel`
- `OIDN_VERSION_STRING`: `2.5.1-devel`
- Installed path used: `D:\Code\ChameleonRT\build\vs\oidn\install`
