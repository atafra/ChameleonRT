# Windows End-to-End Build (Visual Studio, OIDN SYCL + Vulkan + DXR)

This guide walks from clone to configure, build, and run on Windows.

Prerequisites:
- Visual Studio with Desktop C++ workload and a recent Windows SDK
- Vulkan SDK installed (set `VULKAN_SDK` if not detected automatically)
- A DXR-capable GPU/driver and Vulkan ray tracing support
- Git and CMake
- Internet access on first configure (SDL2/OIDN/toolchain downloads)

1. Clone and open a Developer PowerShell:

```powershell
git clone https://github.com/atafra/ChameleonRT.git
cd ChameleonRT
git checkout gliktor-oidn2-easybuild
```

2. Configure with Visual Studio generator and desired backends/features:

```powershell
cmake -S . -B build\vs -G "Visual Studio 18 2026" -A x64 -DENABLE_DXR=ON -DENABLE_VULKAN=ON -DENABLE_OIDN=ON -DOIDN_DEVICE=SYCL
```

If Vulkan is not found automatically, append:

```powershell
-DVULKAN_SDK="C:/VulkanSDK/<version>"
```

3. Build Release:

```powershell
cmake --build build\vs --config Release
```

4. Open in Visual Studio (optional IDE flow):

```powershell
start build\vs\chameleonrt.sln
```

In Visual Studio, set startup project to `chameleonrt`, choose `Release|x64`, and set command arguments to backend + scene.

5. Run from terminal (example):

```powershell
build\vs\Release\chameleonrt.exe vulkan <path-to-scene.gltf>
build\vs\Release\chameleonrt.exe dxr <path-to-scene.gltf>
```

Tip: first configure/build may take longer because external dependencies are fetched.

Tiny helper script (same flow):

```powershell
.\docs\windows_build_oidn_dxr_vulkan.ps1
# with a scene path:
.\docs\windows_build_oidn_dxr_vulkan.ps1 -ScenePath "C:/path/to/scene.gltf"
```
