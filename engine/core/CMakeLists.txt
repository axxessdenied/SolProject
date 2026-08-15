add_library(sol_core STATIC
    src/inflate.cpp
    src/json.cpp
    src/log.cpp
    src/version.cpp
)
add_library(sol::core ALIAS sol_core)

target_include_directories(sol_core PUBLIC include)
target_link_libraries(sol_core PUBLIC sol_options)

target_compile_definitions(sol_core PRIVATE
    SOL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    SOL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    SOL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
