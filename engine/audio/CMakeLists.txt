add_library(sol_audio STATIC
    src/mixer.cpp
)
add_library(sol::audio ALIAS sol_audio)

target_include_directories(sol_audio PUBLIC include)
# The renderer's acoustic sibling: cooked data in (sol::assets), a device out
# (sol::platform, which sol::assets already brings). No decoder here - ogg
# stops at the cooker, per docs/decisions/009-audio-decoder.md.
target_link_libraries(sol_audio PUBLIC sol::assets)
