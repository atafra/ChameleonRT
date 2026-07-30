# Repository Summary

This document tracks active repositories and their development branches.

| Repository | URL | Summary |
|------------|-----|---------|
| ChameleonRT | https://github.com/atafra/ChameleonRT | A multi-backend ray tracing renderer supporting DXR, OptiX, Vulkan, Metal, and Embree, with Intel OIDN denoising integration. |

## ChameleonRT — Active Branches

| Branch | Summary |
|--------|---------|
| `gliktor-oidn2-easybuild` | OIDN2 denoiser integration configured for EasyBuild package management. This branch handles build system integration and dependency management to enable streamlined compilation and deployment of OIDN2 within the ChameleonRT framework using EasyBuild's module system. |
| `gliktor-oidn2-semaphore` | OIDN2 integration with Semaphore CI/CD pipeline configuration. Establishes automated testing and continuous integration workflows for OIDN2 features, ensuring code quality and compatibility across different build configurations and maintaining integration stability. |
| `oidn2` | Main OIDN2 denoiser implementation branch serving as the primary development area. Integrates Intel's OIDN2 denoising library into ChameleonRT's rendering pipeline with ongoing work on performance optimization, API integration, and renderer compatibility. |
