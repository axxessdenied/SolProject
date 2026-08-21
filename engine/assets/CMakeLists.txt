add_library(sol_assets STATIC
    src/asset_loader.cpp
    src/data_defs.cpp
    src/def_doc.cpp
    src/font.cpp
    src/forge_doc.cpp
    src/loadout.cpp
    src/mesh_build.cpp
    src/mesh_edit.cpp
    src/mesh_lod.cpp
    src/texture_doc.cpp
    src/truetype.cpp
)
add_library(sol::assets ALIAS sol_assets)

target_include_directories(sol_assets PUBLIC include)
target_link_libraries(sol_assets PUBLIC sol::platform)
