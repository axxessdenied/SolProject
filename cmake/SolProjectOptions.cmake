# Compiler policy for project-owned targets.
#
# ADR 0003 governs C++ identifier naming. CMake is a separate language, so this file
# uses CMake's conventional lower_snake_case with a `sol_` prefix.
#
# This module deliberately *probes* rather than assumes. ADR 0010 states that the
# values recorded during increment A1 are authoritative over the ADR's prose, so the
# flags that survive probing here are the project's real floating-point policy.

include_guard(GLOBAL)
include(CheckCXXCompilerFlag)

if(NOT MSVC)
    message(FATAL_ERROR
        "ADR 0001 selects MSVC on Windows x64 as the project compiler. "
        "Detected '${CMAKE_CXX_COMPILER_ID}' instead.")
endif()

# --------------------------------------------------------------------------------------
# ADR 0010: reject /fp:fast anywhere it could reach a project-owned target.
# --------------------------------------------------------------------------------------
foreach(_sol_flag_var
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS_MINSIZEREL)
    if("${${_sol_flag_var}}" MATCHES "[/-]fp:fast")
        message(FATAL_ERROR
            "ADR 0010 forbids /fp:fast on project-owned targets, but it appears in "
            "${_sol_flag_var}='${${_sol_flag_var}}'.")
    endif()
endforeach()

# --------------------------------------------------------------------------------------
# ADR 0010: FMA contraction must be off for authoritative floating-point results.
#
# The ADR asks for contraction to be disabled *explicitly* rather than by relying on a
# default. Probe for a disabling spelling; if the toolset provides none, fall back to
# the /fp:precise default and require the increment A1 negative control to prove that
# the default is actually contraction-free. Never silently assume it.
# --------------------------------------------------------------------------------------
set(SOL_FP_CONTRACTION_FLAG "" CACHE INTERNAL "Explicit FMA-contraction disable flag, if the toolset has one")
set(SOL_FP_CONTRACTION_MODE "" CACHE INTERNAL "How FMA contraction is disabled on this toolset")

foreach(_sol_candidate "/fp:contract-" "/fp:no-contract")
    if(SOL_FP_CONTRACTION_FLAG)
        continue()
    endif()
    string(MAKE_C_IDENTIFIER "SOL_HAVE_FLAG_${_sol_candidate}" _sol_probe_var)
    check_cxx_compiler_flag("${_sol_candidate}" ${_sol_probe_var})
    if(${_sol_probe_var})
        set(SOL_FP_CONTRACTION_FLAG "${_sol_candidate}" CACHE INTERNAL "" FORCE)
        set(SOL_FP_CONTRACTION_MODE "explicit-flag" CACHE INTERNAL "" FORCE)
    endif()
endforeach()

if(SOL_FP_CONTRACTION_FLAG)
    message(STATUS "Sol: FMA contraction disabled explicitly via ${SOL_FP_CONTRACTION_FLAG}")
else()
    set(SOL_FP_CONTRACTION_MODE "implicit-off-under-fp-precise" CACHE INTERNAL "" FORCE)
    message(STATUS
        "Sol: this toolset accepts no contraction-disabling flag spelling. Relying on "
        "/fp:precise, under which contraction is opt-in via /fp:contract. The A1 negative "
        "control must demonstrate that adding /fp:contract changes numerical output.")
endif()

# --------------------------------------------------------------------------------------
# Record the literal standard-selection flag CMake will emit for C++23 on this toolset.
# CMake owns this mapping; we observe it rather than hand-writing a /std: flag.
# --------------------------------------------------------------------------------------
if(DEFINED CMAKE_CXX23_STANDARD_COMPILE_OPTION)
    set(SOL_CXX_STANDARD_FLAG "${CMAKE_CXX23_STANDARD_COMPILE_OPTION}" CACHE INTERNAL "")
else()
    message(FATAL_ERROR "CMake reports no C++23 standard flag for this compiler; ADR 0001 requires C++23.")
endif()
message(STATUS "Sol: C++23 standard flag is '${SOL_CXX_STANDARD_FLAG}'")

# --------------------------------------------------------------------------------------
# The single flag set applied to every project-owned target.
# --------------------------------------------------------------------------------------
set(SOL_PROJECT_COMPILE_OPTIONS
    /fp:precise         # ADR 0010: authoritative floating point
    /arch:AVX2          # ADR 0010: baseline is Haswell-era Intel / Zen-era AMD
    /permissive-        # standard conformance
    /Zc:__cplusplus     # report a truthful __cplusplus so feature detection works
    /Zc:preprocessor    # conforming preprocessor
    /utf-8              # deterministic source and execution charset
    /W4
    /WX)

if(SOL_FP_CONTRACTION_FLAG)
    list(APPEND SOL_PROJECT_COMPILE_OPTIONS "${SOL_FP_CONTRACTION_FLAG}")
endif()

# Increment A1 negative control (ADR 0010 validation): configure with
# -DSOL_NEGATIVE_CONTROL_CONTRACT=ON to enable FMA contraction and confirm that the
# determinism digest changes. Never enable this in a build that produces evidence.
option(SOL_NEGATIVE_CONTROL_CONTRACT
    "ADR 0010 negative control: enable /fp:contract to prove the determinism harness detects a flag change"
    OFF)
if(SOL_NEGATIVE_CONTROL_CONTRACT)
    list(APPEND SOL_PROJECT_COMPILE_OPTIONS /fp:contract)
    message(WARNING
        "SOL_NEGATIVE_CONTROL_CONTRACT is ON: /fp:contract is enabled. This build "
        "violates ADR 0010 by design and must not be used to produce evidence.")
endif()

string(JOIN " " SOL_PROJECT_COMPILE_OPTIONS_TEXT ${SOL_PROJECT_COMPILE_OPTIONS})
set(SOL_PROJECT_COMPILE_OPTIONS_TEXT "${SOL_PROJECT_COMPILE_OPTIONS_TEXT}" CACHE INTERNAL "")

#[[
Apply the project-wide language, floating-point, and warning policy to one target.

Every project-owned target must call this. It is the only place compile options are set,
so the flag string baked into the toolchain-facts header is guaranteed to match what the
compiler actually received.
]]
function(sol_apply_project_options target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "sol_apply_project_options: '${target}' is not a target")
    endif()

    target_compile_features(${target} PUBLIC cxx_std_23)

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    target_compile_options(${target} PRIVATE ${SOL_PROJECT_COMPILE_OPTIONS})

    # Windows headers: keep the surface minimal and free of the min/max macros.
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE)

    if(SOL_NEGATIVE_CONTROL_CONTRACT)
        target_compile_definitions(${target} PRIVATE SOL_NEGATIVE_CONTROL_CONTRACT_ENABLED)
    endif()
endfunction()
