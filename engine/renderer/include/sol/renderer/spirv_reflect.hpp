#pragma once

// What a compiled shader ACTUALLY asks for, read out of its SPIR-V (engine
// plan Phase 25 stage C).
//
// ⚑⚑ WHY THIS EXISTS AT ALL, BECAUSE PHASE 25 DECISION 2 SAYS "DECLARED, NOT
// REFLECTED". It still does: the descriptor set layout is built from the
// material's DECLARATION, not from this. What this is for is the other half of
// the same sentence - "refuse a mismatch at load rather than discover it" -
// and you cannot refuse a mismatch without something to compare the
// declaration against. The alternative signal is the Vulkan validation layer,
// which is on in dev builds and OFF in shipping, so a declaration that lies
// currently reads as a wall of validation text on a developer's machine and as
// undefined behaviour on a player's. That is stage C's real product.
//
// ⚑⚑ IT IS FIRST-PARTY ON PURPOSE. AGENTS §5 scopes glslang/shaderc to
// build-time only and vendors no reflection library; SPIRV-Reflect or
// spirv-cross would each be a new dependency needing approval, and §5's own
// sentence - "everything else ... is written in this repo" - points at this.
// What is here is the smallest thing that answers the question: which
// descriptor bindings does this module declare, and what is inside its uniform
// block. It is not a general SPIR-V front end and should not grow into one.
//
// ⚑ NO VULKAN IN THIS HEADER, for the same reason `material_state.hpp` has
// none: the decision this makes is about data, so a suite must be able to
// assert it without a device.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sol::renderer {

enum class ShaderResourceKind
{
    SampledImage,  // combined image sampler - GLSL `sampler2D`
    UniformBuffer, // a `uniform Block { ... }` at a descriptor binding
    Other,         // something this engine has no material key for
};

// One descriptor a module declares. `name` is the GLSL variable name, kept
// because it is what an author sees in the file they have open - an error that
// says "binding 1" sends them counting, and one that says "uGlow" does not.
struct ShaderBinding
{
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    ShaderResourceKind kind = ShaderResourceKind::Other;
    std::string name;
};

// One member of a uniform block. `componentCount` is 1 for a float and 2/3/4
// for a vecN; 0 means a type this engine cannot fill from a `[material.params]`
// row, which is a refusal and not a silent skip.
struct ShaderBlockMember
{
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t componentCount = 0;
};

// A uniform block's contents at one binding.
struct ShaderBlock
{
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    std::uint32_t size = 0; // bytes, from the last member's offset plus its size
    std::vector<ShaderBlockMember> members;
    // ⚑ False when the compiler stripped debug names. Params are matched BY
    // NAME, so this is the difference between a mismatch we can name and one
    // we would have to guess at - the caller refuses rather than matching by
    // position, because matching by position is exactly the silent
    // misalignment this whole file exists to prevent.
    bool hasMemberNames = true;
};

struct ShaderInterface
{
    // Every descriptor binding the module declares, sorted by (set, binding).
    std::vector<ShaderBinding> bindings;
    // Uniform blocks at descriptor bindings, in the same order. Push constant
    // blocks are deliberately NOT here: the 128-byte push block is the
    // engine's, identical in every mesh shader, and not a material's business.
    std::vector<ShaderBlock> blocks;
};

// Reads `words` (a whole .spv file). False with `outError` set on anything
// that is not a SPIR-V module this function understands; the error is written
// for somebody holding the shader, not for somebody holding the spec.
[[nodiscard]] bool
reflectShader(std::span<const std::uint32_t> words, ShaderInterface& out, std::string* outError);

// The block at a set/binding, or nullptr.
[[nodiscard]] const ShaderBlock*
findBlock(const ShaderInterface& interface, std::uint32_t set, std::uint32_t binding);

} // namespace sol::renderer
