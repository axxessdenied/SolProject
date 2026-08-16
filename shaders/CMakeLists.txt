find_package(Vulkan REQUIRED)

# Prefer glslc (full Vulkan SDK); fall back to glslangValidator (CI builds from source).
find_program(SOL_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
find_program(SOL_GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT SOL_GLSLC AND NOT SOL_GLSLANG_VALIDATOR)
    message(FATAL_ERROR "No GLSL->SPIR-V compiler found (need glslc or glslangValidator on PATH or in \$VULKAN_SDK/Bin)")
endif()

set(SOL_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/shaders)

set(SOL_SHADER_SOURCES
    mesh.vert
    mesh.frag
    fullscreen.vert
    tonemap.frag
    sky.vert
    sky.frag
    impostor.vert
    impostor.frag
    debug_line.vert
    debug_line.frag
    particle.vert
    particle.frag
    ui.vert
    ui.frag
)

set(SOL_SHADER_OUTPUTS "")
foreach(shader IN LISTS SOL_SHADER_SOURCES)
    set(input ${CMAKE_CURRENT_SOURCE_DIR}/${shader})
    set(output ${SOL_SHADER_OUTPUT_DIR}/${shader}.spv)
    if(SOL_GLSLC)
        set(compile_command ${SOL_GLSLC} --target-env=vulkan1.3 ${input} -o ${output})
    else()
        set(compile_command ${SOL_GLSLANG_VALIDATOR} -V --target-env vulkan1.3 ${input} -o ${output})
    endif()
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SOL_SHADER_OUTPUT_DIR}
        COMMAND ${compile_command}
        DEPENDS ${input}
        COMMENT "Compiling shader ${shader}"
        VERBATIM
    )
    list(APPEND SOL_SHADER_OUTPUTS ${output})
endforeach()

add_custom_target(sol_shaders DEPENDS ${SOL_SHADER_OUTPUTS})
