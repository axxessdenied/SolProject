# P1a technical-risk prototypes.
#
# Per the P1a milestone plan these executables are disposable: they exist to produce
# evidence, not to become SolEngine. Nothing here may be promoted into engine/ or game/
# without an explicit review. The harness library is the exception the plan anticipates --
# it is durable, and it is kept free of any simulation or domain concept so that promoting
# it later stays a real option rather than a sunk cost.

add_subdirectory(Harness)

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
sol_add_scenario(TimingScenario)

# The cross-run determinism gate. Separate from the DeterminismSmoke test above, which only
# proves in-process stability; this one proves bit-exactness across two process launches.
add_test(NAME DeterminismAcrossRuns
         COMMAND "${CMAKE_COMMAND}"
                 "-DSOL_SCENARIO_EXE=$<TARGET_FILE:DeterminismSmoke>"
                 "-DSOL_WORK_DIR=${SOL_SCENARIO_OUTPUT_DIR}/determinism-across-runs"
                 -P "${CMAKE_SOURCE_DIR}/cmake/RunDeterminismCheck.cmake")
set_tests_properties(DeterminismAcrossRuns PROPERTIES LABELS "p1a;a1;determinism")
