#include "sol/assets/texture_doc.hpp"
#include "sol/test/test.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sol;
using assets::TextureDoc;
using assets::TextureImage;
using assets::TextureOp;

namespace {

[[nodiscard]] bool parses(const std::string& text, TextureDoc& out)
{
    std::string error;
    if (assets::parseTexture(text.c_str(), text.size(), "test.tex", out, &error)) {
        return true;
    }
    std::printf("  unexpected parse failure: %s\n", error.c_str());
    return false;
}

[[nodiscard]] std::string rejects(const std::string& text)
{
    TextureDoc doc;
    std::string error;
    if (assets::parseTexture(text.c_str(), text.size(), "test.tex", doc, &error)) {
        return {};
    }
    return error.empty() ? "rejected" : error;
}

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

// "the file differs" is a poor thing to learn about a 100-row asset, so say
// which line and show both.
void reportFirstDifferingLine(const char* name, const std::string& expected,
                              const std::string& actual)
{
    std::size_t line = 1;
    std::size_t start = 0;
    const std::size_t shortest = expected.size() < actual.size() ? expected.size() : actual.size();
    for (std::size_t i = 0; i < shortest; ++i) {
        if (expected[i] != actual[i]) {
            const std::size_t expectedEnd = expected.find('\n', start);
            const std::size_t actualEnd = actual.find('\n', start);
            std::printf("  %s line %zu\n    source: %s\n    writer: %s\n", name, line,
                        expected.substr(start, expectedEnd - start).c_str(),
                        actual.substr(start, actualEnd - start).c_str());
            return;
        }
        if (expected[i] == '\n') {
            ++line;
            start = i + 1;
        }
    }
    std::printf("  %s: identical for %zu bytes, then source has %zu and writer %zu\n", name,
                shortest, expected.size(), actual.size());
}

// The three committed sources, by stem.
[[nodiscard]] std::vector<std::string> committedTextures()
{
    return {"checker", "hull", "cockpit"};
}

[[nodiscard]] std::string texturePath(const std::string& stem)
{
    return std::string(SOL_TEXTURE_SOURCE_DIR) + "/" + stem + ".tex";
}

// Pixel at (x, y) as three channels; alpha is asserted separately since every
// pixel of every texture here is opaque.
struct Rgb
{
    int r = 0;
    int g = 0;
    int b = 0;

    [[nodiscard]] bool operator==(const Rgb& other) const
    {
        return r == other.r && g == other.g && b == other.b;
    }
};

[[nodiscard]] Rgb pixelAt(const TextureImage& image, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {image.pixels[index], image.pixels[index + 1], image.pixels[index + 2]};
}

[[nodiscard]] bool builds(const std::string& text, TextureImage& out)
{
    TextureDoc doc;
    if (!parses(text, doc)) {
        return false;
    }
    std::string error;
    if (assets::buildTexture(doc, out, &error)) {
        return true;
    }
    std::printf("  unexpected build failure: %s\n", error.c_str());
    return false;
}

} // namespace

SOL_TEST(textureParsesAnOpListInFileOrder)
{
    const std::string source = R"(size = [8, 4]

[[op]]
kind = "fill"
color = [10, 20, 30]

[[op]]
kind = "lines"
color = [1, 2, 3]
width = 2
vertical = [5]
horizontal = [1, 2]
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(doc.width == 8);
    SOL_CHECK(doc.height == 4);
    SOL_REQUIRE(doc.layers.size() == 2);
    SOL_CHECK(doc.layers[0].op == TextureOp::Fill);
    SOL_CHECK(doc.layers[1].op == TextureOp::Lines);
    SOL_CHECK(doc.layers[0].value("color").color.g == 20);
    SOL_CHECK(doc.layers[1].value("width").integer == 2);
    SOL_REQUIRE(doc.layers[1].value("horizontal").integers.size() == 2);
    SOL_CHECK(doc.layers[1].value("horizontal").integers[1] == 2);
    // Unauthored parameters read as their schema default rather than as zero.
    SOL_CHECK(doc.layers[1].value("vertical").integers.size() == 1);
    SOL_CHECK(doc.layers[0].find("color") != nullptr);
}

SOL_TEST(textureRejectsMalformedDocuments)
{
    const std::string prefix = "size = [4, 4]\n\n[[op]]\n";
    SOL_CHECK(!rejects("[[op]]\nkind = \"fill\"\n").empty());                    // no size
    SOL_CHECK(!rejects("size = [0, 4]\n").empty());                              // zero dimension
    SOL_CHECK(!rejects("size = [4]\n").empty());                                 // one dimension
    SOL_CHECK(!rejects(prefix + "kind = \"splatter\"\n").empty());               // unknown op
    SOL_CHECK(!rejects(prefix + "color = [1, 2, 3]\n").empty());                 // no kind
    SOL_CHECK(!rejects(prefix + "kind = \"fill\"\nwidth = 2\n").empty());        // wrong parameter
    SOL_CHECK(!rejects(prefix + "kind = \"fill\"\ncolor = [1, 2]\n").empty());   // short colour
    SOL_CHECK(!rejects(prefix + "kind = \"fill\"\ncolor = [1, 2, 300]\n").empty()); // out of range
    SOL_CHECK(
        !rejects(prefix + "kind = \"rects\"\nrects = [[1, 2, 3]]\n").empty()); // 3 of 4 numbers
    SOL_CHECK(!rejects(prefix + "kind = \"panels\"\npanels = [[1, 2, 3, 4]]\n")
                   .empty()); // a rect row in a panel list
    SOL_CHECK(!rejects(prefix + "kind = \"rects\"\nrects = [[1, 2, -3, 4]]\n")
                   .empty()); // negative size
    // ⚑ A tint may be negative - that is the point of an offset - so the same
    // check must NOT fire here, or "make the panels cooler" is unexpressible.
    SOL_CHECK(rejects(prefix + "kind = \"panels\"\ntint = [-4, 0, 8]\n").empty());
}

SOL_TEST(textureRoundTripsThroughItsOwnWriter)
{
    // Comments in all three positions the model supports: a file header, above
    // an op, and a blank line the author put between them.
    const std::string source = R"(# a header
# over two lines

size = [16, 16]

# the background
[[op]]
kind = "fill"
color = [1, 2, 3]

[[op]]
kind = "panels"
tint = [0, 3, 8]
panels = [
  [1, 2, 3, 4, 90],
  [5, 6, 7, 8, 100],
]
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(!doc.hasUnplaceableComments);
    SOL_CHECK(assets::writeTexture(doc) == source);
}

SOL_TEST(textureWritesAnAuthoredValueEvenWhereItEqualsItsDefault)
{
    // ⚑ `writeForge` learned this the expensive way: skipping a value equal to
    // its schema default deleted four lines people had typed on purpose across
    // the shipped meshes. An author who writes `width = 1` down is naming the
    // knob, and a tool that answers by deleting the line is not one they will
    // trust with the file.
    const std::string source = R"(size = [4, 4]

[[op]]
kind = "lines"
width = 1
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(assets::writeTexture(doc) == source);
}

SOL_TEST(everyCommittedTextureRoundTripsByteForByte)
{
    for (const std::string& stem : committedTextures()) {
        const std::string source = readWholeFile(texturePath(stem));
        SOL_REQUIRE(!source.empty());
        TextureDoc doc;
        SOL_REQUIRE(parses(source, doc));
        SOL_CHECK(!doc.hasUnplaceableComments);
        const std::string written = assets::writeTexture(doc);
        if (written != source) {
            reportFirstDifferingLine((stem + ".tex").c_str(), source, written);
        }
        SOL_CHECK(written == source);
    }
}

SOL_TEST(everyCommittedTextureBuildsOpaqueAtItsDeclaredSize)
{
    for (const std::string& stem : committedTextures()) {
        TextureDoc doc;
        SOL_REQUIRE(parses(readWholeFile(texturePath(stem)), doc));
        TextureImage image;
        SOL_REQUIRE(assets::buildTexture(doc, image, nullptr));
        SOL_CHECK(image.width == static_cast<std::uint32_t>(doc.width));
        SOL_CHECK(image.height == static_cast<std::uint32_t>(doc.height));
        SOL_REQUIRE(image.pixels.size() ==
                    static_cast<std::size_t>(doc.width) * doc.height * 4);
        bool opaque = true;
        for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
            opaque = opaque && image.pixels[i] == 255;
        }
        SOL_CHECK(opaque);
    }
}

SOL_TEST(everyCommittedTextureLayerDrawsSomething)
{
    // ⚑ The invariant that replaces a pixel hash, per Phase 16: a BROKEN source
    // fails and a CHANGED one does not. An op that covers no pixel is either an
    // empty row list or geometry entirely off the canvas, and both are mistakes
    // rather than styles. Moving any op's rows outside the image fails this.
    for (const std::string& stem : committedTextures()) {
        TextureDoc doc;
        SOL_REQUIRE(parses(readWholeFile(texturePath(stem)), doc));
        SOL_REQUIRE(!doc.layers.empty());
        for (std::size_t i = 0; i < doc.layers.size(); ++i) {
            const std::size_t covered = assets::textureLayerCoverage(doc, i);
            if (covered == 0) {
                std::printf("  %s.tex op %zu (%s) draws nothing\n", stem.c_str(), i,
                            assets::textureOpName(doc.layers[i].op));
            }
            SOL_CHECK(covered > 0);
        }
    }
}

SOL_TEST(aWidthTwoPenCoversTheColumnBeforeItsCentre)
{
    // ⚑ Measured off the committed PNGs before this format existed, not chosen:
    // GDI+ centred a 2 px pen so that a seam at c lit [c-1, c]. Getting this
    // backwards would shift every seam in the repo by one pixel, which is
    // exactly the kind of "close enough" a transcription must not be.
    const std::string source = R"(size = [8, 8]

[[op]]
kind = "lines"
color = [255, 0, 0]
width = 2
vertical = [4]
horizontal = [6]
)";
    TextureImage image;
    SOL_REQUIRE(builds(source, image));
    const Rgb red{255, 0, 0};
    const Rgb black{0, 0, 0};
    SOL_CHECK(pixelAt(image, 2, 0) == black);
    SOL_CHECK(pixelAt(image, 3, 0) == red);
    SOL_CHECK(pixelAt(image, 4, 0) == red);
    SOL_CHECK(pixelAt(image, 5, 0) == black);
    SOL_CHECK(pixelAt(image, 0, 4) == black);
    SOL_CHECK(pixelAt(image, 0, 5) == red);
    SOL_CHECK(pixelAt(image, 0, 6) == red);
    SOL_CHECK(pixelAt(image, 0, 7) == black);
}

SOL_TEST(aRectCoversItsOwnBoxAndNothingBeside)
{
    const std::string source = R"(size = [8, 8]

[[op]]
kind = "rects"
color = [0, 255, 0]
rects = [
  [2, 3, 3, 2],
]
)";
    TextureImage image;
    SOL_REQUIRE(builds(source, image));
    const Rgb green{0, 255, 0};
    const Rgb black{0, 0, 0};
    SOL_CHECK(pixelAt(image, 2, 3) == green);
    SOL_CHECK(pixelAt(image, 4, 4) == green); // last covered column and row
    SOL_CHECK(pixelAt(image, 5, 3) == black); // [x, x+w), so x+w is outside
    SOL_CHECK(pixelAt(image, 2, 5) == black);
    SOL_CHECK(pixelAt(image, 1, 3) == black);
}

SOL_TEST(opsApplyInFileOrderSoALaterOpCoversAnEarlierOne)
{
    const std::string source = R"(size = [4, 4]

[[op]]
kind = "fill"
color = [10, 10, 10]

[[op]]
kind = "rects"
color = [20, 20, 20]
rects = [
  [0, 0, 4, 4],
]

[[op]]
kind = "rects"
color = [30, 30, 30]
rects = [
  [1, 1, 1, 1],
]
)";
    TextureImage image;
    SOL_REQUIRE(builds(source, image));
    SOL_CHECK(pixelAt(image, 0, 0) == (Rgb{20, 20, 20}));
    SOL_CHECK(pixelAt(image, 1, 1) == (Rgb{30, 30, 30}));
}

SOL_TEST(drawingIsClippedRatherThanRefused)
{
    // A panel hanging off the edge is a normal authoring move, so it builds and
    // paints the part that lands. Only an op that lands NOWHERE is a defect,
    // which is what the coverage invariant is for.
    const std::string source = R"(size = [4, 4]

[[op]]
kind = "rects"
color = [7, 8, 9]
rects = [
  [-2, -2, 3, 3],
  [3, 3, 8, 8],
]
)";
    TextureImage image;
    SOL_REQUIRE(builds(source, image));
    SOL_CHECK(pixelAt(image, 0, 0) == (Rgb{7, 8, 9}));
    SOL_CHECK(pixelAt(image, 3, 3) == (Rgb{7, 8, 9}));
    SOL_CHECK(pixelAt(image, 2, 2) == (Rgb{0, 0, 0}));

    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(assets::textureLayerCoverage(doc, 0) == 2); // one pixel from each
}

SOL_TEST(panelsTakeTheirColourFromShadePlusTintAndClamp)
{
    const std::string source = R"(size = [4, 2]

[[op]]
kind = "panels"
tint = [0, 3, 8]
panels = [
  [0, 0, 1, 1, 100],
  [1, 0, 1, 1, 250],
]
)";
    TextureImage image;
    SOL_REQUIRE(builds(source, image));
    SOL_CHECK(pixelAt(image, 0, 0) == (Rgb{100, 103, 108}));
    // 250 + 8 exceeds the channel, and a texture that wrapped to 2 would show a
    // black speck in the brightest place on the panel.
    SOL_CHECK(pixelAt(image, 1, 0) == (Rgb{250, 253, 255}));
}

SOL_TEST(aCheckerAlternatesFromTheTopLeftCell)
{
    const std::string source = R"(size = [4, 4]

[[op]]
kind = "checker"
cell = 2
color_a = [1, 1, 1]
color_b = [2, 2, 2]
)";
    TextureImage image;
    SOL_REQUIRE(builds(source, image));
    SOL_CHECK(pixelAt(image, 0, 0) == (Rgb{1, 1, 1}));
    SOL_CHECK(pixelAt(image, 1, 1) == (Rgb{1, 1, 1})); // same cell
    SOL_CHECK(pixelAt(image, 2, 0) == (Rgb{2, 2, 2}));
    SOL_CHECK(pixelAt(image, 0, 2) == (Rgb{2, 2, 2}));
    SOL_CHECK(pixelAt(image, 2, 2) == (Rgb{1, 1, 1}));
}
