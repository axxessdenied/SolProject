# Generates a header describing the exact build that produced a binary.
#
# The P1a shared measurement rules require every report to record commit, preset and
# toolchain, and build settings. Baking those into the binary is the only way a scenario's
# own output can be trusted to describe itself.
#
# Known limitation: these facts are captured at *configure* time. A commit made after
# configuring and before running a scenario will be misreported. Evidence collection
# therefore always starts from a fresh configure of a clean tree.

include_guard(GLOBAL)

function(sol_generate_toolchain_facts)
    set(_sol_commit "unknown")
    set(_sol_dirty "true")

    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _sol_commit_out
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _sol_commit_result)
        if(_sol_commit_result EQUAL 0 AND _sol_commit_out)
            set(_sol_commit "${_sol_commit_out}")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _sol_status_out
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _sol_status_result)
        if(_sol_status_result EQUAL 0)
            if(_sol_status_out STREQUAL "")
                set(_sol_dirty "false")
            endif()
        endif()
    endif()

    # The preset name is not exposed to CMake, so presets set it explicitly.
    if(NOT DEFINED SOL_BUILD_PRESET OR SOL_BUILD_PRESET STREQUAL "")
        set(SOL_BUILD_PRESET "unknown-not-configured-through-a-preset")
    endif()

    set(SOL_GIT_COMMIT "${_sol_commit}")
    set(SOL_GIT_DIRTY "${_sol_dirty}")
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _sol_config_upper)
    set(SOL_FULL_CXX_FLAGS
        "${SOL_CXX_STANDARD_FLAG} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_sol_config_upper}} ${SOL_PROJECT_COMPILE_OPTIONS_TEXT}")
    string(STRIP "${SOL_FULL_CXX_FLAGS}" SOL_FULL_CXX_FLAGS)
    string(REGEX REPLACE " +" " " SOL_FULL_CXX_FLAGS "${SOL_FULL_CXX_FLAGS}")

    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SolToolchainFacts.h.in"
        "${CMAKE_BINARY_DIR}/generated/Sol/Proto/Harness/SolToolchainFacts.h"
        @ONLY)

    add_library(SolToolchainFacts INTERFACE)
    add_library(sol::proto::toolchainFacts ALIAS SolToolchainFacts)
    target_include_directories(SolToolchainFacts INTERFACE "${CMAKE_BINARY_DIR}/generated")
endfunction()
