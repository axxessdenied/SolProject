# Compiles GLSL to SPIR-V at build time and embeds the result in the binary.
#
# Shaders are compiled offline rather than at runtime: the game ships SPIR-V, not a compiler.
# `glslc` comes from the pinned Vulkan SDK (see cmake/SolVulkanSdk.cmake and
# docs/dependencies.md), so the SPIR-V a build produces is a function of a recorded tool
# version rather than of whatever a driver happens to accept.
#
# Two deliberate choices:
#
#   --target-env=vulkan1.2  enforces ADR 0002's candidate floor at the shader toolchain. A
#                           shader that reaches for a 1.3-era feature fails the build here
#                           rather than at runtime on the one machine that lacks it. Both GPUs
#                           available to this project are well past 1.2, so without this the
#                           floor would be untested in the shader path exactly as it was in
#                           the capability path.
#
#   -mfmt=c                 emits a braced initialiser list that a header includes directly,
#                           so the SPIR-V lands in the executable. No runtime file I/O, no
#                           path resolution relative to the binary, and no shader-packaging
#                           decision — which belongs to a later milestone, not to B1.

include_guard(GLOBAL)

#[[
Compile GLSL sources to embeddable SPIR-V and attach them to a target.

Each source produces `${CMAKE_CURRENT_BINARY_DIR}/shaders/<name>.<stage>.inc`, included as:

    constexpr std::uint32_t kTriangleVert[] =
    #include "shaders/Triangle.vert.inc"
    ;

The generated directory is added to the target's private include path.
]]
function(sol_add_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 SOL_SHADER "" "" "SOURCES")

    if(NOT SOL_SHADER_SOURCES)
        message(FATAL_ERROR "sol_add_shaders(${target}): no SOURCES given")
    endif()
    if(NOT SOL_GLSLC_EXECUTABLE)
        message(FATAL_ERROR
            "sol_add_shaders(${target}): glslc was not located. sol_find_vulkan_sdk() must "
            "run before any target compiles shaders.")
    endif()

    set(_sol_outputs "")
    set(_sol_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${_sol_shader_dir}")

    # Debug keeps names and disables optimisation so a capture in RenderDoc is readable;
    # Release optimises. Both record which was used, because a shader compiled differently is
    # a different shader and P1b's precision measurements have to say which one they ran.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_sol_shader_flags -g -O0)
    else()
        set(_sol_shader_flags -O)
    endif()

    foreach(_sol_source IN LISTS SOL_SHADER_SOURCES)
        get_filename_component(_sol_name "${_sol_source}" NAME)
        set(_sol_output "${_sol_shader_dir}/${_sol_name}.inc")
        get_filename_component(_sol_absolute "${_sol_source}" ABSOLUTE)

        add_custom_command(
            OUTPUT "${_sol_output}"
            COMMAND "${SOL_GLSLC_EXECUTABLE}"
                    --target-env=vulkan1.2
                    -mfmt=c
                    ${_sol_shader_flags}
                    "${_sol_absolute}"
                    -o "${_sol_output}"
            DEPENDS "${_sol_absolute}"
            COMMENT "Compiling shader ${_sol_name} to SPIR-V (vulkan1.2)"
            VERBATIM)

        list(APPEND _sol_outputs "${_sol_output}")
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${_sol_outputs})
    add_dependencies(${target} ${target}_shaders)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_sources(${target} PRIVATE ${SOL_SHADER_SOURCES})

    # Shader sources are inputs to a custom command, not to the C++ compiler.
    set_source_files_properties(${SOL_SHADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
endfunction()
