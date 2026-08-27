add_library(sol_renderer STATIC
    src/debug_draw_renderer.cpp
    src/impostor_renderer.cpp
    src/material_registry.cpp
    src/material_state.cpp
    src/mesh_renderer.cpp
    src/particle_renderer.cpp
    src/scene_pass.cpp
    src/sky_renderer.cpp
    src/spirv_reflect.cpp
    src/tonemap_renderer.cpp
    src/ui_renderer.cpp
)
add_library(sol::renderer ALIAS sol_renderer)

target_include_directories(sol_renderer PUBLIC include)
target_link_libraries(sol_renderer PUBLIC sol::rhi sol::assets)
