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
endif()
