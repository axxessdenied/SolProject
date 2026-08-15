add_library(sol_scripting STATIC
    src/handle.cpp
    src/vm.cpp
)
add_library(sol::scripting ALIAS sol_scripting)

target_include_directories(sol_scripting PUBLIC include)
target_link_libraries(sol_scripting PUBLIC sol::core sol::ecs lua)
