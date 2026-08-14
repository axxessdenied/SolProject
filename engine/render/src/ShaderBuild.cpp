#include "Sol/Render/ShaderBuild.h"

// Defined by sol_add_shaders in cmake/SolShaders.cmake, from the same variable that builds the
// glslc command line. One source, so the reported flags and the invoked flags cannot disagree.
#ifndef SOL_SHADER_BUILD_FLAGS
#error "SOL_SHADER_BUILD_FLAGS must be defined by sol_add_shaders; see cmake/SolShaders.cmake."
#endif

namespace sol::render {

std::string_view shaderBuildDescription()
{
    return SOL_SHADER_BUILD_FLAGS;
}

} // namespace sol::render
