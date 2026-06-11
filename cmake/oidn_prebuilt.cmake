if (NOT OIDN_PREBUILT_DIR)
    message(FATAL_ERROR "OIDN_PREBUILT_DIR must be set before including oidn_prebuilt.cmake")
endif()

set(_oidn_runtime_dir "${OIDN_PREBUILT_DIR}/lib")
if (WIN32)
    set(_oidn_runtime_dir "${OIDN_PREBUILT_DIR}/bin")
endif()

set(OIDN_INCLUDE_DIR "${OIDN_PREBUILT_DIR}/include")
if (WIN32)
    set(OIDN_LINK_LIBRARY "${OIDN_PREBUILT_DIR}/lib/OpenImageDenoise${CMAKE_IMPORT_LIBRARY_SUFFIX}")
else()
    set(OIDN_LINK_LIBRARY "${OIDN_PREBUILT_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}OpenImageDenoise${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()

if (NOT EXISTS "${OIDN_INCLUDE_DIR}/OpenImageDenoise/oidn.hpp")
    message(FATAL_ERROR "OIDN_PREBUILT_DIR='${OIDN_PREBUILT_DIR}' does not look like an OIDN install")
endif()
if (NOT EXISTS "${OIDN_LINK_LIBRARY}")
    message(FATAL_ERROR "OIDN_PREBUILT_DIR='${OIDN_PREBUILT_DIR}' missing expected OIDN link library: ${OIDN_LINK_LIBRARY}")
endif()

if (NOT TARGET OpenImageDenoise)
    add_library(OpenImageDenoise INTERFACE)
endif()
target_include_directories(OpenImageDenoise INTERFACE "${OIDN_INCLUDE_DIR}")
target_link_libraries(OpenImageDenoise INTERFACE "${OIDN_LINK_LIBRARY}")

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

set(OIDN_RUNTIME_LIBRARIES
    "${_oidn_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}OpenImageDenoise${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${_oidn_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}OpenImageDenoise_core${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${_oidn_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}${_oidn_device_runtime}${CMAKE_SHARED_LIBRARY_SUFFIX}"
    CACHE INTERNAL "OIDN runtime libs to stage/install")
