# Runs a scenario executable twice and compares the deterministic half of its output.
#
# ADR 0010 asserts bit-exact reproduction on the same build and machine. Proving that needs
# two separate process launches: an in-process repeat would share warmed caches, an already
# initialised runtime, and one set of one-time initialisation, and so could pass while a
# genuine cross-run difference existed.
#
# Only the report's "results" section is compared. The "environment" section holds a
# timestamp and an output path that differ between runs by design. Comparing whole files
# would fail every time; the tempting alternative -- diffing while skipping "some" lines --
# would also skip real numerical changes, which is the failure this check exists to catch.
#
# Invoked as:
#   cmake -DSOL_SCENARIO_EXE=<path> -DSOL_WORK_DIR=<dir> -P RunDeterminismCheck.cmake

if(NOT DEFINED SOL_SCENARIO_EXE)
    message(FATAL_ERROR "RunDeterminismCheck: SOL_SCENARIO_EXE is required")
endif()
if(NOT DEFINED SOL_WORK_DIR)
    message(FATAL_ERROR "RunDeterminismCheck: SOL_WORK_DIR is required")
endif()

file(MAKE_DIRECTORY "${SOL_WORK_DIR}")

set(_sol_first "${SOL_WORK_DIR}/run-a.json")
set(_sol_second "${SOL_WORK_DIR}/run-b.json")

foreach(_sol_pass RANGE 1 2)
    if(_sol_pass EQUAL 1)
        set(_sol_output "${_sol_first}")
    else()
        set(_sol_output "${_sol_second}")
    endif()

    execute_process(
        COMMAND "${SOL_SCENARIO_EXE}" --out "${_sol_output}"
        RESULT_VARIABLE _sol_result
        OUTPUT_VARIABLE _sol_stdout
        ERROR_VARIABLE _sol_stderr)

    if(NOT _sol_result EQUAL 0)
        message(FATAL_ERROR
            "RunDeterminismCheck: run ${_sol_pass} failed with exit code ${_sol_result}\n"
            "stdout: ${_sol_stdout}\n"
            "stderr: ${_sol_stderr}")
    endif()
endforeach()

file(READ "${_sol_first}" _sol_first_text)
file(READ "${_sol_second}" _sol_second_text)

string(JSON _sol_first_results ERROR_VARIABLE _sol_first_error GET "${_sol_first_text}" results)
if(_sol_first_error AND NOT _sol_first_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "RunDeterminismCheck: cannot read results from ${_sol_first}: ${_sol_first_error}")
endif()

string(JSON _sol_second_results ERROR_VARIABLE _sol_second_error GET "${_sol_second_text}" results)
if(_sol_second_error AND NOT _sol_second_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "RunDeterminismCheck: cannot read results from ${_sol_second}: ${_sol_second_error}")
endif()

if(NOT _sol_first_results STREQUAL _sol_second_results)
    file(WRITE "${SOL_WORK_DIR}/results-a.json" "${_sol_first_results}")
    file(WRITE "${SOL_WORK_DIR}/results-b.json" "${_sol_second_results}")
    message(FATAL_ERROR
        "RunDeterminismCheck: FAILED. Two runs of the same build produced different results, "
        "violating ADR 0010's same-machine bit-exactness guarantee.\n"
        "Differing sections were written to:\n"
        "  ${SOL_WORK_DIR}/results-a.json\n"
        "  ${SOL_WORK_DIR}/results-b.json")
endif()

# Guard against the check passing vacuously: an empty or trivially small results section
# would compare equal while proving nothing.
string(LENGTH "${_sol_first_results}" _sol_results_length)
if(_sol_results_length LESS 100)
    message(FATAL_ERROR
        "RunDeterminismCheck: results section is only ${_sol_results_length} characters. "
        "The comparison would pass without exercising anything.")
endif()

message(STATUS
    "RunDeterminismCheck: PASSED. Two runs produced identical results sections "
    "(${_sol_results_length} characters).")
