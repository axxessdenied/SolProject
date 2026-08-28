# The Forge (engine plan Phase 9 stage C): the asset viewer that grows into the
# authoring tool. A second executable beside the cooker rather than a mode
# inside the game - the shipping binary carries no editor, and ImGui stays dev
# tooling (AGENTS.md 5).
add_executable(sol_forge
    src/def_editor.cpp
    src/forge_view.cpp
    src/main.cpp
    src/mesh_library.cpp
    src/part_editor.cpp
    src/point_tool.cpp
    src/project_paths.cpp
    src/sound_preview.cpp
    src/texture_editor.cpp
    src/waveform.cpp
)
set_target_properties(sol_forge PROPERTIES OUTPUT_NAME forge)

# `sol_cooker_lib` is what reads a `.gltf`; the Forge opens authored source
# files as well as cooked ones, and a second importer would be a second
# implementation of the one thing the cooker already does.
#
# ⚑⚑ `sol::audio` IS PHASE 24 STAGE U1's ONE BUILD CHANGE, AND THE SPEC SAID
# THIS STAGE WOULD NEED "ZERO C++" - IT WAS WRONG BY THIS LINE PLUS A FILE.
# `platform::AudioDevice` was already here (`sol::platform`), which is what made
# the estimate look free, but a device without a mixer is a callback with
# nothing to put in the buffer. What the tool still does NOT link is `GameAudio`
# - that is game policy, and AGENTS.md 4's line holds: the tool reads the
# game's DATA and never its CODE.
target_link_libraries(sol_forge PRIVATE
    sol::core
    sol::platform
    sol::rhi
    sol::renderer
    sol::assets
    sol::audio
    sol::ui
    sol_cooker_lib
    imgui
)

# Authored meshes come from the source tree in a dev build, the same way the
# game reads its defs; cooked assets and shaders are found beside the binary.
#
# ⚑ SOL_MODEL_DATA_DIR is the game's DATA directory, and pointing a tool at it
# is not the same as linking game code (AGENTS.md 4): the [[model]] rows are
# parsed by sol::assets::DefDatabase, which is engine. It is what lets the
# viewer compare a measured radius against the authored one instead of leaving
# that to a person reading a panel and a text file side by side.
# ⚑⚑ THE BLENDER DROP DIRECTORY, AND IT IS OUTSIDE `assets/` FOR A HARD REASON
# RATHER THAN A TIDY ONE (stage L). The cooker walks the source tree with a
# RECURSIVE listFiles into ONE FLAT output directory and keys every output on
# the file STEM, so `ship.gltf` anywhere under `assets/` and `ship.forge` in
# `assets/meshes/` both cook to `ship.smesh` - and the collision guard does not
# skip that pair, it aborts the WHOLE cook ("nothing cooked", exit 1). An inbox
# under `assets/` would break the entire build the first time an import worked.
#
# ⚑⚑⚑ PHASE 24 STAGE V MADE THESE CONDITIONAL, AND IT WAS A SHIPPING DEFECT
# RATHER THAN A TIDY-UP. They were unconditional for fifteen phases, so a
# RELEASE build of this tool baked three of the build machine's absolute paths
# into the binary - measured in build/release/bin/forge.exe before this change:
#
#   C:/Users/<name>/dev/cpp/SolProject/assets
#   C:/Users/<name>/dev/cpp/SolProject/blender-inbox
#   C:/Users/<name>/dev/cpp/SolProject/game/data
#
# sol.exe and cooker.exe beside it carried zero, because Phase 22 found and
# fixed exactly this for the game and nobody owned the tool. A shipped Forge
# would have read directories that exist on one machine in the world - and
# worked perfectly on that machine, which is packaging's characteristic
# failure.
#
# What replaces them is a PROJECT (src/project_paths.hpp): a mod directory the
# tool is pointed at, which is the shape game/mods/README.md already defines
# and the shape the game already loads.
if(SOL_DEV_DATA_PATHS)
    target_compile_definitions(sol_forge PRIVATE
        SOL_ASSETS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets"
        SOL_MODEL_DATA_DIR="${CMAKE_SOURCE_DIR}/game/data"
        SOL_FORGE_INBOX_DIR="${CMAKE_SOURCE_DIR}/blender-inbox"
    )
endif()

add_dependencies(sol_forge sol_shaders sol_cook_assets)

# Needs a GPU + Vulkan driver, so it rides the same "gpu" label the game's
# smoke test does: ctest --preset dev-gpu
add_test(NAME forge.smoke COMMAND sol_forge --frames 120)
set_tests_properties(forge.smoke PROPERTIES LABELS gpu)

# ⚑ The Forge's first HEADLESS suite, and it deliberately covers only what is
# not a viewport. AGENTS.md 7 says the tool's UI is verified by running it -
# there is no suite for a window and pretending otherwise is how game/src ended
# up with 21,000 untested lines - so this compiles `mesh_library.cpp` alone,
# which pulls in no ImGui, no device and no window. What it buys is the radius
# tolerance, which the D checkpoint flagged as a real decision in untested code.
add_executable(sol_forge_tests
    src/mesh_library.cpp
    src/project_paths.cpp
    src/waveform.cpp
    test/mesh_library_tests.cpp
)
target_include_directories(sol_forge_tests PRIVATE src)
target_link_libraries(sol_forge_tests PRIVATE
    sol::test
    sol::core
    sol::platform
    sol::assets
    sol_cooker_lib
)
# The committed sources and the game's own `[[model]]` rows, so the thresholds
# are pinned against the real assets rather than against fixtures that agree
# with the code by construction.
#
# ⚑ UNCONDITIONAL, UNLIKE THE TOOL'S OWN ABOVE, AND THAT IS NOT AN OVERSIGHT. A
# test binary is never installed - packaging/check_layout.cmake forbids
# `*_tests*` in both packages - so a baked source path here cannot reach
# anybody's machine, and a suite pinned against the REAL committed assets is
# the whole point of it. The tool's paths were gated because the tool ships.
target_compile_definitions(sol_forge_tests PRIVATE
    SOL_MESH_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/meshes"
    SOL_TEXTURE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/textures"
    SOL_SOUND_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/sounds"
    SOL_MODEL_DATA_DIR="${CMAKE_SOURCE_DIR}/game/data"
)
add_test(NAME forge.unit COMMAND sol_forge_tests)

# ---------------------------------------------------------------------------
# The tool package (engine plan Phase 24 stage V, decision 010).
#
# ⚑⚑ ITS OWN CPACK COMPONENT RATHER THAN EXTRA FILES IN THE GAME'S ARCHIVE, and
# the deciding reason is a test. Phase 9 promised "the shipping binary carries
# no editor" and Phase 22 turned that into `packaging/check_layout.cmake`, which
# forbids `forge*` in an installed game tree. Decision 010 NARROWED the promise
# to "the GAME binary carries no editor" instead of deleting it - so the game
# archive is unchanged, that assertion still holds, and this is a second
# archive with a second assertion of its own
# (`packaging/check_tool_layout.cmake`). A player who will never mod does not
# download an editor.
#
# ⚑ What is absent is absent by omission, which is why the layout test exists:
# no `sol`, no `cooker`, no `data/`, no `mods/`, no `cooked/`, and no GLSL
# compiler (decision 011 - a mod ships SPIR-V and the author brings their own
# `glslc`).
install(TARGETS sol_forge RUNTIME DESTINATION . COMPONENT forge)

# ⚑⚑ THE ENGINE'S OWN SPIR-V, AND THE TOOL CANNOT OPEN A WINDOW WITHOUT IT. It
# is both halves of `project_paths.hpp`'s search path at once: the tonemap and
# the debug lines resolve HERE and nowhere else, and it is also the fallback
# that lets a mod's `[[material]]` row name `mesh` or `membrane` and ship only
# the stage it replaces.
#
# ⚑ The same refusal the game's payload gets, for the same reason: install over
# a missing DIRECTORY is not an error - it creates an empty destination and says
# nothing - so the failure would be an archive that unpacks cleanly and then
# opens a window with no pipelines on somebody else's machine.
set(solForgeShaderDir ${CMAKE_BINARY_DIR}/bin/shaders)
string(CONFIGURE [[
if(NOT EXISTS "@solForgeShaderDir@")
    message(FATAL_ERROR "Nothing to install from @solForgeShaderDir@ - build the sol_shaders target first.")
endif()
]] solForgeShaderCheck @ONLY)
install(CODE "${solForgeShaderCheck}" COMPONENT forge)
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin/shaders/ DESTINATION shaders COMPONENT forge)

# ⚑ The requirements differ from the game's by exactly one line and it is the
# one that matters: this tool needs a GPU too, because the viewport is the real
# renderer (Phase 9 decision 1) rather than a preview of it.
if(WIN32)
    set(SOL_FORGE_REQUIREMENTS
        "Windows 10 or newer, 64-bit, and a GPU and driver supporting Vulkan 1.3.\nThe viewport is the game's own renderer, so the same card runs both.\nThe C runtime is linked statically - no Visual C++ redistributable needed.")
    set(SOL_FORGE_OPEN
        "Drag that folder onto forge.exe. (Or make a shortcut whose target ends\nwith the folder, in quotes if the path has spaces in it.)\n\nStarted with no folder, the Forge opens its own directory as the project,\nwhich is empty - so it will look bare until you point it at a mod.")
else()
    set(SOL_FORGE_REQUIREMENTS
        "Ubuntu 24.04 or newer (glibc 2.38+), 64-bit, a GPU and driver supporting\nVulkan 1.3, and a Wayland session. There is no X11 backend. The runtime\nlibraries are the same ones the game needs:\n  libvulkan1  libwayland-client0  libwayland-cursor0  libxkbcommon0")
    set(SOL_FORGE_OPEN
        "Pass it on the command line:\n\n    ./forge ~/games/TheStarsDontWait/mods/my-mod\n\nStarted with no folder, the Forge opens its own directory as the project,\nwhich is empty - so it will look bare until you point it at a mod.")
endif()
configure_file(${CMAKE_SOURCE_DIR}/packaging/FORGE-README.txt.in
               ${CMAKE_CURRENT_BINARY_DIR}/README.txt @ONLY)

# ⚑ The notices MIT requires to ACCOMPANY A BINARY. This package carries Dear
# ImGui where the game's does not, so the file is not merely copied along for
# symmetry - it is the only place ImGui's notice reaches a Forge user.
install(FILES
    ${CMAKE_SOURCE_DIR}/LICENSE
    ${CMAKE_SOURCE_DIR}/packaging/THIRD-PARTY.txt
    ${CMAKE_CURRENT_BINARY_DIR}/README.txt
    DESTINATION . COMPONENT forge)

# ---------------------------------------------------------------------------
# Tests over the SHIPPING tool layout, in a shipping configuration only - the
# same reasoning as game/CMakeLists.txt's: in a dev tree the installed binary
# would still read the source tree, so these would pass while proving nothing.
#
# ⚑⚑ `--component forge` IS LOAD-BEARING AND NOT A TIDY-UP. Without it the
# install writes BOTH packages into one prefix, and `check_layout.cmake` would
# then find `forge.exe` in the game tree and fail - which is that test doing
# its job. Two components, two prefixes, two assertions.
# ---------------------------------------------------------------------------
if(NOT SOL_DEV_DATA_PATHS)
    set(solForgePackageTestPrefix ${CMAKE_BINARY_DIR}/package-test-forge)

    add_test(NAME package.forge.install COMMAND ${CMAKE_COMMAND}
        -DSOL_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DSOL_PREFIX=${solForgePackageTestPrefix}
        -DSOL_COMPONENT=forge
        -P ${CMAKE_SOURCE_DIR}/packaging/install_fresh.cmake)
    set_tests_properties(package.forge.install PROPERTIES FIXTURES_SETUP sol_forge_package_tree)

    add_test(NAME package.forge.layout COMMAND ${CMAKE_COMMAND}
        -DSOL_PREFIX=${solForgePackageTestPrefix}
        -DSOL_EXE_NAME=$<TARGET_FILE_NAME:sol_forge>
        -P ${CMAKE_SOURCE_DIR}/packaging/check_tool_layout.cmake)
    set_tests_properties(package.forge.layout PROPERTIES FIXTURES_REQUIRED sol_forge_package_tree)

    # ⚑⚑ THE ONLY THING THAT RUNS THE SHIPPED TOOL AGAINST A SHIPPED PROJECT,
    # and it is what a baked source path would fail. `--project` points it at a
    # directory that does not exist yet, inside the install prefix: the tool has
    # to create the layout, find the engine's shaders beside itself, build the
    # placeholder texture because the project has none, and come up with no def
    # file at all. Every one of those is a stage V behaviour and none of them is
    # reachable from the dev tree, where all three directories are full.
    add_test(NAME package.forge.smoke
        COMMAND $<TARGET_FILE_NAME:sol_forge> --project ${solForgePackageTestPrefix}/new-mod --frames 120)
    set_tests_properties(package.forge.smoke PROPERTIES
        FIXTURES_REQUIRED sol_forge_package_tree
        LABELS gpu
        WORKING_DIRECTORY ${solForgePackageTestPrefix})
endif()
