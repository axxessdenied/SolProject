# Locates the Vulkan SDK and pins it as a toolchain input.
#
# ADR 0007 treats platform SDKs and development tools that are not vcpkg packages as
# "pinned toolchain inputs with documented installation and version checks". This module is
# that version check. See docs/dependencies.md for the review that selected it.
#
# Division of responsibility, deliberately drawn so that *building* SolProject never depends
# on a developer's SDK installation matching anyone else's:
#
#   vcpkg (pinned by builtin-baseline)  ->  headers, loader shim, allocator, window library.
#                                           Everything that is compiled or linked.
#   Vulkan SDK (this module)            ->  glslc, validation layer, profiles layer, vkconfig,
#                                           capture tooling. Everything used to *develop* and
#                                           to *produce evidence*.
#
# The SDK is therefore required to build shaders and to run the P1b evidence scenarios, but it
# contributes no headers and no link-time inputs. That keeps the compiled artifact a function
# of the checked-in manifest rather than of an installer.

include_guard(GLOBAL)

# The reviewed SDK version. Raising this is a dependency change under ADR 0007, not routine
# maintenance: the validation layer and the profiles layer are evidence-producing tools, and a
# silent upgrade changes what the gating thresholds were measured against.
set(SOL_VULKAN_SDK_REVIEWED_VERSION "1.4.357.0")

set(SOL_VULKAN_SDK_ROOT "" CACHE PATH "Vulkan SDK root used for shader compilation and layers")
set(SOL_VULKAN_SDK_VERSION "" CACHE INTERNAL "Detected Vulkan SDK version")
set(SOL_GLSLC_EXECUTABLE "" CACHE FILEPATH "glslc from the Vulkan SDK")
set(SOL_VULKAN_LAYER_PATH "" CACHE PATH "Directory holding the SDK's layer manifests")

#[[
Locate a usable Vulkan SDK and record its facts.

Searches VULKAN_SDK first, then the conventional Windows install root. Fails at configure time
with an actionable message rather than deferring the failure to a missing-shader link error.
]]
function(sol_find_vulkan_sdk)
    if(SOL_VULKAN_SDK_ROOT AND EXISTS "${SOL_VULKAN_SDK_ROOT}")
        return()
    endif()

    set(_sol_candidates "")

    if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _sol_env_sdk)
        list(APPEND _sol_candidates "${_sol_env_sdk}")
    endif()

    # The installer does not always export VULKAN_SDK into an already-running shell, so the
    # conventional root is searched too. Newest first, so a machine with several SDKs uses the
    # most recent one that still satisfies the reviewed minimum.
    file(GLOB _sol_installed "C:/VulkanSDK/*")
    list(SORT _sol_installed COMPARE NATURAL ORDER DESCENDING)
    list(APPEND _sol_candidates ${_sol_installed})

    foreach(_sol_candidate IN LISTS _sol_candidates)
        if(NOT IS_DIRECTORY "${_sol_candidate}")
            continue()
        endif()
        if(NOT EXISTS "${_sol_candidate}/Bin/glslc.exe")
            continue()
        endif()

        get_filename_component(_sol_version "${_sol_candidate}" NAME)
        if(NOT _sol_version MATCHES "^[0-9]+(\\.[0-9]+)+$")
            # VULKAN_SDK may point at a directory whose name is not the version. Ask the SDK.
            set(_sol_version "unknown")
        endif()

        if(NOT _sol_version STREQUAL "unknown"
           AND _sol_version VERSION_LESS SOL_VULKAN_SDK_REVIEWED_VERSION)
            continue()
        endif()

        set(SOL_VULKAN_SDK_ROOT "${_sol_candidate}" CACHE PATH "" FORCE)
        set(SOL_VULKAN_SDK_VERSION "${_sol_version}" CACHE INTERNAL "" FORCE)
        set(SOL_GLSLC_EXECUTABLE "${_sol_candidate}/Bin/glslc.exe" CACHE FILEPATH "" FORCE)
        set(SOL_VULKAN_LAYER_PATH "${_sol_candidate}/Bin" CACHE PATH "" FORCE)
        break()
    endforeach()

    if(NOT SOL_VULKAN_SDK_ROOT)
        message(FATAL_ERROR
            "No Vulkan SDK ${SOL_VULKAN_SDK_REVIEWED_VERSION} or newer was found.\n"
            "  Searched: VULKAN_SDK environment variable, then C:/VulkanSDK/*\n"
            "  Install:  winget install --id LunarG.VulkanSDK --exact\n"
            "            or download from https://vulkan.lunarg.com/sdk/home\n"
            "The SDK supplies glslc, the Khronos validation layer, and the Khronos profiles "
            "layer. P1b's validation-output and capability-reporting gates are measured with "
            "those tools, so the build refuses to proceed without them. It supplies no headers "
            "and no link inputs; those come from vcpkg. See docs/dependencies.md.")
    endif()

    # Required layers. Their absence is a broken or partial installation, which is worth
    # distinguishing from an absent SDK because the remedy differs.
    foreach(_sol_layer VkLayer_khronos_validation VkLayer_khronos_profiles)
        if(NOT EXISTS "${SOL_VULKAN_LAYER_PATH}/${_sol_layer}.json")
            message(FATAL_ERROR
                "Vulkan SDK at '${SOL_VULKAN_SDK_ROOT}' is missing ${_sol_layer}.json.\n"
                "P1b requires the validation layer for its validation-output gate and the "
                "profiles layer to constrain this machine's Vulkan 1.4-class devices down to "
                "the 1.2 candidate floor. Re-run the SDK installer and include the layers.")
        endif()
    endforeach()

    if(NOT SOL_VULKAN_SDK_VERSION STREQUAL SOL_VULKAN_SDK_REVIEWED_VERSION)
        message(WARNING
            "Vulkan SDK ${SOL_VULKAN_SDK_VERSION} is in use, but ${SOL_VULKAN_SDK_REVIEWED_VERSION} "
            "is the version reviewed in docs/dependencies.md. This is permitted, but any evidence "
            "produced from this tree must record the version actually used, and a threshold "
            "result measured against a different validation layer is not interchangeable with "
            "one measured against the reviewed layer.")
    endif()

    message(STATUS "Sol: Vulkan SDK ${SOL_VULKAN_SDK_VERSION} at ${SOL_VULKAN_SDK_ROOT}")
    message(STATUS "Sol: glslc ${SOL_GLSLC_EXECUTABLE}")
endfunction()
