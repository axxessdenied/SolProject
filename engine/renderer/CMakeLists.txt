add_library(sol_renderer STATIC
    src/mesh_renderer.cpp
    src/scene_pass.cpp
)
add_library(sol::renderer ALIAS sol_renderer)

target_include_directories(sol_renderer PUBLIC include)
target_link_libraries(sol_renderer PUBLIC sol::rhi sol::assets)
