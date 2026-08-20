# Everything except the entry point lives in a library, so the game's own
# logic - targeting, the map's rows, the screens' fill/execute seam - can be
# reached by a test binary. Before this split `add_executable` held the whole
# tree and every suite linked an engine/ library, so nothing under game/src
# was testable at all: the map and targeting code had no regression test not
# through neglect but because there was no target that could see it. Same
# structural cause as `encodeMesh` sitting in the cooker's main.cpp outside
# sol_cooker_lib, and the same fix.
add_library(sol_game_lib STATIC
    src/combat_effects.cpp
    src/content.cpp
    src/game_audio.cpp
    src/game_ui.cpp
    src/input_actions.cpp
    src/map_screen.cpp
    src/map_ui.cpp
    src/ship_screen.cpp
    src/ship_ui.cpp
    src/menu_screens.cpp
    src/scene_renderer.cpp
    src/shader_watcher.cpp
    src/space_world.cpp
    src/station_screen.cpp
    src/station_ui.cpp
    src/target_pick.cpp
    src/thruster_particles.cpp
)
add_library(sol::game ALIAS sol_game_lib)

# PUBLIC: a test includes these headers by their bare name the same way the
# game's own sources include each other.
target_include_directories(sol_game_lib PUBLIC src)

target_link_libraries(sol_game_lib PUBLIC
    sol::core
    sol::ecs
    sol::sim
    sol::scripting
    sol::platform
    sol::rhi
    sol::renderer
    sol::assets
    sol::audio
    sol::ui
)

add_executable(sol_game src/main.cpp)
set_target_properties(sol_game PROPERTIES OUTPUT_NAME sol)

target_link_libraries(sol_game PRIVATE sol_game_lib)

# Data defs + Lua scripts load straight from the source tree in dev builds so
# edits hot-reload; shipping layouts fall back to dirs beside the executable.
target_compile_definitions(sol_game PRIVATE
    SOL_DATA_SOURCE_DIR="${CMAKE_SOURCE_DIR}/game/data"
    SOL_MODS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/game/mods"
)

# Shader hot-reload (dev): recompile changed GLSL at runtime with glslc.
if(SOL_GLSLC)
    target_compile_definitions(sol_game PRIVATE
        SOL_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders"
        SOL_GLSLC_PATH="${SOL_GLSLC}"
    )
endif()

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

# The game's own logic, headless. Declared here rather than in engine/test/
# because `engine` is configured before `game`, so the library does not exist
# yet at that point. Per AGENTS section 7 this covers sim-adjacent logic -
# targeting, the map's rows - and NOT the renderer, which is still verified by
# running the app.
add_executable(sol_game_tests
    test/map_row_tests.cpp
    test/target_cycle_tests.cpp
)
target_link_libraries(sol_game_tests PRIVATE sol::test sol_game_lib)
add_test(NAME game.unit COMMAND sol_game_tests)
