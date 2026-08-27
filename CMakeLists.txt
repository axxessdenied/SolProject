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

# Phase 22. Dev builds bake absolute source-tree paths so defs, mods and shaders
# hot-reload from the tree; a shipping build must NOT, or the binary reads
# directories that exist on exactly one machine in the world - and it works
# perfectly on that machine, which is packaging's characteristic failure. The
# `release` presets turn this OFF, which is what makes that outcome structurally
# impossible for a packaged binary rather than merely unlikely. It also stops the
# build machine's absolute paths shipping as strings inside the executable.
option(SOL_DEV_DATA_PATHS
    "Bake absolute source-tree paths for hot-reload (dev builds only)" ON)

# Phase 22. ONE C runtime for every configuration. The default is the dynamic
# CRT, which makes a packaged game depend on the VC++ redistributable already
# being installed - exactly the friction a portable archive exists to remove.
# ⚑ MUST be set before any target is created: CMP0091 turns this into a target
# property captured at add_library/add_executable time, so a target declared
# above this line would silently keep the dynamic runtime and fail to link
# against the rest.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    target_compile_definitions(sol_options INTERFACE
        _HAS_EXCEPTIONS=0
        _CRT_SECURE_NO_WARNINGS # portable C stdio (fopen et al.) without MSVC nagging
        UNICODE
        _UNICODE
    )
else()
    target_compile_options(sol_options INTERFACE
        -Wall -Wextra -Werror
        # ⚑ Phase 21. -Wextra turns this on and it fires on a C++20 idiom this
        # codebase uses everywhere on purpose: a designated initializer that
        # names some members and lets the rest value-initialize, which is
        # well-defined and is the whole point of writing `{.kind = X}` instead
        # of listing every field. Twenty-one sites tripped it on the first
        # Linux build and not one was a bug. Silencing the warning is the fix;
        # adding `.position = {}` to each call site would churn working code
        # into something less readable, forever, to satisfy a diagnostic that
        # MSVC does not even raise.
        -Wno-missing-field-initializers
        # ⚑⚑ Phase 22. BOTH OF THESE REQUIRE OPTIMISATION TO FIRE AT ALL, so
        # neither had ever been active in this project: linux-dev is -O0 and
        # Windows is MSVC. The first RelWithDebInfo build on Linux turned them
        # on, and both immediately produced FALSE POSITIVES inside libstdc++
        # headers - the well-known GCC 12/13 family where the optimizer loses a
        # bound through std::vector's allocator inlining chain. Each was read
        # rather than assumed:
        #
        #   -Wstringop-overflow, gpu_profiler.cpp: a push_back into a vector
        #   that provably never reallocates - reserve(kQueriesPerFrame) at
        #   construction, and BOTH call sites return early once queryCount
        #   reaches that same bound. GCC's own note gives it away: "region of
        #   size 9223372036854775792" at "offset -4".
        #
        #   -Warray-bounds, mining_tests.cpp: rocks[1] on a ONE-element vector,
        #   inside a loop bounded by rocks.size() whose single iteration hits
        #   `continue` because i == avoid. GCC speculated an iteration that
        #   cannot execute and then warned about the access it had invented.
        #
        # Neither is fixable in first-party code - the diagnostics point into
        # /usr/include/c++/13 and the bounds they ask for are already enforced,
        # twice over in the first case. Silencing them is the same call the
        # line above makes for the same reason: do not churn correct code to
        # satisfy a diagnostic. ⚑ The honest cost is that these two warnings
        # are real bug-finders in general - but the coverage lost is coverage
        # this project never had, since no build here ever ran them.
        -Wno-stringop-overflow
        -Wno-array-bounds
        -fno-exceptions
        -fno-rtti
    )
    # Phase 22. The Linux counterpart of the static CRT above, and it buys a
    # measured thing: before this the binary required GLIBCXX_3.4.31, which is
    # a GCC 13 runtime. Linking libstdc++ and libgcc statically removes that
    # floor entirely. ⚑ It does NOT remove the glibc floor (GLIBC_2.38 here,
    # i.e. Ubuntu 24.04+) - that belongs to the build host and cannot be
    # lowered without an older build container. See engine plan Phase 22.
    # Reaches the final link because sol_core links sol_options PUBLIC and
    # everything depends on sol::core.
    target_link_options(sol_options INTERFACE -static-libstdc++ -static-libgcc)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(engine)
add_subdirectory(tools/cooker)
add_subdirectory(shaders)
add_subdirectory(game)
# After game/: the Forge depends on sol_cook_assets, which game/ defines.
add_subdirectory(tools/forge)

# ---------------------------------------------------------------------------
# Packaging (Phase 22 stage C). A portable archive per platform: unpack
# anywhere, run, delete the folder to uninstall. No installer - see the engine
# plan's Phase 22 decision 1 for why, and for what an installer would have
# dragged in that this tree does not have.
#
# ⚑⚑ ONLY IN A SHIPPING CONFIGURATION, AND THAT GUARD IS THE POINT. Packaging
# a dev build would produce an archive whose binary reads THIS machine's source
# tree - and it would run perfectly here and nowhere else, which is exactly the
# failure decision 6 exists to make impossible. Refusing to configure CPack at
# all is louder than producing a broken archive.
# ---------------------------------------------------------------------------
if(SOL_DEV_DATA_PATHS)
    message(STATUS "Packaging disabled: this is a dev configuration (SOL_DEV_DATA_PATHS=ON). Use the release presets.")
else()
    set(CPACK_PACKAGE_NAME "TheStarsDontWait")
    set(CPACK_PACKAGE_VENDOR "Nick Slusarczyk")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Carve out a life in a galaxy already in motion.")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
    set(CPACK_PACKAGE_DIRECTORY ${CMAKE_BINARY_DIR}/package)

    # ⚑ Strips the installed binaries. Meaningless on MSVC, which keeps debug
    # info in a .pdb that is never installed - and load-bearing on GCC, where
    # -g embeds it in the ELF: measured 37 MB unstripped against 4.8 MB
    # stripped, for the same game. Setting it here is what stops that being a
    # flag somebody has to remember.
    set(CPACK_STRIP_FILES ON)

    if(WIN32)
        set(CPACK_GENERATOR ZIP)
        set(CPACK_PACKAGE_FILE_NAME "TheStarsDontWait-${PROJECT_VERSION}-win64")
    else()
        set(CPACK_GENERATOR TGZ)
        set(CPACK_PACKAGE_FILE_NAME "TheStarsDontWait-${PROJECT_VERSION}-linux-x86_64")
    endif()

    # The archive keeps its top-level directory (CPack's default for archive
    # generators): unpacking gives one folder rather than spraying 56 files
    # into whatever directory the player happened to be in.
    include(CPack)
endif()
