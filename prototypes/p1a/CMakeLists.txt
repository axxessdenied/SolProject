# P1a technical-risk prototypes.
#
# Per the P1a milestone plan these executables are disposable: they exist to produce
# evidence, not to become SolEngine. Nothing here may be promoted into engine/ or game/
# without an explicit review. The harness library is the exception the plan anticipates --
# it is durable, and it is kept free of any simulation or domain concept so that promoting
# it later stays a real option rather than a sunk cost.

add_subdirectory(Harness)
add_subdirectory(Frames)
add_subdirectory(Orbit)

# Scenario output goes to the build tree by default. Evidence runs redirect it to
# evidence/p1a/A1/raw/, which .gitignore excludes.
set(SOL_SCENARIO_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/scenario-output")

#[[
Declares a headless prototype executable.

Every prototype follows the same shape -- one Main.cpp, the harness, and the project-wide
compiler policy -- so the shape is defined once here.
]]
function(sol_add_prototype name)
    add_executable(${name} ${name}/Main.cpp)
    sol_apply_project_options(${name})
    target_link_libraries(${name} PRIVATE sol::proto::harness)
endfunction()

#[[
Declares a prototype that emits a measurement report, and registers it as a CTest test that
writes to the scenario output directory.
]]
function(sol_add_scenario name)
    sol_add_prototype(${name})
    add_test(NAME ${name}
             COMMAND ${name} --out "${SOL_SCENARIO_OUTPUT_DIR}/${name}.json")
    set_tests_properties(${name} PROPERTIES LABELS "p1a;a1")
endfunction()

#[[
Declares an increment A2 prototype, which additionally links the frame library.

Kept separate from sol_add_prototype rather than adding an optional argument so that a target
cannot acquire the reference-data dependency by accident: A1's targets must stay runnable
without any fixture on disk.
]]
function(sol_add_frames_prototype name)
    add_executable(${name} ${name}/Main.cpp)
    sol_apply_project_options(${name})
    target_link_libraries(${name} PRIVATE sol::proto::harness sol::proto::frames)
endfunction()

function(sol_add_frames_scenario name)
    sol_add_frames_prototype(${name})
    add_test(NAME ${name}
             COMMAND ${name} --out "${SOL_SCENARIO_OUTPUT_DIR}/${name}.json")
    set_tests_properties(${name} PROPERTIES LABELS "p1a;a2")
endfunction()

# The conformance gate emits no report; its gate is that it compiled at all.
sol_add_prototype(Cpp23Conformance)
add_test(NAME Cpp23Conformance COMMAND Cpp23Conformance)
set_tests_properties(Cpp23Conformance PROPERTIES LABELS "p1a;a1")

# Guards the harness itself. Every later measurement is only as trustworthy as this.
sol_add_prototype(HarnessSelfCheck)
add_test(NAME HarnessSelfCheck COMMAND HarnessSelfCheck)
set_tests_properties(HarnessSelfCheck PROPERTIES LABELS "p1a;a1;harness")

sol_add_scenario(ToolchainReport)
sol_add_scenario(DeterminismSmoke)

# A1 also carried TimingScenario, a synthetic transform loop that existed to prove the
# measurement pipeline end to end. A1 marked it disposable and expected A2 to replace it;
# FrameModelCost now does that job against real conversions, so it was removed with user
# approval on 2026-08-12. Its evidence and rationale remain in evidence/p1a/A1/.

# --------------------------------------------------------------------------------------
# Increment A2 -- reference frames and numerical precision.
# --------------------------------------------------------------------------------------

# Guards the frame library, as HarnessSelfCheck guards the harness. Emits no report: its gate
# is that every asserted property of the library holds.
sol_add_frames_prototype(FramesSelfCheck)
add_test(NAME FramesSelfCheck COMMAND FramesSelfCheck)
set_tests_properties(FramesSelfCheck PROPERTIES LABELS "p1a;a2;frames")

# The ADR 0008 reference-data generation record.
sol_add_frames_scenario(ReferenceFixtures)

# The accepted frame-conversion and conversion-drift thresholds.
sol_add_frames_scenario(FrameRoundTrip)

# Per-boundary conversion error along a surface-to-200-km trajectory.
sol_add_frames_scenario(AscentSampling)

# The measured precision budget.
sol_add_frames_scenario(PrecisionBudget)

# Conversion cost, for P1b to budget against.
sol_add_frames_scenario(FrameModelCost)

# --------------------------------------------------------------------------------------
# Increment A3 -- hybrid orbit and time warp.
# --------------------------------------------------------------------------------------

#[[
Declares an increment A3 prototype, which links the orbit library on top of the frame library.

Kept separate from sol_add_frames_prototype for the same reason that one was kept separate from
sol_add_prototype: a target must not acquire a dependency by accident, and A2's targets must
stay buildable and runnable without anything A3 added.
]]
function(sol_add_orbit_prototype name)
    add_executable(${name} ${name}/Main.cpp)
    sol_apply_project_options(${name})
    target_link_libraries(${name} PRIVATE sol::proto::harness sol::proto::orbit)
endfunction()

function(sol_add_orbit_scenario name)
    sol_add_orbit_prototype(${name})
    add_test(NAME ${name}
             COMMAND ${name} --out "${SOL_SCENARIO_OUTPUT_DIR}/${name}.json")
    set_tests_properties(${name} PROPERTIES LABELS "p1a;a3")
endfunction()

# Guards the orbit library, as FramesSelfCheck guards the frame library. Emits no report.
sol_add_orbit_prototype(OrbitSelfCheck)
add_test(NAME OrbitSelfCheck COMMAND OrbitSelfCheck)
set_tests_properties(OrbitSelfCheck PROPERTIES LABELS "p1a;a3;orbit")

# The accepted 100 m one-orbit gate, and the long-run energy behaviour it cannot see.
sol_add_orbit_scenario(ReferenceOrbit)

# The accepted 1 m / 1 mm/s handoff gate, and the rejection coverage A3 requires.
sol_add_orbit_scenario(HybridHandoff)

# Sphere-of-influence entry and exit: the only structurally nonzero transition in the contract.
sol_add_orbit_scenario(SoiCrossing)

# The accepted warp-equivalence gate, and ADR 0011's own no-decay validation item.
sol_add_orbit_scenario(WarpEquivalence)

# A3's numbers are physical and its results sections carry no timing except ReferenceOrbit's,
# so the handoff, crossing, and warp scenarios are pinned across process launches exactly as
# A2 pinned FrameRoundTrip.
foreach(scenario HybridHandoff SoiCrossing WarpEquivalence)
    add_test(NAME ${scenario}AcrossRuns
             COMMAND "${CMAKE_COMMAND}"
                     "-DSOL_SCENARIO_EXE=$<TARGET_FILE:${scenario}>"
                     "-DSOL_WORK_DIR=${SOL_SCENARIO_OUTPUT_DIR}/${scenario}-across-runs"
                     -P "${CMAKE_SOURCE_DIR}/cmake/RunDeterminismCheck.cmake")
    set_tests_properties(${scenario}AcrossRuns PROPERTIES LABELS "p1a;a3;determinism")
endforeach()

# A2's numbers are physical, so they are worth pinning across runs. A1 deferred this
# deliberately: pinning goldens against an arbitrary synthetic workload would have proved
# nothing. FrameRoundTrip contains no timing, so its entire results section is deterministic.
add_test(NAME FrameRoundTripAcrossRuns
         COMMAND "${CMAKE_COMMAND}"
                 "-DSOL_SCENARIO_EXE=$<TARGET_FILE:FrameRoundTrip>"
                 "-DSOL_WORK_DIR=${SOL_SCENARIO_OUTPUT_DIR}/frame-round-trip-across-runs"
                 -P "${CMAKE_SOURCE_DIR}/cmake/RunDeterminismCheck.cmake")
set_tests_properties(FrameRoundTripAcrossRuns PROPERTIES LABELS "p1a;a2;determinism")

# The cross-run determinism gate. Separate from the DeterminismSmoke test above, which only
# proves in-process stability; this one proves bit-exactness across two process launches.
add_test(NAME DeterminismAcrossRuns
         COMMAND "${CMAKE_COMMAND}"
                 "-DSOL_SCENARIO_EXE=$<TARGET_FILE:DeterminismSmoke>"
                 "-DSOL_WORK_DIR=${SOL_SCENARIO_OUTPUT_DIR}/determinism-across-runs"
                 -P "${CMAKE_SOURCE_DIR}/cmake/RunDeterminismCheck.cmake")
set_tests_properties(DeterminismAcrossRuns PROPERTIES LABELS "p1a;a1;determinism")
