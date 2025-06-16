cmake_minimum_required(VERSION 3.15)
project(TECMFS)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

find_package(OpenGL REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)

# === Cliente (GUI) ===
add_executable(tecmfs_client
    main.cpp
    "src/DiskController.cpp"
    src/Client.cpp
    src/InterfaceGUI.cpp

    # Archivos base de ImGui
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/imgui_tables.cpp

    # Backends
    external/imgui/imgui_impl_glfw.cpp
    external/imgui/imgui_impl_opengl3.cpp
)

target_include_directories(tecmfs_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/external/http
    ${CMAKE_CURRENT_SOURCE_DIR}/includes
)

target_link_libraries(tecmfs_client
    glfw
    OpenGL::GL
    tinyxml2::tinyxml2
)


if(UNIX AND NOT APPLE)
    target_link_libraries(tecmfs_client PRIVATE dl pthread)
endif()