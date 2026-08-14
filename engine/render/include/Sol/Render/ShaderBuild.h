/// @file
/// How this build's SPIR-V was compiled, so a measurement can record it.
///
/// Debug and Release do not run the same shaders. Debug compiles `-g -O0` to keep a RenderDoc
/// capture readable; Release compiles `-O`, and `spirv-opt` is free to reassociate floating-point
/// arithmetic while doing so. ADR 0010 pins MSVC's floating-point behaviour for the CPU and says
/// nothing about the GPU, so nothing constrains that reassociation.
///
/// Both configurations run the full test suite, so every gate result exists in two variants
/// produced by two different shader binaries. That is acceptable — what is not acceptable is
/// publishing a number without saying which one produced it, which is what happened until this
/// existed. The gates print this alongside the device and driver.

#pragma once

#include <string_view>

namespace sol::render {

/// The `glslc` flags this build's embedded SPIR-V was compiled with.
///
/// Baked in from the build description rather than inferred from `NDEBUG`, so it cannot drift
/// from what the shader compiler was actually invoked with.
[[nodiscard]] std::string_view shaderBuildDescription();

} // namespace sol::render
