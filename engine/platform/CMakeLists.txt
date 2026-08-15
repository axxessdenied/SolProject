add_library(sol_platform STATIC)
add_library(sol::platform ALIAS sol_platform)

target_include_directories(sol_platform PUBLIC include)
target_link_libraries(sol_platform PUBLIC sol::core)

if(WIN32)
    target_sources(sol_platform PRIVATE
        src/win32/platform_win32.cpp
    )
else()
    message(FATAL_ERROR "Sol has no platform backend for this OS yet (Windows only for now).")
endif()
