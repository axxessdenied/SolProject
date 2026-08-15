cmake_minimum_required(VERSION 3.28)

project(Sol
    VERSION 0.1.0
    DESCRIPTION "Sol Engine and game - single-player 3D space sandbox"
    LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset dev")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++20 modules for now (see engine plan)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake injects /EHsc by default; drop it so our /EHs-c- doesn't trigger D9025.
if(MSVC)
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# Shared compile settings for all first-party targets.
# third_party code must never link this target (see AGENTS.md section 5).
add_library(sol_options INTERFACE)
if(MSVC)
    target_compile_options(sol_options INTERFACE
        /W4 /WX
        /permissive-
        /EHs-c-     # exceptions off
        /GR-        # RTTI off
        /utf-8
    )
    target_compile_definitions(sol_options INTERFACE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(sol_options INTERFACE
        -Wall -Wextra -Werror
        -fno-exceptions
        -fno-rtti
    )
endif()

enable_testing()

add_subdirectory(engine)
add_subdirectory(game)
