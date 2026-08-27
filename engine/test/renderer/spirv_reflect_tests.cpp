#include <cstdint>
#include <string>
#include <vector>

#include <sol/platform/file_io.hpp>
#include <sol/renderer/spirv_reflect.hpp>
#include <sol/test/test.hpp>

using sol::renderer::findBlock;
using sol::renderer::reflectShader;
using sol::renderer::ShaderBinding;
using sol::renderer::ShaderInterface;
using sol::renderer::ShaderResourceKind;

namespace {

// ⚑⚑ THE FIXTURES ARE THE SHADERS THIS GAME ACTUALLY SHIPS, COMPILED BY THE
// BUILD, and that is deliberate rather than lazy. A hand-written SPIR-V blob
// would test this parser against my understanding of the format; the real
// output tests it against the compiler the game is built with, which is the
// only thing it has to agree with. `assets.unit` makes the same argument about
// the committed `.tex` sources and `geometry.unit` about the `.forge` files.
//
// ⚑ The counterpart - and the reason this file also builds words by hand
// below - is that a real module cannot be made malformed on purpose. Truncation
// and a bad magic number have to be constructed.
[[nodiscard]] bool loadShader(const char* name, std::vector<std::uint32_t>& out)
{
    const std::string path = std::string(SOL_SHADER_BINARY_DIR) + "/" + name;
    std::vector<std::uint8_t> bytes;
    if (!sol::platform::readFileBytes(path.c_str(), bytes) || bytes.empty() || bytes.size() % 4 != 0) {
        return false;
    }
    out.resize(bytes.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint32_t>(bytes[i * 4]) |
                 (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8) |
                 (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16) |
                 (static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24);
    }
    return true;
}

[[nodiscard]] const ShaderBinding*
findBinding(const ShaderInterface& interface, std::uint32_t set, std::uint32_t binding)
{
    for (const ShaderBinding& candidate : interface.bindings) {
        if (candidate.set == set && candidate.binding == binding) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

// The stock mesh pair, which is what every material in this game drew through
// before stage C and what most of them still draw through.
SOL_TEST(spirvReflectReadsTheStockMeshPair)
{
    std::vector<std::uint32_t> words;
    SOL_REQUIRE(loadShader("mesh.frag.spv", words));

    ShaderInterface interface;
    std::string error;
    SOL_REQUIRE(reflectShader(words, interface, &error));

    // One combined image sampler at set 0 binding 0, and nothing else. This is
    // Phase 25's finding 2 - "exactly one descriptor set layout shape in the
    // entire engine" - asserted rather than remembered.
    SOL_REQUIRE(interface.bindings.size() == 1);
    SOL_CHECK(interface.bindings[0].set == 0);
    SOL_CHECK(interface.bindings[0].binding == 0);
    SOL_CHECK(interface.bindings[0].kind == ShaderResourceKind::SampledImage);
    SOL_CHECK(interface.bindings[0].name == "uAlbedo");
    SOL_CHECK(interface.blocks.empty());

    // ⚑ THE PUSH BLOCK IS NOT A BINDING AND MUST NOT BE ONE. `mesh.frag` has a
    // 128-byte push constant block; it has no descriptor set or binding
    // decoration, so it drops out with no special case. If it ever appeared
    // here, every material in the game would fail its declaration check.
    std::vector<std::uint32_t> vertexWords;
    SOL_REQUIRE(loadShader("mesh.vert.spv", vertexWords));
    ShaderInterface vertex;
    SOL_REQUIRE(reflectShader(vertexWords, vertex, &error));
    SOL_CHECK(vertex.bindings.empty());
    SOL_CHECK(vertex.blocks.empty());
}

// ⚑⚑ THE ONE THIS PARSER EXISTS FOR. `cockpit.frag` is the first shader in the
// game with a set 1, and every field checked here is one the declaration check
// compares a `[[material]]` row against.
SOL_TEST(spirvReflectReadsTheCockpitSlotsAndParams)
{
    std::vector<std::uint32_t> words;
    SOL_REQUIRE(loadShader("cockpit.frag.spv", words));

    ShaderInterface interface;
    std::string error;
    SOL_REQUIRE(reflectShader(words, interface, &error));

    // Set 0 unchanged: the albedo is the engine's, not the material's.
    const ShaderBinding* albedo = findBinding(interface, 0, 0);
    SOL_REQUIRE(albedo != nullptr);
    SOL_CHECK(albedo->kind == ShaderResourceKind::SampledImage);
    SOL_CHECK(albedo->name == "uAlbedo");

    // Set 1 is the material's: its declared slot, then its params.
    const ShaderBinding* glow = findBinding(interface, 1, 0);
    SOL_REQUIRE(glow != nullptr);
    SOL_CHECK(glow->kind == ShaderResourceKind::SampledImage);
    SOL_CHECK(glow->name == "uGlow");

    const ShaderBinding* params = findBinding(interface, 1, 1);
    SOL_REQUIRE(params != nullptr);
    SOL_CHECK(params->kind == ShaderResourceKind::UniformBuffer);

    // ⚑ SORTED BY (SET, BINDING), which the registry relies on to read a
    // declaration's slots off in order without sorting them again.
    SOL_REQUIRE(interface.bindings.size() == 3);
    SOL_CHECK(interface.bindings[0].set == 0);
    SOL_CHECK(interface.bindings[1].set == 1 && interface.bindings[1].binding == 0);
    SOL_CHECK(interface.bindings[2].set == 1 && interface.bindings[2].binding == 1);

    // ⚑⚑ THE MEMBER, ITS NAME AND ITS OFFSET - the three facts a param is
    // written with. The name is the contract (params are matched by name), the
    // offset is where the float goes, and `componentCount == 1` is what says a
    // single float can fill it.
    const sol::renderer::ShaderBlock* block = findBlock(interface, 1, 1);
    SOL_REQUIRE(block != nullptr);
    SOL_CHECK(block->hasMemberNames);
    SOL_REQUIRE(block->members.size() == 1);
    SOL_CHECK(block->members[0].name == "glow_strength");
    SOL_CHECK(block->members[0].offset == 0);
    SOL_CHECK(block->members[0].componentCount == 1);
    SOL_CHECK(block->size == 4);
}

// The membrane brings its own fragment stage and no interface at all, which is
// the case that has to keep costing nothing.
SOL_TEST(spirvReflectSeesNoMaterialSetInTheMembrane)
{
    std::vector<std::uint32_t> words;
    SOL_REQUIRE(loadShader("membrane.frag.spv", words));

    ShaderInterface interface;
    std::string error;
    SOL_REQUIRE(reflectShader(words, interface, &error));
    for (const ShaderBinding& binding : interface.bindings) {
        SOL_CHECK(binding.set == 0);
    }
    SOL_CHECK(interface.blocks.empty());
}

// ⚑ THE ENGINE'S OTHER SHADERS ARE A FREE CORPUS AND ARE USED AS ONE. None of
// them is a material shader, so nothing here asserts what they contain - only
// that the parser walks a real module of every shape the build produces without
// failing or reading past the end. Vertex, fragment, fullscreen, cubemap and
// point-sprite stages are all in this list.
SOL_TEST(spirvReflectWalksEveryShippedShader)
{
    const char* names[] = {"debug_line.frag.spv",
                           "debug_line.vert.spv",
                           "fullscreen.vert.spv",
                           "impostor.frag.spv",
                           "impostor.vert.spv",
                           "particle.frag.spv",
                           "particle.vert.spv",
                           "sky.frag.spv",
                           "sky.vert.spv",
                           "tonemap.frag.spv",
                           "ui.frag.spv",
                           "ui.vert.spv"};
    for (const char* name : names) {
        std::vector<std::uint32_t> words;
        SOL_REQUIRE(loadShader(name, words));
        ShaderInterface interface;
        std::string error;
        SOL_CHECK(reflectShader(words, interface, &error));
        // Every descriptor this engine has is a fragment-stage sampler at set
        // 0, so anything at set 1 in a shader nobody declared one for would be
        // a surprise worth knowing about.
        for (const ShaderBinding& binding : interface.bindings) {
            SOL_CHECK(binding.set == 0);
        }
    }
}

// ⚑⚑ MALFORMED INPUT IS A REFUSAL WITH A SENTENCE, NOT A CRASH. A .spv reaches
// this parser from a mod directory since stage E, so "the bytes are wrong" is a
// case a player can cause and not only a case a developer can.
SOL_TEST(spirvReflectRefusesWhatIsNotAModule)
{
    ShaderInterface interface;
    std::string error;

    // Too short to hold a header.
    const std::uint32_t stub[] = {0x07230203u, 0x00010600u};
    SOL_CHECK(!reflectShader(stub, interface, &error));
    SOL_CHECK(!error.empty());

    // Not SPIR-V at all.
    const std::uint32_t notSpirv[] = {0x12345678u, 0, 0, 16, 0};
    error.clear();
    SOL_CHECK(!reflectShader(notSpirv, interface, &error));
    SOL_CHECK(error.find("magic") != std::string::npos);

    // ⚑ Byte-swapped gets its OWN message, because it is a real thing that
    // happens to a file moved by something that thinks it is text - and "wrong
    // magic number" would send the reader to check their compiler instead of
    // their copy.
    const std::uint32_t swapped[] = {0x03022307u, 0, 0, 16, 0};
    error.clear();
    SOL_CHECK(!reflectShader(swapped, interface, &error));
    SOL_CHECK(error.find("byte-swapped") != std::string::npos);

    // An implausible id bound, which is the field a corrupt file most easily
    // turns into an allocation nobody wants.
    const std::uint32_t hugeBound[] = {0x07230203u, 0x00010600u, 0, 0xFFFFFFFFu, 0};
    error.clear();
    SOL_CHECK(!reflectShader(hugeBound, interface, &error));
    SOL_CHECK(error.find("bound") != std::string::npos);

    // An instruction claiming more words than the module holds. Built by taking
    // a real module and lying about its first instruction's length, so
    // everything up to that point is genuine.
    std::vector<std::uint32_t> truncated;
    SOL_REQUIRE(loadShader("mesh.frag.spv", truncated));
    truncated[5] = (0xFFFFu << 16) | (truncated[5] & 0xFFFFu);
    error.clear();
    SOL_CHECK(!reflectShader(truncated, interface, &error));
    SOL_CHECK(error.find("past the end") != std::string::npos);

    // ⚑ And a zero-length instruction, which is the same defect in the
    // direction that loops forever rather than reading off the end.
    std::vector<std::uint32_t> zeroLength;
    SOL_REQUIRE(loadShader("mesh.frag.spv", zeroLength));
    zeroLength[5] = zeroLength[5] & 0xFFFFu;
    error.clear();
    SOL_CHECK(!reflectShader(zeroLength, interface, &error));
    SOL_CHECK(!error.empty());
}
