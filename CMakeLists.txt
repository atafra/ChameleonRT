cmake_minimum_required(VERSION 3.18)
project(chameleonrt)

if (NOT WIN32)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
endif()

add_definitions(-DNOMINMAX -DGLM_ENABLE_EXPERIMENTAL -DSDL_MAIN_HANDLED
    -DWIN32_LEAN_AND_MEAN)

if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    add_definitions(-D_DEBUG)
endif ()


set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_LIST_DIR}/cmake")
# Output all render plugins to the root binary dir where the executable is
# so setting runtime paths is easier
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

find_package(Threads REQUIRED)
# Include glm as an external project
include(cmake/glm.cmake)
# Include RapidJSON as an external project (header-only, used for benchmark and
# scene reports consumed by the analysis scripts)
include(cmake/rapidjson.cmake)
include(cmake/package.cmake)

include(CMakeDependentOption)

include(cmake/sdl2.cmake)

# OIDN denoiser: can be built from source on demand (see cmake/oidn.cmake)
option(ENABLE_OIDN "Build with Intel Open Image Denoise support." OFF)
set(OIDN_DEVICE "SYCL" CACHE STRING
    "OIDN device backend to build (exactly one): SYCL, CUDA, HIP, METAL, CPU")
set_property(CACHE OIDN_DEVICE PROPERTY STRINGS SYCL CUDA HIP METAL CPU)
set(OIDN_PREBUILT_DIR "" CACHE PATH
    "Path to an existing OIDN install; if set, skip downloading/building OIDN")

if (ENABLE_OIDN)
    include(cmake/oidn.cmake)
endif()

add_subdirectory(imgui)
add_subdirectory(util)
add_subdirectory(backends)

option(REPORT_RAY_STATS "Track and report rays/second. May incur a slight rendering performance penalty" OFF)

add_executable(chameleonrt main.cpp)

set_target_properties(chameleonrt PROPERTIES
	CXX_STANDARD 14
	CXX_STANDARD_REQUIRED ON)

target_link_libraries(chameleonrt PUBLIC
    util
    display)

install(TARGETS chameleonrt
        RUNTIME DESTINATION bin)

