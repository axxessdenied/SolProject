# Minimal in-repo test harness (see AGENTS.md section 7 - no external test framework).
add_library(sol_test_harness STATIC
    src/test_main.cpp
)
add_library(sol::test ALIAS sol_test_harness)

target_include_directories(sol_test_harness PUBLIC include)
target_link_libraries(sol_test_harness PUBLIC sol_options)

# Unit test executables, one per engine module under test.
add_executable(sol_core_tests
    core/inflate_tests.cpp
    core/jobs_tests.cpp
    core/json_tests.cpp
    core/math_tests.cpp
    core/version_tests.cpp
)
target_link_libraries(sol_core_tests PRIVATE sol::test sol::core)
add_test(NAME core.unit COMMAND sol_core_tests)

add_executable(sol_ecs_tests
    ecs/ecs_tests.cpp
)
target_link_libraries(sol_ecs_tests PRIVATE sol::test sol::ecs)
add_test(NAME ecs.unit COMMAND sol_ecs_tests)

# Phase 3 ECS storage spike benchmark; run manually in the release preset,
# results recorded in docs/decisions/. Not registered with CTest.
add_executable(sol_ecs_storage_bench
    bench/ecs_storage_bench.cpp
)
target_link_libraries(sol_ecs_storage_bench PRIVATE sol_options)
