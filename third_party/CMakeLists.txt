# Vendored third-party code (AGENTS.md section 5). Never links sol_options;
# compiled with default warnings, never modified in place.

find_package(Vulkan REQUIRED)

add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_vulkan.cpp
    imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC imgui imgui/backends)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan)
