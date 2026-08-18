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
endif()
