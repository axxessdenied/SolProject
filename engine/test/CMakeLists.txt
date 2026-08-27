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
    core/gpu_timestamp_tests.cpp
    core/hash_tests.cpp
    core/inflate_tests.cpp
    core/jobs_tests.cpp
    core/json_tests.cpp
    core/math_tests.cpp
    core/profiler_tests.cpp
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
    assets/def_doc_tests.cpp
    assets/font_tests.cpp
    assets/loadout_tests.cpp
    assets/texture_doc_tests.cpp
    assets/truetype_tests.cpp
)
target_link_libraries(sol_assets_tests PRIVATE sol::test sol::assets)
# The committed `.tex` sources are this suite's regression net for the same
# reason the `.forge` files are geometry.unit's: an invariant asserted against a
# fixture the code built agrees with the code by construction.
#
# SOL_DEF_DATA_DIR is the same argument one directory over (Phase 9 stage H):
# the def document's round trip is only worth anything against the files a
# person actually hand-wrote, since those are the ones carrying the comments,
# the integer spellings and the exponent forms it exists to preserve.
target_compile_definitions(sol_assets_tests
    PRIVATE SOL_TEXTURE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/textures"
            SOL_DEF_DATA_DIR="${CMAKE_SOURCE_DIR}/game/data")
add_test(NAME assets.unit COMMAND sol_assets_tests)

# The editable mesh core (Phase 9 stage B). Headless by construction - no
# device, no window - and the five shipped meshes are its regression net.
add_executable(sol_geometry_tests
    geometry/forge_doc_tests.cpp
    geometry/mesh_build_tests.cpp
    geometry/mesh_edit_tests.cpp
    geometry/mesh_lod_tests.cpp
    geometry/shipped_mesh_tests.cpp
)
target_link_libraries(sol_geometry_tests PRIVATE sol::test sol::assets)
# The committed .forge sources are the regression net now that the generator's
# mesh half is gone, so the suite has to read the real files. Handed in by the
# build rather than found by walking up from the working directory, which is a
# different place under ctest than under a debugger.
target_compile_definitions(sol_geometry_tests
    PRIVATE SOL_MESH_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/meshes")
add_test(NAME geometry.unit COMMAND sol_geometry_tests)

add_executable(sol_platform_tests
    platform/file_io_tests.cpp
    platform/input_bindings_tests.cpp
)
target_link_libraries(sol_platform_tests PRIVATE sol::test sol::platform)
# The file_io tests write real files, so they need a directory they are allowed
# to make a mess in. Under the build tree rather than the source tree, and named
# rather than derived from the CWD, which differs under ctest and a debugger.
target_compile_definitions(sol_platform_tests
    PRIVATE SOL_PLATFORM_TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/scratch")
# The xkb keysym table (Phase 21 stage B) is the one part of a keyboard backend
# that can be wrong without anything failing: a mis-mapped key compiles, links,
# runs, and waits for somebody to press it. It is pure data over a header of
# constants, so it joins this suite on Linux rather than needing a compositor.
if(LINUX)
    target_sources(sol_platform_tests PRIVATE platform/xkb_keymap_tests.cpp)
    target_link_libraries(sol_platform_tests PRIVATE PkgConfig::XKBCOMMON)
endif()
add_test(NAME platform.unit COMMAND sol_platform_tests)

# The mixer is exercised headlessly by calling renderAudio directly - no
# device, no sound card, no wall clock. A rule verified only by ear is a rule
# that gets re-verified by ear forever.
add_executable(sol_audio_tests
    audio/mixer_tests.cpp
)
target_link_libraries(sol_audio_tests PRIVATE sol::test sol::audio)
add_test(NAME audio.unit COMMAND sol_audio_tests)

add_executable(sol_ui_tests
    ui/cockpit_frame_tests.cpp
    ui/context_tests.cpp
    ui/draw_list_tests.cpp
    ui/layout_tests.cpp
    ui/map_projection_tests.cpp
    ui/pick_tests.cpp
    ui/radar_projection_tests.cpp
)
target_link_libraries(sol_ui_tests PRIVATE sol::test sol::ui)
# The Key -> ImGuiKey table (Phase 21 stage C) has the same shape of risk the
# xkb table does one layer down: a wrong row is invisible until somebody
# presses that key with the console focused. It joins this suite on Linux only,
# because on Windows imgui_impl_win32 owns the equivalent table upstream and
# there is nothing of ours to check.
if(LINUX)
    target_sources(sol_ui_tests PRIVATE ui/imgui_key_map_tests.cpp)
endif()
add_test(NAME ui.unit COMMAND sol_ui_tests)

add_executable(sol_scripting_tests
    scripting/vm_tests.cpp
)
target_link_libraries(sol_scripting_tests PRIVATE sol::test sol::scripting)
add_test(NAME scripting.unit COMMAND sol_scripting_tests)

add_executable(sol_sim_tests
    sim/collision_tests.cpp
    sim/damage_tests.cpp
    sim/docking_tests.cpp
    sim/economy_tests.cpp
    sim/faction_sim_tests.cpp
    sim/jump_transition_tests.cpp
    sim/mining_tests.cpp
    sim/mission_tests.cpp
    sim/pilot_tips_tests.cpp
    sim/predation_tests.cpp
    sim/fixed_loop_tests.cpp
    sim/flight_tests.cpp
    sim/power_tests.cpp
    sim/steering_tests.cpp
    sim/survey_tests.cpp
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
