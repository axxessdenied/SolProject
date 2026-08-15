add_executable(sol_game
    src/main.cpp
    src/scene_renderer.cpp
)
set_target_properties(sol_game PROPERTIES OUTPUT_NAME sol)

target_link_libraries(sol_game PRIVATE
    sol::core
    sol::platform
    sol::rhi
    sol::renderer
    sol::assets
)

add_dependencies(sol_game sol_shaders sol_cook_assets)

# Cook source assets into the runtime directory on every build; the cooker
# itself skips files whose outputs are already up to date.
add_custom_target(sol_cook_assets
    COMMAND $<TARGET_FILE:sol_cooker> ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/bin/cooked
    COMMENT "Cooking assets"
    VERBATIM
)
add_dependencies(sol_cook_assets sol_cooker)

# Needs a GPU + Vulkan driver, so it is excluded from the default test presets
# (label "gpu"); run locally with: ctest --preset dev-gpu
add_test(NAME game.smoke COMMAND sol_game --frames 120)
set_tests_properties(game.smoke PROPERTIES LABELS gpu)
