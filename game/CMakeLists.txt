add_executable(sol_game
    src/main.cpp
)
set_target_properties(sol_game PROPERTIES OUTPUT_NAME sol)

target_link_libraries(sol_game PRIVATE
    sol::core
    sol::platform
)
