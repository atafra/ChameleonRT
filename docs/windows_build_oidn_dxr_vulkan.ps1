param(
    [string]$BuildDir = "build/vs",
    [string]$Config = "Release",
    [string]$ScenePath = "Assets/Sponza/sponza.obj",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$Arch = "x64",
    [string[]]$ExtraCMakeArgs = @()
)

$ErrorActionPreference = "Stop"

# Run this from a clone of https://github.com/atafra/ChameleonRT.git
# on branch gliktor-oidn2-easybuild.

Write-Host "Configuring ChameleonRT (DXR + Vulkan + OIDN SYCL)..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G $Generator -A $Arch `
    -DENABLE_DXR=ON `
    -DENABLE_VULKAN=ON `
    -DENABLE_OIDN=ON `
    -DOIDN_DEVICE=SYCL `
    @ExtraCMakeArgs

Write-Host "Building $Config..." -ForegroundColor Cyan
cmake --build $BuildDir --config $Config

$exe = Join-Path $BuildDir "$Config/chameleonrt.exe"
if (-not (Test-Path $exe)) {
    throw "Build finished, but executable not found at: $exe"
}

Write-Host "Built executable: $exe" -ForegroundColor Green

if ($ScenePath) {
    if (-not (Test-Path $ScenePath)) {
        throw "Scene path not found: $ScenePath"
    }

    Write-Host "Running Vulkan backend..." -ForegroundColor Cyan
    & $exe vulkan $ScenePath

    Write-Host "Running DXR backend..." -ForegroundColor Cyan
    & $exe dxr $ScenePath
}
else {
    Write-Host "No scene path provided. Example run:" -ForegroundColor Yellow
    Write-Host "  $exe vulkan Assets/Sponza/sponza.obj"
    Write-Host "  $exe dxr Assets/Sponza/sponza.obj"
}
