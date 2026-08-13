# SolProject root build description.
#
# Scope: the disposable P1a headless prototypes under prototypes/p1a/, plus the production
# engine/ and tests/ trees started by P1b increment B1. game/ and editor/ do not exist yet.
#
# P1b increment B1 is built in the production tree by user decision recorded in the P1b
# milestone plan: a working Vulkan bootstrap is never realistically discarded, so it is held
# to production standards from the first commit rather than at a later cleanup.
#
# Governing decisions:
#   ADR 0001 - C++23, MSVC, CMake, single-config Ninja, explicit source lists.
#   ADR 0002 - Vulkan 1.2 candidate floor; Vulkan types stay behind renderer-owned interfaces.
#   ADR 0007 - vcpkg manifest mode with a checked-in builtin-baseline.
#   ADR 0010 - /fp:precise, contraction policy, /arch:AVX2, never /fp:fast.

cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Must precede project(): CMAKE_TOOLCHAIN_FILE is ignored once the project is declared.
include(SolVcpkg)

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

include(SolProjectOptions)
include(SolShaders)
include(SolToolchainFacts)
include(SolVulkanSdk)

sol_generate_toolchain_facts()
sol_find_vulkan_sdk()

enable_testing()

add_subdirectory(prototypes/p1a)
add_subdirectory(engine)
add_subdirectory(tests)
