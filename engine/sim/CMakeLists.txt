add_library(sol_sim STATIC
    src/collision.cpp
    src/damage.cpp
    src/economy.cpp
    src/fixed_loop.cpp
    src/flight.cpp
    src/power.cpp
    src/steering.cpp
    src/universe.cpp
    src/weapons.cpp
)
add_library(sol::sim ALIAS sol_sim)

target_include_directories(sol_sim PUBLIC include)
target_link_libraries(sol_sim PUBLIC sol::core)
