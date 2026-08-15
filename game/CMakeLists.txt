add_executable(sol_game
    src/main.cpp
    src/triangle_renderer.cpp
)
set_target_properties(sol_game PROPERTIES OUTPUT_NAME sol)

target_link_libraries(sol_game PRIVATE
    sol::core
    sol::platform
    sol::rhi
)

add_dependencies(sol_game sol_shaders)

# Needs a GPU + Vulkan driver, so it is excluded from the default test presets
# (label "gpu"); run locally with: ctest --preset dev-gpu
add_test(NAME game.smoke COMMAND sol_game --frames 120)
set_tests_properties(game.smoke PROPERTIES LABELS gpu)
