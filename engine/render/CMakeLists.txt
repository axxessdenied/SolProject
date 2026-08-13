# sol::render — the only module permitted to see Vulkan types (ADR 0002).
#
# The Vulkan dependencies below are linked PRIVATE without exception. That is not a style
# preference: it is the mechanism that enforces ADR 0002's boundary requirement. A public link
# would propagate Vulkan's include directories to every consumer, and the rule would then hold
# only by everyone's continued good intentions. Linking privately makes a violation a compile
# error in the offending target instead.

add_library(SolRender STATIC
    include/Sol/Render/CapabilityRequirement.h
    include/Sol/Render/DeviceCapabilities.h
    include/Sol/Render/Renderer.h
    include/Sol/Render/VulkanInstance.h
    include/Sol/Render/WorldVec3.h
    src/CapabilityRequirement.cpp
    src/DeviceCapabilities.cpp
    src/RenderMath.h
    src/Renderer.cpp
    src/TerrainLod.cpp
    src/TerrainLod.h
    src/VmaImplementation.cpp
    src/VolkImplementation.cpp
    src/VulkanInstance.cpp
    src/VulkanInstanceImpl.h)

add_library(sol::render ALIAS SolRender)

sol_apply_project_options(SolRender)

target_include_directories(SolRender
    PUBLIC include
    PRIVATE src)

find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

target_link_libraries(SolRender
    PRIVATE
        volk::volk_headers
        GPUOpen::VulkanMemoryAllocator)

# The allocator's link edge is earned here rather than assumed: the renderer now creates a
# VkDevice, a depth image, and geometry buffers, which is the demonstration ADR 0002 asks for
# before an allocator may be selected. See docs/dependencies.md.
#
# VMA resolves Vulkan entry points through the function pointers it is handed, because nothing
# in this project links vulkan-1.lib — volk loads them. These are set on the target so every
# translation unit including the header agrees with the one that defines VMA_IMPLEMENTATION.
target_compile_definitions(SolRender
    PRIVATE
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

sol_add_shaders(SolRender
    SOURCES
        shaders/Reference.vert
        shaders/Reference.frag
        shaders/Terrain.vert
        shaders/Terrain.frag)

# volk dispatches through function pointers it loads itself, so nothing links vulkan-1.lib.
# An absent loader then becomes a value this module can inspect and report, rather than a
# process that fails to start with an OS-level "vulkan-1.dll not found". ADR 0002 requires the
# shipped game to "detect the installed Vulkan loader/driver and fail clearly"; this is where
# that becomes possible.
target_compile_definitions(SolRender PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)

# volk's implementation body is third-party code and does not compile clean under the /W4 /WX
# policy sol_apply_project_options applies to project-owned code. The relaxation is confined
# to that one file so the rest of the module keeps warnings-as-errors, and it is stated here
# rather than inside the source so the exception is visible in the build description.
set_source_files_properties(src/VolkImplementation.cpp src/VmaImplementation.cpp
    PROPERTIES COMPILE_OPTIONS "/W0")
