/// @file
/// The single translation unit that instantiates volk's loader.
///
/// volk is header-only plus one implementation body, which must be compiled exactly once.
///
/// Why not vcpkg's prebuilt `volk::volk` static library: that target is built without
/// platform-specific defines, so it omits the Win32 entry points. The renderer needs
/// vkGetPhysicalDeviceWin32PresentationSupportKHR to answer whether a queue family can
/// present *without creating a window*, which is what lets the capability report run headless
/// as a first-line diagnostic. Compiling the implementation here with
/// VK_USE_PLATFORM_WIN32_KHR set on the target is the port's own documented alternative.
///
/// This file is deliberately empty of project code. It exists to own one macro.

// volk's implementation body triggers warnings that the project's /W4 /WX policy would turn
// into errors. Suppressing them for this file is scoped in CMake rather than here, so the
// suppression is visible in the build description instead of buried in a source file.
#define VOLK_IMPLEMENTATION
#include <volk.h>
