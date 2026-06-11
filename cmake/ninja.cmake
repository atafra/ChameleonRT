include(ExternalProject)

if (DEFINED OIDN_NINJA_EXECUTABLE AND OIDN_NINJA_EXECUTABLE)
    return()
endif()

find_program(OIDN_NINJA_EXECUTABLE ninja)
if (OIDN_NINJA_EXECUTABLE)
    set(OIDN_NINJA_DEP_TARGET "" CACHE INTERNAL "Optional Ninja dependency target")
    message(STATUS "Using Ninja from PATH: ${OIDN_NINJA_EXECUTABLE}")
    return()
endif()

set(OIDN_NINJA_VERSION "v1.12.1")
if (WIN32)
    set(_oidn_ninja_archive "ninja-win.zip")
elseif(APPLE)
    set(_oidn_ninja_archive "ninja-mac.zip")
else()
    set(_oidn_ninja_archive "ninja-linux.zip")
endif()

ExternalProject_Add(ninja_ext
    PREFIX ninja
    DOWNLOAD_DIR ninja
    STAMP_DIR ninja/stamp
    SOURCE_DIR ninja/src
    BINARY_DIR ninja/build
    URL "https://github.com/ninja-build/ninja/releases/download/${OIDN_NINJA_VERSION}/${_oidn_ninja_archive}"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_ALWAYS OFF)

if (CRT_DEPENDENCY_FOLDER)
    set_target_properties(ninja_ext PROPERTIES FOLDER "${CRT_DEPENDENCY_FOLDER}")
endif()

set(OIDN_NINJA_EXECUTABLE "${CMAKE_CURRENT_BINARY_DIR}/ninja/src/ninja${CMAKE_EXECUTABLE_SUFFIX}")
set(OIDN_NINJA_DEP_TARGET ninja_ext CACHE INTERNAL "Optional Ninja dependency target")
message(STATUS "Ninja not found on PATH; will download ${OIDN_NINJA_VERSION} for OIDN builds")
