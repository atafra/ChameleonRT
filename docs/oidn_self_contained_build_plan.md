# Implementation Plan — Self-Contained OIDN Build for ChameleonRT

Status: **DRAFT — for iteration**
Branch: `gliktor-oidn2-easybuild`
Goal: Make ChameleonRT download the OIDN **source** and **build it locally** on
demand from CMake — together with its DPC++/SYCL toolchain dependencies — instead
of requiring a pre-built, externally-discoverable OIDN install. Mirror the
approach already used by RPTR (summarized in `process_oidn2_cmake_dependencies.md`).

Decision (this revision): **build OIDN from source only — no prebuilt binary
downloads.** A lighter/prebuilt path may be revisited later (see §8).

---

## 1. Current state in ChameleonRT

### 1.1 How OIDN is wired today

| File | Lines | What it does |
|---|---|---|
| `util/CMakeLists.txt` | 72–77 | `option(ENABLE_OIDN ...)` ? `find_package(OpenImageDenoise REQUIRED)` ? `target_compile_definitions(util PUBLIC ENABLE_OIDN)` + `target_link_libraries(util PUBLIC OpenImageDenoise)` |
| `backends/dxr/CMakeLists.txt` | 24–25 | Appends `ENABLE_OIDN=1` to HLSL shader defines |
| `backends/vulkan/CMakeLists.txt` | 16–17 | Appends `ENABLE_OIDN=1` to GLSL shader defines |
| `backends/metal/CMakeLists.txt` | 31–32 | Appends `ENABLE_OIDN=1` to Metal shader defines |
| `backends/{dxr,vulkan,metal}/render_*.h` | top | `#ifdef ENABLE_OIDN` ? `#include <OpenImageDenoise/oidn.hpp>` |

Key consequence: **`util` is the single integration point.** Because
`ENABLE_OIDN` (compile def) and the `OpenImageDenoise` (link target, with its
include dirs) are both `PUBLIC` on `util`, every backend that links `util`
inherits them transitively. The backend `CMakeLists.txt` files only add the
*shader* define and do **not** call `find_package` or link OIDN themselves.

> This means we can make the build self-contained by changing **`util/CMakeLists.txt`
> + one new `cmake/oidn.cmake` helper**, with no changes required to the backend
> `CMakeLists.txt` files (only validation, see §5).

### 1.2 Existing external-dependency convention

ChameleonRT does **not** use `FetchContent_MakeAvailable`. It uses
`ExternalProject_Add` wrapped in an INTERFACE target:

- `cmake/glm.cmake` — `ExternalProject_Add(glm_ext ...)` ? `add_library(glm INTERFACE)` ? `add_dependencies(glm glm_ext)` ? `target_include_directories(glm INTERFACE ...)`.
- `cmake/rapidjson.cmake` — same shape.
- `cmake/package.cmake` — `crt_add_packaged_dependency()` + `crt_install_namelink()` copy/symlink/install shared libs into `bin/`.

The top-level `CMakeLists.txt` pulls these in with `include(cmake/glm.cmake)` etc.
**We will follow this same convention** for OIDN for consistency.

### 1.3 Which OIDN APIs ChameleonRT actually uses (drives toolchain choice)

| Backend | OIDN device creation | Interop surface used |
|---|---|---|
| DXR (`render_dxr.cpp`) | `oidn::newDevice(oidn::LUID{...})` | External **memory** only — `ExternalMemoryTypeFlag::OpaqueWin32` |
| Vulkan (`render_vulkan.cpp`) | `oidn::newDevice(uuid)` | External **memory** (`OpaqueFD` / `DMABuf` / `OpaqueWin32`) **and external semaphores**: `oidn_device.newSemaphore(...)`, `waitSemaphoreAsync`, `signalSemaphoreAsync`, `oidn::SemaphoreRef` (render_vulkan.cpp ~467–571, 1132–1338; render_vulkan.h 99–103) |
| Metal (`render_metal.mm`) | `oidn::newMetalDevice(...)` | OIDN-managed Metal buffers |

**Critical finding:** the Vulkan backend depends on OIDN's external-**semaphore**
interop (`SemaphoreRef`, `waitSemaphoreAsync`, `signalSemaphoreAsync`). Per the
RPTR notes, that API is **not in the public OIDN release** and lives on the
internal `aafra/semaphore` branch. Therefore, to preserve ChameleonRT's existing
Vulkan interop modes (`timeline_semaphore`, `binary_semaphore`), we must
**build OIDN from source from that branch** — a prebuilt binary release cannot
work here. This is why the plan commits to a from-source build (§2).

Because OIDN's SYCL device must be compiled with the Intel **DPC++/SYCL**
compiler (not MSVC/GCC), this build is done as a *separate* `ExternalProject`
with its own compiler — exactly as RPTR does. (A plain
`FetchContent_MakeAvailable`/`add_subdirectory` cannot switch compilers
mid-build, so it is not suitable for the SYCL device; see §7.)

---

## 2. Chosen approach: build OIDN from source (RPTR parity)

ChameleonRT will **download the OIDN source and build it locally**, on demand, as
part of the CMake configure/build — **no prebuilt binary archives**. This mirrors
RPTR and is required for feature parity: the Vulkan backend uses OIDN's external-
**semaphore** interop (`SemaphoreRef`, `waitSemaphoreAsync`,
`signalSemaphoreAsync`), which only exists on the `aafra/semaphore` branch and is
absent from every public OIDN binary release (§1.3).

| Aspect | Decision |
|---|---|
| OIDN source | `RenderKit/oidn`, **tracking the `aafra/semaphore` branch tip**, submodule `weights`, fetched via `ExternalProject_Add` |
| Device backend | **Exactly one device per build**, chosen by `OIDN_DEVICE` (one of `SYCL`/`CUDA`/`HIP`/`METAL`/`CPU`). **`SYCL` is the default** (forced to `METAL` on Apple) so the common case needs no manual configuration. We never build more than one device for a given build. |
| Toolchain (SYCL only) | Intel DPC++/SYCL + Level Zero, downloaded on demand at **pinned versions** (§4.3–4.4). Only fetched when `OIDN_DEVICE=SYCL`. |
| Build mechanism | `ExternalProject_Add` (lets OIDN use its own compiler), consistent with the existing `glm`/`rapidjson` pattern |
| Escape hatch | `OIDN_PREBUILT_DIR` (path to an existing OIDN install/build) skips the download+build entirely and just wires the target — useful for CI caching and developers who already have a suitable OIDN (§4.8). |

The default path is **a single, well-tested SYCL build** — no manual configuration
needed for the most common (Intel) case. Selecting another vendor is a matter of
setting `OIDN_DEVICE=CUDA`/`HIP`/`CPU`; CUDA/HIP require that vendor's toolkit
(CUDA Toolkit / ROCm) to be installed, which the plan does **not** auto-download.
There is still **no prebuilt *binary* download**; the `OIDN_PREBUILT_DIR` escape
hatch points at a build the user already produced locally.

**Toolchain versions are pinned to exactly what RPTR uses** — DPC++/SYCL
`intel/llvm` **`v6.2.1`** and Level Zero **`v1.29.0`**. The external-semaphore
interop on the `aafra/semaphore` branch is experimental and version-sensitive;
mismatched DPC++/Level Zero versions can silently break semaphore sharing, so
these must not drift from RPTR (§4.3–4.4).

---

## 3. New / changed files overview

```text
CMakeLists.txt                 (modify) define ENABLE_OIDN + OIDN_DEVICE + OIDN_PREBUILT_DIR; include cmake/oidn.cmake; add validation
cmake/oidn.cmake               (NEW)    OIDN source build orchestrator + INTERFACE target
cmake/dpcpp.cmake              (NEW)    download Intel DPC++/SYCL toolchain at pinned v6.2.1 (SYCL only)
cmake/level_zero.cmake         (NEW)    download Level Zero headers/runtime at pinned v1.29.0 (SYCL only)
cmake/ninja.cmake              (NEW)    locate or fetch the Ninja generator
cmake/oidn_prebuilt.cmake      (NEW)    wire OpenImageDenoise from OIDN_PREBUILT_DIR (escape hatch)
cmake/package.cmake            (modify) helper to stage OIDN + DPC++ runtime libs
util/CMakeLists.txt            (modify) drop find_package; consume new target
backends/*/CMakeLists.txt      (no functional change; verify only)
README.md                      (modify) remove manual OIDN build step; document -DENABLE_OIDN=ON and OIDN_DEVICE
```

The OIDN `option(...)` definition currently lives in `util/CMakeLists.txt`. We
will **move it to the top-level `CMakeLists.txt`** so the option and its
validation are defined before `add_subdirectory(util)` and
`add_subdirectory(backends)` (matching how `REPORT_RAY_STATS` and the
`cmake/*.cmake` includes are handled at the top level).

---

## 4. Detailed design

### 4.1 Top-level `CMakeLists.txt`

Add near the other dependency includes (after `include(cmake/package.cmake)`):

```cmake
# OIDN denoiser: downloaded + built from source on demand (see cmake/oidn.cmake)
option(ENABLE_OIDN "Build with Intel Open Image Denoise support." OFF)

# Single device backend per build. SYCL is the default so the common (Intel)
# case needs no manual configuration; we never build more than one device.
# CUDA/HIP require the matching vendor toolkit (CUDA Toolkit / ROCm) to be
# installed and are NOT auto-downloaded. On Apple this is forced to METAL.
set(OIDN_DEVICE "SYCL" CACHE STRING
    "OIDN device backend to build (exactly one): SYCL, CUDA, HIP, METAL, CPU")
set_property(CACHE OIDN_DEVICE PROPERTY STRINGS SYCL CUDA HIP METAL CPU)

# Escape hatch: point at an existing OIDN install/build to skip download+build
# (CI cache or a pre-existing local build). Empty = build from source.
set(OIDN_PREBUILT_DIR "" CACHE PATH
    "Path to an existing OIDN install; if set, skip downloading/building OIDN")

if (ENABLE_OIDN)
    include(cmake/oidn.cmake)   # defines INTERFACE target `OpenImageDenoise`
endif()
```

A drop-down (`STRINGS` property) keeps the GUI usable; `cmake/oidn.cmake`
validates the value and applies the Apple override (§4.2, §6).

This must appear **before** `add_subdirectory(util)` and
`add_subdirectory(backends)` so the `OpenImageDenoise` target exists when those
subdirectories configure.

### 4.2 `cmake/oidn.cmake` (new — the core)

Responsibilities:
1. **Escape hatch first:** if `OIDN_PREBUILT_DIR` is set, skip all
   downloading/building and create the `OpenImageDenoise` INTERFACE target from
   that directory (§4.8), then return.
2. Verify a usable generator: ensure **Ninja** is available for the OIDN
   sub-build (find it, or fetch it — §4.7).
3. When `OIDN_DEVICE_SYCL` is ON, ensure the DPC++/SYCL toolchain + Level Zero
   are available (`include(cmake/dpcpp.cmake)`, `include(cmake/level_zero.cmake)`).
   These are skipped entirely for non-SYCL builds.
4. Download OIDN source (`ExternalProject_Add` with `GIT_REPOSITORY` +
   `GIT_TAG aafra/semaphore` **tracking the branch tip** + the `weights`
   submodule), configure it with the **selected** `OIDN_DEVICE_*` options, and
   build it. Use the DPC++ `clang` as the compiler only when SYCL is ON.
5. Wrap the result in an INTERFACE target named **`OpenImageDenoise`** (same name
   `util` already links) that exposes the include dir + the produced import
   libraries, and `add_dependencies` it on the build step.
6. Record the produced runtime shared libraries in a cache list
   (`OIDN_RUNTIME_LIBRARIES`) for staging/installation (see §4.5).

Sketch (mirrors `glm.cmake` shape + RPTR steps 1–6; paths/flags to be finalized
during implementation):

```cmake
include(ExternalProject)

# (1) Escape hatch — use an existing OIDN, skip download+build.
if (OIDN_PREBUILT_DIR)
    include(cmake/oidn_prebuilt.cmake)   # defines OpenImageDenoise from OIDN_PREBUILT_DIR (§4.8)
    return()
endif()

# (2) Ninja is required for the OIDN sub-build (see §4.7).
include(cmake/ninja.cmake)               # sets OIDN_NINJA_EXECUTABLE

# (3) Resolve the single device backend. SYCL default; forced to METAL on Apple.
set(OIDN_DEVICE_RESOLVED "${OIDN_DEVICE}")
if (APPLE AND NOT OIDN_DEVICE_RESOLVED STREQUAL "CPU")
    set(OIDN_DEVICE_RESOLVED "METAL")
endif()

set(_oidn_valid SYCL CUDA HIP METAL CPU)
if (NOT OIDN_DEVICE_RESOLVED IN_LIST _oidn_valid)
    message(FATAL_ERROR "OIDN_DEVICE='${OIDN_DEVICE}' is invalid; choose one of: ${_oidn_valid}")
endif()

# Turn every device OFF, then enable just the selected one (single-device build).
set(OIDN_DEVICE_ARGS
    -DOIDN_DEVICE_CPU=OFF
    -DOIDN_DEVICE_SYCL=OFF
    -DOIDN_DEVICE_CUDA=OFF
    -DOIDN_DEVICE_HIP=OFF
    -DOIDN_DEVICE_METAL=OFF
    -DOIDN_DEVICE_${OIDN_DEVICE_RESOLVED}=ON)

# (4) Only the SYCL device needs the whole library compiled with DPC++ clang +
#     Level Zero; pull those in (at pinned versions) only for SYCL.
set(OIDN_COMPILER_ARGS "")
set(OIDN_TOOLCHAIN_DEPS "")
if (OIDN_DEVICE_RESOLVED STREQUAL "SYCL")
    include(cmake/dpcpp.cmake)       # sets DPCPP_CLANG / DPCPP_CLANGXX, DPCPP_BIN_DIR (pinned v6.2.1)
    include(cmake/level_zero.cmake)  # sets LEVEL_ZERO_ROOT (pinned v1.29.0)
    list(APPEND OIDN_COMPILER_ARGS
        -DCMAKE_C_COMPILER=${DPCPP_CLANG}
        -DCMAKE_CXX_COMPILER=${DPCPP_CLANGXX}
        -DLEVEL_ZERO_ROOT=${LEVEL_ZERO_ROOT})
    set(OIDN_TOOLCHAIN_DEPS dpcpp_ext level_zero_ext)
endif()

set(OIDN_ROOT        ${CMAKE_CURRENT_BINARY_DIR}/oidn)
set(OIDN_INSTALL_DIR ${OIDN_ROOT}/install)

ExternalProject_Add(oidn_ext
    PREFIX oidn DOWNLOAD_DIR oidn STAMP_DIR oidn/stamp
    SOURCE_DIR oidn/src BINARY_DIR oidn/build
    DEPENDS ${OIDN_TOOLCHAIN_DEPS}
    GIT_REPOSITORY "https://github.com/RenderKit/oidn.git"
    GIT_TAG        "origin/aafra/semaphore"   # branch ref ? tracks the tip
    GIT_SUBMODULES "weights"
    GIT_SHALLOW    TRUE
    UPDATE_DISCONNECTED OFF                   # re-fetch the branch tip on reconfigure
    CMAKE_GENERATOR "Ninja"
    # For OIDN_DEVICE=SYCL, prepend the DPC++ bin dir to PATH for configure+build
    # (see §4.3), e.g. via `${CMAKE_COMMAND} -E env` wrappers on the steps.
    CMAKE_ARGS
        -DCMAKE_MAKE_PROGRAM=${OIDN_NINJA_EXECUTABLE}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${OIDN_INSTALL_DIR}
        -DOIDN_APPS=OFF
        -DOIDN_DEVICE_SYCL_AOT=OFF
        ${OIDN_DEVICE_ARGS}
        ${OIDN_COMPILER_ARGS}
    BUILD_ALWAYS OFF)

set(OIDN_INCLUDE_DIR ${OIDN_INSTALL_DIR}/include)
set(OIDN_IMPORT_LIB  ${OIDN_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}OpenImageDenoise${CMAKE_IMPORT_LIBRARY_SUFFIX})
file(GLOB OIDN_RUNTIME_LIBRARIES "${OIDN_INSTALL_DIR}/bin/*${CMAKE_SHARED_LIBRARY_SUFFIX}*")  # + DPC++ runtime, see §4.5

# include dir must exist at configure time for INTERFACE usage requirements
file(MAKE_DIRECTORY ${OIDN_INCLUDE_DIR})

add_library(OpenImageDenoise INTERFACE)
add_dependencies(OpenImageDenoise oidn_ext)
target_include_directories(OpenImageDenoise INTERFACE ${OIDN_INCLUDE_DIR})
target_link_libraries(OpenImageDenoise INTERFACE ${OIDN_IMPORT_LIB})

set(OIDN_RUNTIME_LIBRARIES "${OIDN_RUNTIME_LIBRARIES}" CACHE INTERNAL "OIDN runtime libs to stage")
```

> Notes / TODO during implementation:
> - **Branch-tip tracking (decided):** `GIT_TAG origin/aafra/semaphore` keeps the
>   build on the latest commit of that branch. The ExternalProject update step
>   re-fetches on reconfigure (keep `UPDATE_DISCONNECTED OFF`). Trade-off: builds
>   are not bit-for-bit reproducible across time; this is accepted for now. To
>   force a fresh tip, re-run the `oidn_ext-update` step (or wipe the stamp).
> - Confirm the import-library vs runtime-library naming on each platform
>   (Windows: `OpenImageDenoise.lib` + `OpenImageDenoise.dll`; Linux:
>   `libOpenImageDenoise.so` is both). `${CMAKE_IMPORT_LIBRARY_SUFFIX}` is empty
>   on Linux — handle both.
> - The `file(GLOB ...)` at configure time will be empty before the first build;
>   prefer a fixed, known list of expected runtime libs (see §4.5) rather than a
>   glob, or move staging to a `POST_BUILD`/`ExternalProject_Add_Step`.
> - The produced device runtime lib matches the single selected `OIDN_DEVICE`
>   (e.g. `OpenImageDenoise_device_sycl` / `_cuda` / `_hip` / `_metal`). Stage the
>   one that was built (§4.5).

### 4.3 `cmake/dpcpp.cmake` (new — only used when `OIDN_DEVICE=SYCL`)

Download the Intel DPC++/SYCL release archive and expose the `clang`/`clang++`
paths + bin dir. Mirrors RPTR's `setup_dpcpp()` / `setup_sycl_environment()`.
This file is only `include()`d when `OIDN_DEVICE=SYCL`, so other device builds
(CUDA/HIP/CPU/Metal) never download the DPC++ toolchain.

> **Pinned version (hard requirement): `intel/llvm` `v6.2.1` — the exact release
> RPTR uses.** The external-semaphore interop on the `aafra/semaphore` OIDN branch
> is experimental and tightly coupled to this toolchain; a different DPC++ build
> can make semaphore sharing silently fail. Do **not** bump this without
> re-validating interop against RPTR. Pin the archive `URL` + `URL_HASH`.

```cmake
include(ExternalProject)   # or FetchContent for a pure download

# Pinned to match RPTR exactly — see note above. Do not change casually.
set(DPCPP_VERSION "v6.2.1")
if (WIN32)
    set(DPCPP_URL  "https://github.com/intel/llvm/releases/download/${DPCPP_VERSION}/sycl_windows.tar.gz")
    set(DPCPP_HASH "SHA256=...")   # pin
else()
    set(DPCPP_URL  "https://github.com/intel/llvm/releases/download/${DPCPP_VERSION}/sycl_linux.tar.gz")
    set(DPCPP_HASH "SHA256=...")   # pin
endif()

# Download + extract only (no build). FetchContent_Populate or ExternalProject
# with empty configure/build/install, like glm.cmake. Use URL_HASH for integrity.
...
set(DPCPP_BIN_DIR "${dpcpp_SOURCE_DIR}/bin")
set(DPCPP_CLANG   "${DPCPP_BIN_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX}")
set(DPCPP_CLANGXX "${DPCPP_BIN_DIR}/clang++${CMAKE_EXECUTABLE_SUFFIX}")
# Record DPC++ runtime libs (sycl8, ur_loader, ur_win_proxy_loader,
# ur_adapter_level_zero, ...) for staging in §4.5.
```

The DPC++ toolchain must be on `PATH` (Windows) / `LD_LIBRARY_PATH` (Linux)
*while OIDN configures and builds*, so the OIDN `ExternalProject_Add` step's
environment is augmented (e.g. wrap the configure/build with
`${CMAKE_COMMAND} -E env "PATH=${DPCPP_BIN_DIR};$ENV{PATH}" ...`).

> Name the download target **`dpcpp_ext`** so the `DEPENDS` /
> `OIDN_TOOLCHAIN_DEPS` reference in §4.2 resolves (or drop it from
> `OIDN_TOOLCHAIN_DEPS` if you use `FetchContent` instead of `ExternalProject`).

### 4.4 `cmake/level_zero.cmake` (new — only used when `OIDN_DEVICE=SYCL`)

Download Level Zero release (headers + loader) and expose `LEVEL_ZERO_ROOT`.
Like `dpcpp.cmake`, only pulled in for SYCL builds.

> **Pinned version (hard requirement): Level Zero `v1.29.0` — the exact release
> RPTR uses.** Same reasoning as DPC++: the SYCL Level-Zero adapter version must
> match what the experimental semaphore-sharing path was validated against. Pin
> the archive `URL` + `URL_HASH`; do not bump independently of RPTR.

```cmake
set(LEVEL_ZERO_VERSION "v1.29.0")
set(LEVEL_ZERO_URL  "https://github.com/oneapi-src/level-zero/releases/download/${LEVEL_ZERO_VERSION}/...")
set(LEVEL_ZERO_HASH "SHA256=...")   # pin
# download + extract; set(LEVEL_ZERO_ROOT ...)
```

> Name the download target **`level_zero_ext`** to match the
> `OIDN_TOOLCHAIN_DEPS` reference in §4.2 (same caveat as `dpcpp_ext`).

> **Keep these versions in lock-step with RPTR.** If RPTR updates its DPC++ /
> Level Zero pins, update `DPCPP_VERSION` / `LEVEL_ZERO_VERSION` here to the same
> values and re-validate semaphore interop. Consider a short comment in each file
> pointing back to the RPTR `ext/CMakeLists.txt` revision the pins came from.

### 4.5 Runtime library redistribution (`cmake/package.cmake`)

ChameleonRT loads backends as plugin modules next to `chameleonrt`. The OIDN and
DPC++ runtime shared libraries must be copied beside the executable and
installed into `bin/`. Reuse the existing redistribution style.

Runtime libraries to ship (from RPTR doc, SYCL build):

```text
OpenImageDenoise(.dll/.so)
OpenImageDenoise_core
OpenImageDenoise_device_sycl
sycl8, ur_loader, ur_win_proxy_loader, ur_adapter_level_zero   (DPC++ runtime)
```

Plan:
- Add a helper `crt_add_packaged_files(<list>)` to `package.cmake` that, given a
  list of resolved shared-library paths, copies them into `${PROJECT_BINARY_DIR}`
  via `add_custom_command`/`add_custom_target` and `install(PROGRAMS ... DESTINATION bin)`.
  (The existing `crt_add_packaged_dependency` only handles IMPORTED targets, so a
  file-list variant is cleaner here.)
- Because these files are produced by `oidn_ext`/the toolchain ExternalProjects,
  the copy must depend on `oidn_ext` and run after it. Implement as an
  `ExternalProject_Add_Step(oidn_ext stage_runtime ...)` or a custom target that
  `add_dependencies(... oidn_ext)`.
- Wire it from `cmake/oidn.cmake` after the `OpenImageDenoise` target is defined,
  using `OIDN_RUNTIME_LIBRARIES` (+ the DPC++ runtime list from `dpcpp.cmake`).

### 4.6 `util/CMakeLists.txt`

Replace the `find_package`-based block (lines 72–77) so it consumes the target
created by `cmake/oidn.cmake`. The `option(...)` moves to the top level (§4.1).

```cmake
# OIDN is provided by cmake/oidn.cmake (downloaded + built on demand) when
# ENABLE_OIDN is set at the top level.
if (ENABLE_OIDN)
    target_compile_definitions(util PUBLIC ENABLE_OIDN)
    target_link_libraries(util PUBLIC OpenImageDenoise)
endif()
```

No other `util` changes are needed; the PUBLIC link/def continue to propagate to
all backends exactly as today.

### 4.7 `cmake/ninja.cmake` (new — generator check / fetch)

The OIDN sub-build uses the **Ninja** generator (the DPC++ `clang` compiler is
not well supported by the Visual Studio generator on Windows). Resolve Ninja in
this order and expose `OIDN_NINJA_EXECUTABLE`:

1. `find_program(OIDN_NINJA_EXECUTABLE ninja)` — found on PATH or beside the
   toolchain. (The Visual Studio "C++ CMake tools" component ships a
   `ninja.exe`, so VS users usually already have it.)
2. If not found, **fetch** the small prebuilt Ninja binary release (e.g.
   `ninja-build/ninja` `ninja-win.zip` / `ninja-linux.zip` / `ninja-mac.zip`) via
   `FetchContent`/`ExternalProject` and point `OIDN_NINJA_EXECUTABLE` at it.
3. If both fail, `message(FATAL_ERROR ...)` with guidance to install Ninja.

This is the only "fetch a build tool" step; Ninja is a single self-contained
binary so the download is tiny. `OIDN_NINJA_EXECUTABLE` is passed to the OIDN
`ExternalProject` as `-DCMAKE_MAKE_PROGRAM=` (§4.2).

### 4.8 OIDN escape hatch (`OIDN_PREBUILT_DIR`, `cmake/oidn_prebuilt.cmake`)

To support CI caching and developers who already have a suitable OIDN build,
setting `-DOIDN_PREBUILT_DIR=<path>` skips **all** downloading/building. The
helper just creates the same `OpenImageDenoise` INTERFACE target from that
directory:

```cmake
# cmake/oidn_prebuilt.cmake — included from oidn.cmake when OIDN_PREBUILT_DIR set
set(OIDN_INCLUDE_DIR ${OIDN_PREBUILT_DIR}/include)
set(OIDN_IMPORT_LIB  ${OIDN_PREBUILT_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}OpenImageDenoise${CMAKE_IMPORT_LIBRARY_SUFFIX})
file(GLOB OIDN_RUNTIME_LIBRARIES "${OIDN_PREBUILT_DIR}/bin/*${CMAKE_SHARED_LIBRARY_SUFFIX}*")

if (NOT EXISTS "${OIDN_INCLUDE_DIR}/OpenImageDenoise/oidn.hpp")
    message(FATAL_ERROR "OIDN_PREBUILT_DIR='${OIDN_PREBUILT_DIR}' does not look like an OIDN install")
endif()

add_library(OpenImageDenoise INTERFACE)   # no oidn_ext dependency — already built
target_include_directories(OpenImageDenoise INTERFACE ${OIDN_INCLUDE_DIR})
target_link_libraries(OpenImageDenoise INTERFACE ${OIDN_IMPORT_LIB})
set(OIDN_RUNTIME_LIBRARIES "${OIDN_RUNTIME_LIBRARIES}" CACHE INTERNAL "OIDN runtime libs to stage")
```

> This is **not** a binary *download* — the user supplies a path to a build they
> already have (e.g. a previous `oidn_ext` output cached by CI, or a system
> install). It is the "system escape hatch" requested in the open questions.

This complements **build-tree caching** for CI: because the OIDN `ExternalProject`
lives under `${CMAKE_CURRENT_BINARY_DIR}/oidn`, caching that directory (or the
final `oidn/install`) across CI runs avoids rebuilding OIDN every time; a cached
`oidn/install` can also be fed back in via `OIDN_PREBUILT_DIR`.

### 4.9 Backend `CMakeLists.txt` files

**No functional changes required.** They already only append the `ENABLE_OIDN=1`
shader define and inherit the OIDN headers/libs via `util`. Action item: just
re-verify each still configures with the new target (esp. include-dir
propagation), and that `dxr`/`vulkan` headers compile against the
`aafra/semaphore` OIDN API.

---

## 5. Option / validation matrix

Add validation in the top-level `CMakeLists.txt` (after the options are defined),
adapting RPTR's rules to ChameleonRT's actual options:

| Rule | Rationale |
|---|---|
| `ENABLE_OIDN` (and `OIDN_PREBUILT_DIR` not set) ? require Git, and Ninja (find or fetch, §4.7) | the OIDN source fetch + `ExternalProject` need them |
| `OIDN_DEVICE` must be one of `SYCL`/`CUDA`/`HIP`/`METAL`/`CPU` | exactly one device per build; reject typos with `FATAL_ERROR` (§4.2) |
| `OIDN_DEVICE=CUDA` ? a CUDA Toolkit must be discoverable | OIDN builds the CUDA device with `nvcc`; not auto-downloaded |
| `OIDN_DEVICE=HIP` ? ROCm/HIP must be discoverable | OIDN builds the HIP device with `hipcc`; not auto-downloaded |
| `OIDN_DEVICE=SYCL` ? fetch DPC++ `v6.2.1` + Level Zero `v1.29.0` (pinned) | experimental semaphore interop is version-sensitive (§4.3–4.4) |
| On macOS, `OIDN_DEVICE` resolves to `METAL` (unless `CPU`) | DPC++/SYCL is not used for the Metal device (§6) |
| At least one of `ENABLE_DXR` / `ENABLE_VULKAN` / `ENABLE_METAL` should be ON when `ENABLE_OIDN` | OIDN is consumed only by GPU backends; warn otherwise |
| `OIDN_PREBUILT_DIR` set ? skip the Git/Ninja/toolchain requirements | nothing is built from source in that case (§4.8) |

> Unlike RPTR there is no `ENABLE_POST_PROCESSING` / `ENABLE_AOV_BUFFERS` /
> `IHV_COMPATIBLE` in ChameleonRT, so those RPTR rules do **not** apply.

---

## 6. Cross-platform considerations

| Concern | Windows | Linux | macOS |
|---|---|---|---|
| External memory handle | `OpaqueWin32` (DXR + Vulkan) | `OpaqueFD` / `DMABuf` (Vulkan) | n/a (Metal device) |
| Default `OIDN_DEVICE` | `SYCL` (Level Zero) | `SYCL` (Level Zero) | `METAL` (forced) |
| Other valid `OIDN_DEVICE` | `CUDA`, `HIP`, `CPU` | `CUDA`, `HIP`, `CPU` | `CPU` |
| DPC++/SYCL toolchain fetch | only when `OIDN_DEVICE=SYCL` | only when `OIDN_DEVICE=SYCL` | never (Metal device) |
| Toolchain versions | DPC++ `v6.2.1`, Level Zero `v1.29.0` (pinned) | DPC++ `v6.2.1`, Level Zero `v1.29.0` (pinned) | n/a |
| Vendor toolkit prerequisite | CUDA Toolkit / ROCm if that device chosen | CUDA Toolkit / ROCm if that device chosen | Xcode/Metal |
| Import vs runtime lib | `.lib` + `.dll` | `.so` (single) | `.dylib` |

Action items:
- Gate the SYCL toolchain fetch (`cmake/dpcpp.cmake` + `cmake/level_zero.cmake`)
  on `OIDN_DEVICE=SYCL` (resolved value, which is never SYCL on Apple).
- On macOS, resolve `OIDN_DEVICE` to `METAL` (unless `CPU`); no DPC++.
- For CUDA/HIP, do **not** auto-download the toolkit; pass the resolved OIDN
  device flag through and let OIDN locate `nvcc`/`hipcc`. Document the prerequisite.
- Stage the single device-specific runtime lib that actually got built (§4.5).

---

## 7. Why not plain `FetchContent_MakeAvailable`?

The request mentions "download using FetchContent." Clarification on what's
feasible:

- `FetchContent` is fine for **downloading** archives/sources (it is essentially
  a configure-time `ExternalProject` download). We can use it for the DPC++ and
  Level Zero archives if preferred over `ExternalProject_Add`.
- `FetchContent_MakeAvailable(oidn)` would `add_subdirectory(oidn)`, compiling
  OIDN **with ChameleonRT's main compiler (MSVC/GCC)**. The OIDN **SYCL device
  cannot** be built that way — it needs Intel DPC++ `clang`. You cannot switch
  the compiler for a subdirectory mid-configure. Hence OIDN-SYCL must be a
  *separate* `ExternalProject` with its own `CMAKE_CXX_COMPILER`, which is
  exactly what RPTR does and what §4.2 specifies.
- ChameleonRT's existing deps (`glm`, `rapidjson`) already standardize on
  `ExternalProject_Add`. Staying with that keeps the codebase consistent.

**Conclusion:** use `ExternalProject_Add` for the OIDN source build (and
optionally `FetchContent` for the pure toolchain downloads). Functionally both
"download on demand and build with the project," satisfying the goal.

---

## 8. Resolved decisions

These were previously open; they are now settled and reflected in §§2–7:

1. **OIDN ref — track the branch tip.** Use `GIT_TAG origin/aafra/semaphore`
   (a branch ref) and let the ExternalProject update step follow the tip
   (`UPDATE_DISCONNECTED OFF`). Reproducibility-over-time is traded away
   intentionally (§4.2).
2. **GPU vendor coverage — one device per build, default SYCL.** A single
   `OIDN_DEVICE` enum selects exactly one of `SYCL`/`CUDA`/`HIP`/`METAL`/`CPU`
   (default `SYCL`, forced `METAL` on Apple). We never build more than one device.
   CUDA/HIP require the user's CUDA Toolkit / ROCm and are not auto-downloaded
   (§4.1, §4.2, §5, §6). Choosing `HIP` also gives AMD users a path around the
   `render_vulkan.cpp` AMD-UUID FIXME.
3. **DPC++ / Level Zero versions — pinned to RPTR.** The SYCL toolchain is pinned
   to **DPC++ `intel/llvm` `v6.2.1`** and **Level Zero `v1.29.0`** (the exact
   versions RPTR uses) with archive hashes. The experimental semaphore-sharing
   interop is version-sensitive, so these must stay in lock-step with RPTR and
   may not be bumped without re-validating interop (§4.3–4.4).
4. **macOS — confirmed.** `OIDN_DEVICE` resolves to `METAL` (no DPC++).
   *Note: not yet testable on macOS hardware; revisit when available.*
5. **Build time / CI — include the escape hatch + caching.** Added
   `OIDN_PREBUILT_DIR` to skip download+build and reuse an existing OIDN, plus a
   CI build-tree caching note (§4.8). This is a local path, not a binary download.
6. **Ninja — check then fetch.** `cmake/ninja.cmake` finds Ninja (VS ships it),
   otherwise fetches the small prebuilt binary, otherwise errors (§4.7).
7. **Doc location — moved.** This document now lives at
   `docs/oidn_self_contained_build_plan.md`.

## 8a. Remaining open questions (non-blocking)

- **Exact DPC++ runtime lib names to stage:** the *versions* are pinned (DPC++
  `v6.2.1`, Level Zero `v1.29.0`, §4.3–4.4), but confirm the precise runtime lib
  filenames that ship with those releases (`sycl8` vs `sycl`, the full set of
  `ur_*` libs) on each OS for staging (§4.5). Cross-check against RPTR's
  `EXT_RUNTIME_LIBRARIES` list.
- **Archive hashes:** fill in the `URL_HASH` values for the pinned DPC++ /
  Level Zero archives (and confirm the Level Zero asset filename) (§4.3–4.4).

---

## 9. Phased rollout / checklist

**Phase 0 — CMake rewiring (no behavior change yet):**
- [ ] Move `option(ENABLE_OIDN ...)` to the top-level `CMakeLists.txt` and add the
      `OIDN_DEVICE` enum (default `SYCL`) + `OIDN_PREBUILT_DIR` (§4.1).
- [ ] Rewire `util/CMakeLists.txt` to drop `find_package(OpenImageDenoise)` and
      link the new `OpenImageDenoise` INTERFACE target (§4.6).
- [ ] Add `crt_add_packaged_files` to `cmake/package.cmake` (§4.5).

**Phase 1 — Generator + toolchain download:**
- [ ] Create `cmake/ninja.cmake` (find-or-fetch Ninja) (§4.7).
- [ ] Create `cmake/dpcpp.cmake` (§4.3) and `cmake/level_zero.cmake` (§4.4),
      **pinned to DPC++ `v6.2.1` / Level Zero `v1.29.0` with archive hashes**,
      `include()`d only when `OIDN_DEVICE=SYCL`.
- [ ] Verify DPC++ `clang`/`clang++` and Level Zero land in the build tree and the
      runtime-lib lists are populated.

**Phase 2 — OIDN source build (core, SYCL default):**
- [ ] Create `cmake/oidn.cmake`: escape-hatch check ? Ninja ? resolve `OIDN_DEVICE`
      (Apple?METAL) ? (SYCL) toolchain ? `ExternalProject_Add` for
      `aafra/semaphore` + `weights`, single selected device flag, DPC++ compiler
      when SYCL; define the INTERFACE `OpenImageDenoise` target (§4.2).
- [ ] Augment the `ExternalProject` env with DPC++ on `PATH` / `LD_LIBRARY_PATH`
      (SYCL only).
- [ ] Stage OIDN + DPC++ runtime libs (`sycl8`, `ur_*`, ...) and the selected
      device's lib next to the executable and into `bin/` (§4.5).
- [ ] Build & smoke-test `-DENABLE_OIDN=ON` (SYCL default) from a clean build dir.

**Phase 3 — Escape hatch + other devices:**
- [ ] Create `cmake/oidn_prebuilt.cmake` and wire `OIDN_PREBUILT_DIR` (§4.8).
- [ ] Smoke-test `-DOIDN_DEVICE=CUDA` / `-DOIDN_DEVICE=HIP` on machines with the
      respective toolkits; confirm the staged device runtime lib.

**Phase 4 — Polish & validation:**
- [ ] Add option validation (§5).
- [ ] Cross-platform gating (§6); macOS `OIDN_DEVICE_METAL` path (untested for now).
- [ ] Update `README.md`: remove the "build OIDN yourself" prerequisite; document
      `-DENABLE_OIDN=ON`, the `OIDN_DEVICE` option, and `OIDN_PREBUILT_DIR`.
- [ ] Validate Vulkan `timeline_semaphore` + `binary_semaphore` interop modes
      end-to-end.
- [ ] CI: cache the `oidn/` build tree (or feed `oidn/install` back via
      `OIDN_PREBUILT_DIR`); confirm download+build time acceptable.

---

## 10. Validation / testing

- Configure matrix: `{ENABLE_DXR, ENABLE_VULKAN}` on Windows; `{ENABLE_VULKAN}` on
  Linux; `{ENABLE_METAL}` on macOS — all with the default `OIDN_DEVICE`
  (`SYCL`, resolved to `METAL` on Apple).
- Additional device coverage where hardware/toolkits exist: `-DOIDN_DEVICE=CUDA`
  (NVIDIA) and `-DOIDN_DEVICE=HIP` (AMD).
- Escape-hatch path: configure once to build OIDN, then reconfigure a clean tree
  with `-DOIDN_PREBUILT_DIR=<that oidn/install>` and confirm no rebuild occurs.
- Ninja resolution: test both "Ninja on PATH" and "Ninja fetched" cases.
- Confirm the produced runtime libraries (including device-specific ones) land
  next to `chameleonrt` and in the install `bin/`.
- Run a denoise on a sample scene per backend; for Vulkan exercise each
  `--oidn-interop` mode (`host_blocking`, `timeline_semaphore`,
  `binary_semaphore`).
- Verify a clean build directory works from scratch (true "on-demand" download).
```
