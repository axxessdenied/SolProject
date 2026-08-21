// The acceptance gate for stage G's transcription: every `.tex` document must
// build the exact image the PNG it replaces decoded to.
//
// ⚑⚑ THIS TEST IS DELIBERATELY TEMPORARY AND SHOULD BE DELETED WITH THE PNGs.
// Phase 16 removed the file-vs-recipe tests because they were a RATCHET: they
// made a shipped asset immovable, and one of them blocked a real human edit. A
// permanent pixel comparison against a committed PNG would be that ratchet
// rebuilt in a new format - the moment somebody edits `hull.tex`, which is the
// entire point of stage G, this test becomes a false alarm. It exists to prove
// the transcription changed nothing, once, in the commit that performs it. What
// survives afterwards lives in `assets.unit`: the round trip, the build
// invariants, and the pixel rules asserted on documents that read no asset.
//
// ⚑ It lives here rather than beside those because `decodePng` is cooker code:
// `sol_cooker_tests` links `sol_cooker_lib`, which owns the decoder AND links
// `sol::assets` publicly, so this is the only suite that can see both halves.

#include "png.hpp"

#include "sol/assets/texture_doc.hpp"
#include "sol/test/test.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sol;

namespace {

[[nodiscard]] std::string readWholeFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("  cannot open %s\n", path.c_str());
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

SOL_TEST(everyTranscribedTextureBuildsTheImageItsPngDecodedTo)
{
    for (const char* stem : {"checker", "hull", "cockpit"}) {
        const std::string base = std::string(SOL_TEXTURE_SOURCE_DIR) + "/" + stem;

        const std::string source = readWholeFile(base + ".tex");
        SOL_REQUIRE(!source.empty());
        assets::TextureDoc doc;
        std::string error;
        if (!assets::parseTexture(source.c_str(), source.size(), stem, doc, &error)) {
            std::printf("  %s\n", error.c_str());
            SOL_REQUIRE(false);
        }
        assets::TextureImage built;
        SOL_REQUIRE(assets::buildTexture(doc, built, nullptr));

        const std::string pngBytes = readWholeFile(base + ".png");
        SOL_REQUIRE(!pngBytes.empty());
        cooker::ImageRgba decoded;
        SOL_REQUIRE(decodePng(reinterpret_cast<const std::uint8_t*>(pngBytes.data()),
                              pngBytes.size(), decoded));

        SOL_REQUIRE(built.width == decoded.width && built.height == decoded.height);
        SOL_REQUIRE(built.pixels.size() == decoded.pixels.size());

        // "the image differs" says nothing useful about 65,536 pixels, so name
        // the first one and show both colours.
        std::size_t differing = 0;
        std::size_t firstIndex = 0;
        for (std::size_t i = 0; i < built.pixels.size(); i += 4) {
            const bool same = built.pixels[i] == decoded.pixels[i] &&
                              built.pixels[i + 1] == decoded.pixels[i + 1] &&
                              built.pixels[i + 2] == decoded.pixels[i + 2] &&
                              built.pixels[i + 3] == decoded.pixels[i + 3];
            if (!same) {
                if (differing == 0) {
                    firstIndex = i;
                }
                ++differing;
            }
        }
        if (differing != 0) {
            const std::size_t pixel = firstIndex / 4;
            std::printf("  %s: %zu of %u pixels differ; first at (%zu, %zu) "
                        "built (%u,%u,%u,%u) png (%u,%u,%u,%u)\n",
                        stem, differing, built.width * built.height, pixel % built.width,
                        pixel / built.width, built.pixels[firstIndex],
                        built.pixels[firstIndex + 1], built.pixels[firstIndex + 2],
                        built.pixels[firstIndex + 3], decoded.pixels[firstIndex],
                        decoded.pixels[firstIndex + 1], decoded.pixels[firstIndex + 2],
                        decoded.pixels[firstIndex + 3]);
        }
        SOL_CHECK(differing == 0);
    }
}
