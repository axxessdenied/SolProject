find_package(Vulkan REQUIRED)

add_library(sol_rhi STATIC
    src/context.cpp
    src/descriptors.cpp
    src/gpu_profiler.cpp
    src/pipeline.cpp
    src/resources.cpp
    src/swapchain.cpp
)
add_library(sol::rhi ALIAS sol_rhi)

target_include_directories(sol_rhi PUBLIC include)
target_link_libraries(sol_rhi PUBLIC sol::platform Vulkan::Vulkan)

if(WIN32)
    # Native surface creation: the sanctioned platform touch point (engine plan 2.1).
    target_sources(sol_rhi PRIVATE src/win32/surface_win32.cpp)
elseif(LINUX)
    target_sources(sol_rhi PRIVATE src/linux/surface_wayland.cpp)
    # wayland-client is a system library here, the Linux counterpart of ole32 -
    # not a third-party dependency under AGENTS.md section 5.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    target_link_libraries(sol_rhi PRIVATE PkgConfig::WAYLAND_CLIENT)
else()
    # ⚑ Phase 21: this branch used to be absent, so a build on an unsupported
    # OS simply omitted the file and failed at LINK time naming a mangled
    # createNativeSurface. Refusing at configure time and naming the gap is the
    # Phase 19 rule - a refusal that names the file beats a silent fallback.
    message(FATAL_ERROR "sol_rhi has no native Vulkan surface backend for this OS (Windows and Linux only).")
endif()
