include(ExternalProject)

ExternalProject_Add(rapidjson_ext
    PREFIX rapidjson
    DOWNLOAD_DIR rapidjson
    STAMP_DIR rapidjson/stamp
    SOURCE_DIR rapidjson/src
    BINARY_DIR rapidjson
    URL "https://github.com/Tencent/rapidjson/archive/refs/tags/v1.1.0.zip"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_ALWAYS OFF
)

set(RAPIDJSON_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/rapidjson/src/include)

add_library(rapidjson INTERFACE)

add_dependencies(rapidjson rapidjson_ext)

if (CRT_DEPENDENCY_FOLDER)
    set_target_properties(rapidjson_ext PROPERTIES FOLDER "${CRT_DEPENDENCY_FOLDER}")
endif()

target_include_directories(rapidjson INTERFACE
    ${RAPIDJSON_INCLUDE_DIRS})
