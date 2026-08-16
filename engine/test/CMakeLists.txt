# Minimal in-repo test harness (see AGENTS.md section 7 - no external test framework).
add_library(sol_test_harness STATIC
    src/test_main.cpp
)
add_library(sol::test ALIAS sol_test_harness)

target_include_directories(sol_test_harness PUBLIC include)
target_link_libraries(sol_test_harness PUBLIC sol_options)

# Unit test executables, one per engine module under test.
add_executable(sol_core_tests
    core/events_tests.cpp
    core/inflate_tests.cpp
    core/jobs_tests.cpp
    core/json_tests.cpp
    core/math_tests.cpp
    core/random_tests.cpp
    core/serialize_tests.cpp
    core/toml_tests.cpp
    core/version_tests.cpp
)
target_link_libraries(sol_core_tests PRIVATE sol::test sol::core)
add_test(NAME core.unit COMMAND sol_core_tests)

add_executable(sol_ecs_tests
    ecs/ecs_tests.cpp
    ecs/snapshot_tests.cpp
)
target_link_libraries(sol_ecs_tests PRIVATE sol::test sol::ecs)
add_test(NAME ecs.unit COMMAND sol_ecs_tests)

add_executable(sol_assets_tests
    assets/data_defs_tests.cpp
    assets/font_tests.cpp
    assets/loadout_tests.cpp
    assets/truetype_tests.cpp
)
target_link_libraries(sol_assets_tests PRIVATE sol::test sol::assets)
add_test(NAME assets.unit COMMAND sol_assets_tests)

add_executable(sol_ui_tests
    ui/draw_list_tests.cpp
)
target_link_libraries(sol_ui_tests PRIVATE sol::test sol::ui)
add_test(NAME ui.unit COMMAND sol_ui_tests)

add_executable(sol_scripting_tests
    scripting/vm_tests.cpp
)
target_link_libraries(sol_scripting_tests PRIVATE sol::test sol::scripting)
add_test(NAME scripting.unit COMMAND sol_scripting_tests)

add_executable(sol_sim_tests
    sim/collision_tests.cpp
    sim/damage_tests.cpp
    sim/economy_tests.cpp
    sim/faction_sim_tests.cpp
    sim/mission_tests.cpp
    sim/fixed_loop_tests.cpp
    sim/flight_tests.cpp
    sim/power_tests.cpp
    sim/steering_tests.cpp
    sim/universe_tests.cpp
    sim/weapons_tests.cpp
)
target_link_libraries(sol_sim_tests PRIVATE sol::test sol::sim)
add_test(NAME sim.unit COMMAND sol_sim_tests)

# Phase 3 ECS storage spike benchmark; run manually in the release preset,
# results recorded in docs/decisions/. Not registered with CTest.
add_executable(sol_ecs_storage_bench
    bench/ecs_storage_bench.cpp
)
target_link_libraries(sol_ecs_storage_bench PRIVATE sol_options)
