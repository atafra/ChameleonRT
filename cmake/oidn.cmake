include(ExternalProject)

if (TARGET OpenImageDenoise)
    return()
endif()

string(TOUPPER "${OIDN_DEVICE}" OIDN_DEVICE_RESOLVED)
if (APPLE AND NOT OIDN_DEVICE_RESOLVED STREQUAL "CPU")
    set(OIDN_DEVICE_RESOLVED "METAL")
endif()

set(_oidn_valid_devices SYCL CUDA HIP METAL CPU)
if (NOT OIDN_DEVICE_RESOLVED IN_LIST _oidn_valid_devices)
    message(FATAL_ERROR "OIDN_DEVICE='${OIDN_DEVICE}' is invalid; choose one of: ${_oidn_valid_devices}")
endif()

if (OIDN_PREBUILT_DIR)
    include(cmake/oidn_prebuilt.cmake)
    crt_add_packaged_files(${OIDN_RUNTIME_LIBRARIES})
    message(STATUS "OIDN: using prebuilt install from ${OIDN_PREBUILT_DIR} (device=${OIDN_DEVICE_RESOLVED})")
    return()
endif()

find_package(Git QUIET)
if (NOT GIT_FOUND)
    message(FATAL_ERROR "ENABLE_OIDN requires Git when OIDN_PREBUILT_DIR is not set")
endif()

include(cmake/ninja.cmake)

if (OIDN_DEVICE_RESOLVED STREQUAL "CUDA")
    find_package(CUDAToolkit QUIET)
    if (NOT CUDAToolkit_FOUND)
        message(FATAL_ERROR "OIDN_DEVICE=CUDA requires CUDA Toolkit")
    endif()
elseif(OIDN_DEVICE_RESOLVED STREQUAL "HIP")
    find_program(HIPCC_EXECUTABLE hipcc)
    if (NOT HIPCC_EXECUTABLE)
        message(FATAL_ERROR "OIDN_DEVICE=HIP requires ROCm/HIP (hipcc not found)")
    endif()
endif()

set(_oidn_device_args
    -DOIDN_DEVICE_CPU=OFF
    -DOIDN_DEVICE_SYCL=OFF
    -DOIDN_DEVICE_CUDA=OFF
    -DOIDN_DEVICE_HIP=OFF
    -DOIDN_DEVICE_METAL=OFF
    -DOIDN_DEVICE_${OIDN_DEVICE_RESOLVED}=ON)

set(_oidn_toolchain_deps)
set(_oidn_compiler_args)
set(_oidn_runtime_libs)
if (OIDN_DEVICE_RESOLVED STREQUAL "SYCL")
    include(cmake/dpcpp.cmake)
    include(cmake/level_zero.cmake)
    list(APPEND _oidn_toolchain_deps dpcpp_ext level_zero_ext)
    list(APPEND _oidn_compiler_args
        -DCMAKE_C_COMPILER=${DPCPP_CLANG}
        -DCMAKE_CXX_COMPILER=${DPCPP_CLANGXX}
        -DLEVEL_ZERO_ROOT=${LEVEL_ZERO_ROOT})
    list(APPEND _oidn_runtime_libs ${DPCPP_RUNTIME_LIBRARIES})
endif()
if (OIDN_NINJA_DEP_TARGET)
    list(APPEND _oidn_toolchain_deps ${OIDN_NINJA_DEP_TARGET})
endif()

set(OIDN_ROOT "${CMAKE_CURRENT_BINARY_DIR}/oidn")
set(OIDN_SOURCE_DIR "${OIDN_ROOT}/src")
set(OIDN_BUILD_DIR "${OIDN_ROOT}/build")
set(OIDN_INSTALL_DIR "${OIDN_ROOT}/install")
set(OIDN_INCLUDE_DIR "${OIDN_INSTALL_DIR}/include")

ExternalProject_Add(oidn_ext
    PREFIX oidn
    DOWNLOAD_DIR oidn
    STAMP_DIR oidn/stamp
    SOURCE_DIR "${OIDN_SOURCE_DIR}"
    BINARY_DIR "${OIDN_BUILD_DIR}"
    DEPENDS ${_oidn_toolchain_deps}
    GIT_REPOSITORY "https://github.com/RenderKit/oidn.git"
    GIT_TAG "origin/aafra/semaphore"
    GIT_SUBMODULES "weights"
    GIT_SHALLOW TRUE
    UPDATE_DISCONNECTED OFF
    CMAKE_GENERATOR "Ninja"
    CMAKE_ARGS
        -DCMAKE_MAKE_PROGRAM=${OIDN_NINJA_EXECUTABLE}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${OIDN_INSTALL_DIR}
        -DOIDN_APPS=OFF
        -DOIDN_DEVICE_SYCL_AOT=OFF
        ${_oidn_device_args}
        ${_oidn_compiler_args}
    BUILD_ALWAYS OFF)

if (WIN32)
    set(OIDN_LINK_LIBRARY "${OIDN_INSTALL_DIR}/lib/OpenImageDenoise${CMAKE_IMPORT_LIBRARY_SUFFIX}")
    set(_oidn_runtime_dir "${OIDN_INSTALL_DIR}/bin")
else()
    set(OIDN_LINK_LIBRARY "${OIDN_INSTALL_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}OpenImageDenoise${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(_oidn_runtime_dir "${OIDN_INSTALL_DIR}/lib")
endif()

set(_oidn_device_runtime "OpenImageDenoise_device_sycl")
if (OIDN_DEVICE_RESOLVED STREQUAL "CUDA")
    set(_oidn_device_runtime "OpenImageDenoise_device_cuda")
elseif(OIDN_DEVICE_RESOLVED STREQUAL "HIP")
    set(_oidn_device_runtime "OpenImageDenoise_device_hip")
elseif(OIDN_DEVICE_RESOLVED STREQUAL "METAL")
    set(_oidn_device_runtime "OpenImageDenoise_device_metal")
elseif(OIDN_DEVICE_RESOLVED STREQUAL "CPU")
    set(_oidn_device_runtime "OpenImageDenoise_device_cpu")
endif()

list(APPEND _oidn_runtime_libs
    "${_oidn_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}OpenImageDenoise${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${_oidn_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}OpenImageDenoise_core${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${_oidn_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}${_oidn_device_runtime}${CMAKE_SHARED_LIBRARY_SUFFIX}")

set(OIDN_RUNTIME_LIBRARIES "${_oidn_runtime_libs}" CACHE INTERNAL "OIDN runtime libs to stage/install")

file(MAKE_DIRECTORY "${OIDN_INCLUDE_DIR}")
add_library(OpenImageDenoise INTERFACE)
add_dependencies(OpenImageDenoise oidn_ext)
target_include_directories(OpenImageDenoise INTERFACE "${OIDN_INCLUDE_DIR}")
target_link_libraries(OpenImageDenoise INTERFACE "${OIDN_LINK_LIBRARY}")

# These runtime libraries are produced by the source builds, not present on disk
# at configure time. Stage them only after oidn_ext finishes; since oidn_ext
# DEPENDS on the toolchain projects (dpcpp_ext/level_zero_ext), the DPC++ runtime
# DLLs are also guaranteed to exist by then. Without this DEPENDS, the generated
# copy projects race the external builds and fail on a clean first build.
crt_add_packaged_files(${OIDN_RUNTIME_LIBRARIES} DEPENDS oidn_ext)

message(STATUS "OIDN: configured self-contained source build (device=${OIDN_DEVICE_RESOLVED})")
