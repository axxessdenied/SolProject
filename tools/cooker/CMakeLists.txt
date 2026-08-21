add_library(sol_cooker_lib STATIC
    src/bc1.cpp
    src/font.cpp
    src/gltf.cpp
    src/mesh.cpp
    src/png.cpp
    src/sound.cpp
)
target_include_directories(sol_cooker_lib PUBLIC src)
target_link_libraries(sol_cooker_lib PUBLIC sol::assets)
# The ogg decoder stops here (decisions/009): cooking is the only place it is
# linked, so the shipping binary has no third-party code in its audio path.
target_link_libraries(sol_cooker_lib PRIVATE stb_vorbis)

add_executable(sol_cooker src/main.cpp)
set_target_properties(sol_cooker PROPERTIES OUTPUT_NAME cooker)
target_link_libraries(sol_cooker PRIVATE sol_cooker_lib)

# Unit tests for the import/encode paths.
add_executable(sol_cooker_tests
    test/cooker_tests.cpp
    test/font_tests.cpp
    test/texture_tests.cpp
)
target_link_libraries(sol_cooker_tests PRIVATE sol_cooker_lib sol::test)
# Stage G's acceptance gate reads both the `.tex` source and the `.png` it
# replaces, and this is the only suite that links the decoder and sol::assets at
# once. The define goes with the test that needs it.
target_compile_definitions(sol_cooker_tests
    PRIVATE SOL_TEXTURE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/textures")
# The committed `.forge` sources are what the mesh round trip cooks, so the
# suite reads the real assets rather than a fixture that agrees with the code by
# construction - the same reason geometry.unit is handed this directory.
target_compile_definitions(sol_cooker_tests
    PRIVATE SOL_MESH_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/meshes")
add_test(NAME cooker.unit COMMAND sol_cooker_tests)
