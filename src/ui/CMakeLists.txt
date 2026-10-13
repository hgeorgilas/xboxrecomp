cmake_minimum_required(VERSION 3.20)

# Settings overlay: Dear ImGui (third_party/imgui, v1.91.5, MIT) over the
# D3D11 present. The only C++ in the runtime.

get_filename_component(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/imgui" ABSOLUTE)

add_library(xbox_ui STATIC
    xbox_overlay.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(xbox_ui PRIVATE ${IMGUI_DIR})
set_target_properties(xbox_ui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(xbox_ui PRIVATE d3d11 d3dcompiler dxgi)
