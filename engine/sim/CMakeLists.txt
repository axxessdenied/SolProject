add_library(sol_sim STATIC
    src/fixed_loop.cpp
)
add_library(sol::sim ALIAS sol_sim)

target_include_directories(sol_sim PUBLIC include)
target_link_libraries(sol_sim PUBLIC sol::core)
