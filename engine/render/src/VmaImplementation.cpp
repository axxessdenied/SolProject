/// @file
/// The single translation unit that instantiates the Vulkan Memory Allocator.
///
/// VMA is header-only plus one implementation body, which must be compiled exactly once.
///
/// The allocator is told to resolve Vulkan entry points dynamically rather than link them,
/// because nothing in this project links `vulkan-1.lib` — volk loads them. The function
/// pointers are handed over explicitly at allocator creation; see Renderer.cpp.
///
/// This file is deliberately empty of project code. It exists to own three macros.

// VMA_STATIC_VULKAN_FUNCTIONS and VMA_DYNAMIC_VULKAN_FUNCTIONS are set on the target rather
// than here, so every translation unit that includes the header agrees with this one.
#define VMA_IMPLEMENTATION

#include <vk_mem_alloc.h>
