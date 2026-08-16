add_library(sol_sim STATIC
    src/collision.cpp
    src/damage.cpp
    src/economy.cpp
    src/faction_sim.cpp
    src/fixed_loop.cpp
    src/flight.cpp
    src/mining.cpp
    src/missions.cpp
    src/power.cpp
    src/steering.cpp
    src/survey.cpp
    src/universe.cpp
    src/weapons.cpp
)
add_library(sol::sim ALIAS sol_sim)

target_include_directories(sol_sim PUBLIC include)
target_link_libraries(sol_sim PUBLIC sol::core)
