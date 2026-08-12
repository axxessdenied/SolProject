# SolProject root build description.
#
# Scope: this build graph currently contains only the P1a increment A1 measurement
# harness and its disposable headless prototypes. The engine/, game/, editor/, and
# tests/ trees described in AGENTS.md do not exist yet and are not created here.
#
# Governing decisions:
#   ADR 0001 - C++23, MSVC, CMake, single-config Ninja, explicit source lists.
#   ADR 0010 - /fp:precise, contraction policy, /arch:AVX2, never /fp:fast.

cmake_minimum_required(VERSION 3.28)

project(SolProject
    VERSION 0.0.0
    DESCRIPTION "SolEngine and Frontiers of Sol"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Configure through a checked-in preset, "
        "for example: cmake --preset windows-msvc-debug")
endif()

if(NOT CMAKE_BUILD_TYPE)
    message(FATAL_ERROR
        "CMAKE_BUILD_TYPE is empty. ADR 0001 requires separate single-configuration "
        "Ninja trees; configure through a checked-in preset.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(SolProjectOptions)
include(SolToolchainFacts)

sol_generate_toolchain_facts()

enable_testing()

add_subdirectory(prototypes/p1a)
