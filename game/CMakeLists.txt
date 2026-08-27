# Everything except the entry point lives in a library, so the game's own
# logic - targeting, the map's rows, the screens' fill/execute seam - can be
# reached by a test binary. Before this split `add_executable` held the whole
# tree and every suite linked an engine/ library, so nothing under game/src
# was testable at all: the map and targeting code had no regression test not
# through neglect but because there was no target that could see it. Same
# structural cause as `encodeMesh` sitting in the cooker's main.cpp outside
# sol_cooker_lib, and the same fix.
add_library(sol_game_lib STATIC
    src/combat_effects.cpp
    src/content.cpp
    src/game_audio.cpp
    src/game_ui.cpp
    src/input_actions.cpp
    src/map_screen.cpp
    src/map_ui.cpp
    src/ship_screen.cpp
    src/ship_ui.cpp
    src/menu_screens.cpp
    src/scene_renderer.cpp
    src/shader_watcher.cpp
    src/space_world.cpp
    src/station_screen.cpp
    src/station_ui.cpp
    src/target_pick.cpp
    src/thruster_particles.cpp
)
add_library(sol::game ALIAS sol_game_lib)

# PUBLIC: a test includes these headers by their bare name the same way the
# game's own sources include each other.
target_include_directories(sol_game_lib PUBLIC src)

target_link_libraries(sol_game_lib PUBLIC
    sol::core
    sol::ecs
    sol::sim
    sol::scripting
    sol::platform
    sol::rhi
    sol::renderer
    sol::assets
    sol::audio
    sol::ui
)

add_executable(sol_game src/main.cpp)
set_target_properties(sol_game PROPERTIES OUTPUT_NAME sol)

target_link_libraries(sol_game PRIVATE sol_game_lib)

# Data defs + Lua scripts load straight from the source tree in dev builds so
# edits hot-reload; shipping layouts fall back to dirs beside the executable
# (`main.cpp` picks the fallback when the define is absent or empty).
#
# ⚑ Phase 22 made these CONDITIONAL. They were unconditional for seventeen
# phases, so no build ever took the fallback and the shipping path had never
# once been executed - the branch was written, reviewed, and dead.
if(SOL_DEV_DATA_PATHS)
    target_compile_definitions(sol_game PRIVATE
        SOL_DATA_SOURCE_DIR="${CMAKE_SOURCE_DIR}/game/data"
        SOL_MODS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/game/mods"
        # ⚑⚑ Phase 23. "This build reads the source tree" and "this is a dev
        # build" are the same fact, so they share ONE branch and cannot drift
        # apart. Deliberately not NDEBUG: a developer profiling a
        # RelWithDebInfo build WITH dev paths must keep their overlay, and
        # Phases 8n/8o are exactly that workflow.
        SOL_DEV_BUILD=1
    )
endif()

# Shader hot-reload (dev): recompile changed GLSL at runtime with glslc.
#
# ⚑⚑ Phase 22. This pair is gated on SOL_DEV_DATA_PATHS too, and it is the pair
# nobody had counted - the roadmap named only the data/mods defines. It was
# baked into every build on any machine with the Vulkan SDK, including release.
# It never broke anything (ShaderWatcher gets an empty listFiles, leaves itself
# unavailable and says so) - it simply shipped the build machine's absolute
# paths, including the developer's user name, as strings inside the binary.
if(SOL_GLSLC AND SOL_DEV_DATA_PATHS)
    target_compile_definitions(sol_game PRIVATE
        SOL_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders"
        SOL_GLSLC_PATH="${SOL_GLSLC}"
    )
endif()

add_dependencies(sol_game sol_shaders sol_cook_assets)

# Cook source assets into the runtime directory on every build; the cooker
# itself skips files whose outputs are already up to date.
add_custom_target(sol_cook_assets
    COMMAND $<TARGET_FILE:sol_cooker> ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/bin/cooked
    COMMENT "Cooking assets"
    VERBATIM
)
add_dependencies(sol_cook_assets sol_cooker)

# ---------------------------------------------------------------------------
# The shipping layout (Phase 22).
#
# Until this block existed there was no statement anywhere in the tree of what
# "the game" IS as a set of files - only of what the build tree happened to
# contain, which is why build/release/bin/ has always held cooked/ and shaders/
# (custom targets generate them there) and never data/ or mods/ (nothing
# copies them, and nothing ever did).
#
# ⚑ The exclusions are enforced by omission and that is deliberate: the cooker,
# the Forge and every test binary are absent because they are never named here.
# AGENTS.md 5 keeps ImGui to dev tooling and tools/forge/CMakeLists.txt says
# the shipping binary carries no editor - this is where those stop being
# comments and become something a test can assert (see package.layout).
# ---------------------------------------------------------------------------

install(TARGETS sol_game RUNTIME DESTINATION .)

# Authored content. ⚑ `mods/` ships empty-but-present on purpose: listFiles
# cannot tell a missing directory from an empty one, so a player with no mods
# and a player whose mods directory vanished would see identical behaviour.
# Shipping the directory is what makes the layer visible.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/game/data/ DESTINATION data)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/game/mods/ DESTINATION mods)

# ⚑⚑ Generated payload, and the reason it needs a guard rather than a rule.
# cooked/ and shaders/ are produced INTO the build tree by sol_cook_assets and
# sol_shaders, so at install time they are merely directories that may or may
# not exist yet. install(DIRECTORY) over a missing source is NOT an error - it
# creates an empty destination and says nothing, which yields a package that
# unpacks cleanly and then dies at first load on somebody else's machine. That
# is the exact failure mode packaging is prone to, so refuse instead, naming
# the directory: a refusal that names the file beats a silent fallback.
set(solPayloadGuard [[
if(NOT EXISTS "@solPayloadDir@")
    message(FATAL_ERROR "Nothing to install from @solPayloadDir@ - build the sol_game target first (it depends on sol_cook_assets and sol_shaders).")
endif()
]])
foreach(solPayloadName cooked shaders)
    set(solPayloadDir ${CMAKE_BINARY_DIR}/bin/${solPayloadName})
    string(CONFIGURE "${solPayloadGuard}" solPayloadCheck @ONLY)
    install(CODE "${solPayloadCheck}")
endforeach()
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/cooked/ DESTINATION cooked)
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/shaders/ DESTINATION shaders)

# What a distribution has to carry and this repo did not have: the third-party
# notices MIT requires to ACCOMPANY A BINARY (third_party/ is not distributed,
# so neither Lua's nor Dear ImGui's notice reached a player), and a README
# written for someone who wants to play rather than to build.
#
# ⚑ The requirements differ per platform and the Linux one is a measured
# claim, not a polite guess: the binary needs GLIBC_2.38, so "Linux" would be
# false and "Ubuntu 24.04 or newer" is true.
if(WIN32)
    set(SOL_PACKAGE_PLATFORM "Windows 10 or newer, 64-bit")
    set(SOL_PACKAGE_REQUIREMENTS
        "A GPU and driver supporting Vulkan 1.3. Nothing else: the C runtime is\nlinked statically, so no Visual C++ redistributable is required.")
    set(SOL_PACKAGE_RUN "Double-click sol.exe.")
else()
    set(SOL_PACKAGE_PLATFORM "Ubuntu 24.04 or newer (glibc 2.38+), 64-bit")
    set(SOL_PACKAGE_REQUIREMENTS
        "A GPU and driver supporting Vulkan 1.3, plus the Vulkan loader and the\nWayland client libraries, which ship with any current desktop install:\n  libvulkan1  libwayland-client0  libwayland-cursor0  libxkbcommon0\nA Wayland session is required; there is no X11 backend.")
    set(SOL_PACKAGE_RUN "Run ./sol from a terminal in this directory.")
endif()
configure_file(${CMAKE_SOURCE_DIR}/packaging/README.txt.in
               ${CMAKE_CURRENT_BINARY_DIR}/README.txt @ONLY)

install(FILES
    ${CMAKE_SOURCE_DIR}/LICENSE
    ${CMAKE_SOURCE_DIR}/packaging/THIRD-PARTY.txt
    ${CMAKE_CURRENT_BINARY_DIR}/README.txt
    DESTINATION .)

# ---------------------------------------------------------------------------
# Tests over the SHIPPING layout (Phase 22 stage C). Only in a shipping
# configuration, because in a dev tree they would test the wrong thing: the
# installed binary would still read the source tree, so package.smoke would
# pass while proving nothing at all.
#
# A CTest fixture rather than test DEPENDS: the install has to happen once,
# before either test, and CTest pulls a fixture's setup in automatically even
# when a label filter would otherwise have excluded it - which is what lets
# `ctest -L gpu` run package.smoke on its own and still get a tree to run.
# ---------------------------------------------------------------------------
if(NOT SOL_DEV_DATA_PATHS)
    set(solPackageTestPrefix ${CMAKE_BINARY_DIR}/package-test)

    add_test(NAME package.install COMMAND ${CMAKE_COMMAND}
        -DSOL_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DSOL_PREFIX=${solPackageTestPrefix}
        -P ${CMAKE_SOURCE_DIR}/packaging/install_fresh.cmake)
    set_tests_properties(package.install PROPERTIES FIXTURES_SETUP sol_package_tree)

    # What the package must contain, and - the half that needs a test at all -
    # what it must not. The exclusions are enforced by omission above, and an
    # omission is invisible to everything except an assertion like this one.
    add_test(NAME package.layout COMMAND ${CMAKE_COMMAND}
        -DSOL_PREFIX=${solPackageTestPrefix}
        -DSOL_EXE_NAME=$<TARGET_FILE_NAME:sol_game>
        -P ${CMAKE_SOURCE_DIR}/packaging/check_layout.cmake)
    set_tests_properties(package.layout PROPERTIES FIXTURES_REQUIRED sol_package_tree)

    # ⚑ The only test that exercises the real resolution path end to end, and
    # the one that would catch a data file which exists in the source tree and
    # never made it into an install rule. It runs the INSTALLED binary, so a
    # baked source path would be the difference between passing here and
    # failing on every other machine.
    add_test(NAME package.smoke
        COMMAND ${solPackageTestPrefix}/$<TARGET_FILE_NAME:sol_game> --frames 120)
    set_tests_properties(package.smoke PROPERTIES
        FIXTURES_REQUIRED sol_package_tree
        LABELS gpu
        WORKING_DIRECTORY ${solPackageTestPrefix})
endif()

# Needs a GPU + Vulkan driver, so it is excluded from the default test presets
# (label "gpu"); run locally with: ctest --preset dev-gpu
#
# ⚑ Phase 22. Dev configurations only, and not out of tidiness: this runs the
# binary AS IT SITS IN THE BUILD TREE, which only works while that tree is
# self-sufficient - i.e. while dev paths are baked. A shipping build looks for
# data/ beside the executable and there is none there, so this test would fail
# for a completely uninteresting reason. `package.smoke` below is its
# replacement in a shipping tree, and it runs the artifact that actually ships.
if(SOL_DEV_DATA_PATHS)
    add_test(NAME game.smoke COMMAND sol_game --frames 120)
    set_tests_properties(game.smoke PROPERTIES LABELS gpu)
endif()

# The game's own logic, headless. Declared here rather than in engine/test/
# because `engine` is configured before `game`, so the library does not exist
# yet at that point. Per AGENTS section 7 this covers sim-adjacent logic -
# targeting, the map's rows - and NOT the renderer, which is still verified by
# running the app.
add_executable(sol_game_tests
    test/keyboard_routing_tests.cpp
    test/map_row_tests.cpp
    test/model_role_tests.cpp
    test/target_cycle_tests.cpp
)
target_link_libraries(sol_game_tests PRIVATE sol::test sol_game_lib)
# Phase 19. The role contract is a statement about THIS game's committed data
# and THIS game's role vocabulary, and only this suite can see both: the
# vocabulary lives in game/src and assets.unit cannot link it, while the data
# lives in game/data and only assets.unit had a path to it. Same split as
# stage H's - the rule goes where a test can reach it, decided before the
# promise rather than after.
target_compile_definitions(sol_game_tests
    PRIVATE SOL_DEF_DATA_DIR="${CMAKE_SOURCE_DIR}/game/data")
add_test(NAME game.unit COMMAND sol_game_tests)
