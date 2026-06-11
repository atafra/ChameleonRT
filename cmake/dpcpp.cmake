include(ExternalProject)

if (TARGET dpcpp_ext)
    return()
endif()

if (APPLE)
    message(FATAL_ERROR "DPC++ is not used on macOS in this project; choose OIDN_DEVICE=METAL or CPU")
endif()

set(DPCPP_VERSION "v6.2.1" CACHE STRING "Pinned Intel LLVM release used for OIDN SYCL builds")
set(DPCPP_ARCHIVE_HASH "" CACHE STRING "Optional SHA256=... hash for the DPC++ archive")

if (WIN32)
    set(_dpcpp_archive "sycl_windows.tar.gz")
else()
    set(_dpcpp_archive "sycl_linux.tar.gz")
endif()

set(_dpcpp_url "https://github.com/intel/llvm/releases/download/${DPCPP_VERSION}/${_dpcpp_archive}")
set(_dpcpp_hash_args)
if (DPCPP_ARCHIVE_HASH)
    list(APPEND _dpcpp_hash_args URL_HASH "${DPCPP_ARCHIVE_HASH}")
else()
    message(WARNING "DPCPP_ARCHIVE_HASH is empty; download integrity is not pinned yet")
endif()

ExternalProject_Add(dpcpp_ext
    PREFIX dpcpp
    DOWNLOAD_DIR dpcpp
    STAMP_DIR dpcpp/stamp
    SOURCE_DIR dpcpp/src
    BINARY_DIR dpcpp/build
    URL "${_dpcpp_url}"
    ${_dpcpp_hash_args}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_ALWAYS OFF)

if (CRT_DEPENDENCY_FOLDER)
    set_target_properties(dpcpp_ext PROPERTIES FOLDER "${CRT_DEPENDENCY_FOLDER}")
endif()

set(DPCPP_ROOT "${CMAKE_CURRENT_BINARY_DIR}/dpcpp/src" CACHE INTERNAL "Extracted DPC++ root")
set(DPCPP_BIN_DIR "${DPCPP_ROOT}/bin" CACHE INTERNAL "DPC++ bin directory")
set(DPCPP_CLANG "${DPCPP_BIN_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX}" CACHE INTERNAL "DPC++ clang executable")
set(DPCPP_CLANGXX "${DPCPP_BIN_DIR}/clang++${CMAKE_EXECUTABLE_SUFFIX}" CACHE INTERNAL "DPC++ clang++ executable")

if (WIN32)
    set(_dpcpp_runtime_dir "${DPCPP_BIN_DIR}")
else()
    set(_dpcpp_runtime_dir "${DPCPP_ROOT}/lib")
endif()

set(DPCPP_RUNTIME_LIBRARIES
    "${_dpcpp_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}sycl8${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${_dpcpp_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}ur_loader${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${_dpcpp_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}ur_win_proxy_loader${CMAKE_SHARED_LIBRARY_SUFFIX}"
    "${_dpcpp_runtime_dir}/${CMAKE_SHARED_LIBRARY_PREFIX}ur_adapter_level_zero${CMAKE_SHARED_LIBRARY_SUFFIX}"
    CACHE INTERNAL "DPC++ runtime libraries to package")
