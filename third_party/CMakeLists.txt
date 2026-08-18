# Vendored third-party code (AGENTS.md section 5). Never links sol_options;
# compiled with default warnings, never modified in place.

find_package(Vulkan REQUIRED)

# Lua 5.4 (www.lua.org, MIT). Compiled as C so errors unwind via longjmp;
# the scripting layer keeps every lua_State interaction behind pcall.
enable_language(C)
add_library(lua STATIC
    lua/lapi.c
    lua/lauxlib.c
    lua/lbaselib.c
    lua/lcode.c
    lua/lcorolib.c
    lua/lctype.c
    lua/ldblib.c
    lua/ldebug.c
    lua/ldo.c
    lua/ldump.c
    lua/lfunc.c
    lua/lgc.c
    lua/linit.c
    lua/liolib.c
    lua/llex.c
    lua/lmathlib.c
    lua/lmem.c
    lua/loadlib.c
    lua/lobject.c
    lua/lopcodes.c
    lua/loslib.c
    lua/lparser.c
    lua/lstate.c
    lua/lstring.c
    lua/lstrlib.c
    lua/ltable.c
    lua/ltablib.c
    lua/ltm.c
    lua/lundump.c
    lua/lutf8lib.c
    lua/lvm.c
    lua/lzio.c
)
target_include_directories(lua PUBLIC lua)

add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_vulkan.cpp
    imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC imgui imgui/backends)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan)

# stb_vorbis v1.22 (nothings.org/stb, public domain). Ogg Vorbis decode for
# the COOKER ONLY - see docs/decisions/009-audio-decoder.md and the AGENTS.md
# section 5 table. The shipping binary carries no decoder: .ogg and .wav both
# cook to PCM in .saud. Nothing above sol::assets may link this.
add_library(stb_vorbis STATIC
    stb_vorbis/stb_vorbis.c
)
target_include_directories(stb_vorbis PUBLIC stb_vorbis)
