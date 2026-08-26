add_library(sol_ui STATIC
    src/context.cpp
    src/dev_ui.cpp
    src/draw_list.cpp
    src/imgui_host.cpp
)
add_library(sol::ui ALIAS sol_ui)

target_include_directories(sol_ui PUBLIC include)
target_link_libraries(sol_ui PUBLIC sol::renderer imgui)

if(WIN32)
    # ImGui platform backend bridge: sanctioned platform touch point (AGENTS.md 4).
    target_sources(sol_ui PRIVATE src/win32/dev_ui_win32.cpp)
elseif(LINUX)
    # Hand-written: there is no upstream imgui_impl_wayland to delegate to.
    target_sources(sol_ui PRIVATE src/linux/dev_ui_wayland.cpp)
else()
    # Same reasoning as sol_rhi's: fail naming the gap, not the mangled symbol.
    message(FATAL_ERROR "sol_ui has no ImGui platform backend for this OS (Windows and Linux only).")
endif()
