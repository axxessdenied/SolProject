add_library(sol_platform STATIC
    src/file_io.cpp
    src/input_bindings.cpp
)
add_library(sol::platform ALIAS sol_platform)

target_include_directories(sol_platform PUBLIC include)
target_link_libraries(sol_platform PUBLIC sol::core)

if(WIN32)
    target_sources(sol_platform PRIVATE
        src/win32/audio_win32.cpp
        src/win32/system_win32.cpp
        src/win32/window_win32.cpp
    )
    # WASAPI reaches the endpoint through COM (decisions/009 / Phase 8t).
    target_link_libraries(sol_platform PRIVATE ole32)
else()
    message(FATAL_ERROR "Sol has no platform backend for this OS yet (Windows only for now).")
endif()
