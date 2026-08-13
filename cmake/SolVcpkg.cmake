# Resolves the vcpkg toolchain file before project() runs.
#
# ADR 0007 selects vcpkg manifest mode with a checked-in builtin-baseline. This module only
# locates the vcpkg *tool*; which packages and versions are used is decided entirely by the
# checked-in vcpkg.json, so two machines with different vcpkg clones still resolve the same
# dependency graph.
#
# This must be included before project(). CMAKE_TOOLCHAIN_FILE has no effect afterwards.

include_guard(GLOBAL)

# Static third-party libraries against the dynamic CRT. Static libraries mean no third-party
# DLLs travel with the game (ADR 0005 already makes SolEngine itself static); the dynamic CRT
# keeps us on the same runtime as the OS-supplied Vulkan loader and avoids the allocator
# mismatches a static CRT invites across module boundaries.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    return()
endif()

set(_sol_vcpkg_roots "")
if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _sol_vcpkg_env)
    list(APPEND _sol_vcpkg_roots "${_sol_vcpkg_env}")
endif()
list(APPEND _sol_vcpkg_roots "C:/vcpkg")

foreach(_sol_root IN LISTS _sol_vcpkg_roots)
    if(EXISTS "${_sol_root}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${_sol_root}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "vcpkg toolchain")
        set(SOL_VCPKG_ROOT "${_sol_root}" CACHE INTERNAL "")
        message(STATUS "Sol: vcpkg at ${_sol_root}, triplet ${VCPKG_TARGET_TRIPLET}")
        return()
    endif()
endforeach()

message(FATAL_ERROR
    "vcpkg was not found. ADR 0007 makes vcpkg manifest mode the acquisition mechanism for "
    "every C/C++ dependency, and vcpkg.json in this repository declares them.\n"
    "  Searched: VCPKG_ROOT environment variable, then C:/vcpkg\n"
    "  Install:  git clone https://github.com/microsoft/vcpkg C:/vcpkg\n"
    "            C:/vcpkg/bootstrap-vcpkg.bat -disableMetrics\n"
    "No admin rights are required and the clone is self-contained.")
