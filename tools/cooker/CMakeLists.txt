add_library(sol_cooker_lib STATIC
    src/bc1.cpp
    src/font.cpp
    src/gltf.cpp
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
)
target_link_libraries(sol_cooker_tests PRIVATE sol_cooker_lib sol::test)
add_test(NAME cooker.unit COMMAND sol_cooker_tests)
