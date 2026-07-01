find_package(OpenGL REQUIRED)

add_subdirectory(glad)

add_library(display
    imgui_impl_sdl.cpp
    gpu_info.cpp
    gldisplay.cpp
    shader.cpp
    imgui_impl_opengl3.cpp)

set_target_properties(display PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

target_link_libraries(display PUBLIC util glad)

if (WIN32)
    target_link_libraries(display PRIVATE setupapi)
endif()

