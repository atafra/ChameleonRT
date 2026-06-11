include(ExternalProject)

if (TARGET level_zero_ext)
    return()
endif()

set(LEVEL_ZERO_VERSION "v1.29.0" CACHE STRING "Pinned Level Zero release used for OIDN SYCL builds")
set(LEVEL_ZERO_ARCHIVE_HASH "" CACHE STRING "Optional SHA256=... hash for the Level Zero archive")

set(_level_zero_url "https://github.com/oneapi-src/level-zero/archive/refs/tags/${LEVEL_ZERO_VERSION}.zip")
set(_level_zero_hash_args)
if (LEVEL_ZERO_ARCHIVE_HASH)
    list(APPEND _level_zero_hash_args URL_HASH "${LEVEL_ZERO_ARCHIVE_HASH}")
else()
    message(WARNING "LEVEL_ZERO_ARCHIVE_HASH is empty; download integrity is not pinned yet")
endif()

ExternalProject_Add(level_zero_ext
    PREFIX level_zero
    DOWNLOAD_DIR level_zero
    STAMP_DIR level_zero/stamp
    SOURCE_DIR level_zero/src
    BINARY_DIR level_zero/build
    URL "${_level_zero_url}"
    ${_level_zero_hash_args}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_ALWAYS OFF)

set(LEVEL_ZERO_ROOT "${CMAKE_CURRENT_BINARY_DIR}/level_zero/src" CACHE INTERNAL "Extracted Level Zero root")
