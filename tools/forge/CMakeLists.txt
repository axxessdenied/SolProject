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
    src/sound_preview.cpp
    src/texture_editor.cpp
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
target_compile_definitions(sol_forge PRIVATE
    SOL_ASSETS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets"
    SOL_MODEL_DATA_DIR="${CMAKE_SOURCE_DIR}/game/data"
    SOL_FORGE_INBOX_DIR="${CMAKE_SOURCE_DIR}/blender-inbox"
)

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
target_compile_definitions(sol_forge_tests PRIVATE
    SOL_MESH_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/meshes"
    SOL_TEXTURE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/textures"
    SOL_SOUND_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/sounds"
    SOL_MODEL_DATA_DIR="${CMAKE_SOURCE_DIR}/game/data"
)
add_test(NAME forge.unit COMMAND sol_forge_tests)
