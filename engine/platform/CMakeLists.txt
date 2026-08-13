# sol::platform — window and OS integration.
#
# GLFW is linked PRIVATE for the same reason the Vulkan packages are: the public header
# exposes only void* native handles, and a private link makes that a compile-time guarantee
# rather than a convention. sol::render never sees GLFW, and this module never sees Vulkan.

add_library(SolPlatform STATIC
    include/Sol/Platform/Window.h
    src/Window.cpp)

add_library(sol::platform ALIAS SolPlatform)

sol_apply_project_options(SolPlatform)

target_include_directories(SolPlatform
    PUBLIC include
    PRIVATE src)

find_package(glfw3 CONFIG REQUIRED)

target_link_libraries(SolPlatform PRIVATE glfw)
