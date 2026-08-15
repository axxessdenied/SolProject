add_library(sol_assets STATIC
    src/asset_loader.cpp
    src/data_defs.cpp
)
add_library(sol::assets ALIAS sol_assets)

target_include_directories(sol_assets PUBLIC include)
target_link_libraries(sol_assets PUBLIC sol::platform)
