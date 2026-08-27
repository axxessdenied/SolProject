find_package(Vulkan REQUIRED)

# Prefer glslc (full Vulkan SDK); fall back to glslangValidator (CI builds from source).
find_program(SOL_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
find_program(SOL_GLSLANG_VALIDATOR glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin")
if(NOT SOL_GLSLC AND NOT SOL_GLSLANG_VALIDATOR)
    message(FATAL_ERROR "No GLSL->SPIR-V compiler found (need glslc or glslangValidator on PATH or in \$VULKAN_SDK/Bin)")
endif()

set(SOL_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin/shaders)

# ⚑⚑ EVERY .vert/.frag IN THIS DIRECTORY, RATHER THAN A HANDWRITTEN LIST OF
# FOURTEEN (engine plan Phase 25 stage B). The list was one of the walls the
# phase's diagnosis counted: with a material naming its shader pair as data, a
# new pair is meant to be two files and a `[[material]]` row, and a build list
# that has to be edited as well is a C++-shaped step wearing CMake's clothes.
#
# ⚑ CONFIGURE_DEPENDS is not optional here. Without it CMake globs once, at
# configure time, and a shader added later is silently never compiled - which
# would show up as a material that "cannot find its SPIR-V" and send the reader
# looking at the search path rather than at this file.
file(GLOB SOL_SHADER_SOURCES CONFIGURE_DEPENDS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/*.frag
)
list(SORT SOL_SHADER_SOURCES)
if(NOT SOL_SHADER_SOURCES)
    message(FATAL_ERROR "No GLSL sources found in ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

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
