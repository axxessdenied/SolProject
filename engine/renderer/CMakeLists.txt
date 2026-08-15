add_library(sol_renderer STATIC
    src/impostor_renderer.cpp
    src/mesh_renderer.cpp
    src/scene_pass.cpp
    src/sky_renderer.cpp
    src/tonemap_renderer.cpp
)
add_library(sol::renderer ALIAS sol_renderer)

target_include_directories(sol_renderer PUBLIC include)
target_link_libraries(sol_renderer PUBLIC sol::rhi sol::assets)
