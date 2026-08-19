# The Forge (engine plan Phase 9 stage C): the asset viewer that grows into the
# authoring tool. A second executable beside the cooker rather than a mode
# inside the game - the shipping binary carries no editor, and ImGui stays dev
# tooling (AGENTS.md 5).
add_executable(sol_forge
    src/forge_view.cpp
    src/main.cpp
    src/mesh_library.cpp
    src/part_editor.cpp
)
set_target_properties(sol_forge PROPERTIES OUTPUT_NAME forge)

# `sol_cooker_lib` is what reads a `.gltf`; the Forge opens authored source
# files as well as cooked ones, and a second importer would be a second
# implementation of the one thing the cooker already does.
target_link_libraries(sol_forge PRIVATE
    sol::core
    sol::platform
    sol::rhi
    sol::renderer
    sol::assets
    sol::ui
    sol_cooker_lib
    imgui
)

# Authored meshes come from the source tree in a dev build, the same way the
# game reads its defs; cooked assets and shaders are found beside the binary.
target_compile_definitions(sol_forge PRIVATE
    SOL_ASSETS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets"
)

add_dependencies(sol_forge sol_shaders sol_cook_assets)

# Needs a GPU + Vulkan driver, so it rides the same "gpu" label the game's
# smoke test does: ctest --preset dev-gpu
add_test(NAME forge.smoke COMMAND sol_forge --frames 120)
set_tests_properties(forge.smoke PROPERTIES LABELS gpu)
