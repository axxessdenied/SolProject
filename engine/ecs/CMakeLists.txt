add_library(sol_ecs STATIC
    src/registry.cpp
)
add_library(sol::ecs ALIAS sol_ecs)

target_include_directories(sol_ecs PUBLIC include)
target_link_libraries(sol_ecs PUBLIC sol::core)
