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
void reportFirstDifferingLine(const char* name, const std::string& expected, const std::string& actual)
{
    std::size_t line = 1;
    std::size_t start = 0;
    const std::size_t shortest = expected.size() < actual.size() ? expected.size() : actual.size();
    for (std::size_t i = 0; i < shortest; ++i) {
        if (expected[i] != actual[i]) {
            const std::size_t expectedEnd = expected.find('\n', start);
            const std::size_t actualEnd = actual.find('\n', start);
            std::printf("  %s line %zu\n    source: %s\n    writer: %s\n",
                        name,
                        line,
                        expected.substr(start, expectedEnd - start).c_str(),
                        actual.substr(start, actualEnd - start).c_str());
            return;
        }
        if (expected[i] == '\n') {
            ++line;
            start = i + 1;
        }
    }
    std::printf("  %s: identical for %zu bytes, then source has %zu and writer %zu\n",
                name,
                shortest,
                expected.size(),
                actual.size());
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
    SOL_CHECK(!rejects("[[op]]\nkind = \"fill\"\n").empty());                        // no size
    SOL_CHECK(!rejects("size = [0, 4]\n").empty());                                  // zero dimension
    SOL_CHECK(!rejects("size = [4]\n").empty());                                     // one dimension
    SOL_CHECK(!rejects(prefix + "kind = \"splatter\"\n").empty());                   // unknown op
    SOL_CHECK(!rejects(prefix + "color = [1, 2, 3]\n").empty());                     // no kind
    SOL_CHECK(!rejects(prefix + "kind = \"fill\"\nwidth = 2\n").empty());            // wrong parameter
    SOL_CHECK(!rejects(prefix + "kind = \"fill\"\ncolor = [1, 2]\n").empty());       // short colour
    SOL_CHECK(!rejects(prefix + "kind = \"fill\"\ncolor = [1, 2, 300]\n").empty());  // out of range
    SOL_CHECK(!rejects(prefix + "kind = \"rects\"\nrects = [[1, 2, 3]]\n").empty()); // 3 of 4 numbers
    SOL_CHECK(!rejects(prefix + "kind = \"panels\"\npanels = [[1, 2, 3, 4]]\n")
                   .empty()); // a rect row in a panel list
    SOL_CHECK(!rejects(prefix + "kind = \"rects\"\nrects = [[1, 2, -3, 4]]\n").empty()); // negative size
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
        SOL_REQUIRE(image.pixels.size() == static_cast<std::size_t>(doc.width) * doc.height * 4);
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
                std::printf("  %s.tex op %zu (%s) draws nothing\n",
                            stem.c_str(),
                            i,
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

// --- picking (stage I) ------------------------------------------------------

namespace {

// The colour a named row paints, re-derived from the document rather than read
// off the mask the hit test used.
//
// ⚑ That independence is the whole point of the test below. A hit test built on
// the builder's own walk agrees with the builder by construction, so asserting
// it against that same walk would assert nothing. This re-reads the schema,
// says what colour that row WOULD paint, and makes the picture match.
[[nodiscard]] bool rowColor(const TextureDoc& doc, assets::TextureHit hit, int x, int y, Rgb& out)
{
    if (!hit.valid()) {
        return false;
    }
    const assets::TextureLayer& layer = doc.layers[static_cast<std::size_t>(hit.layer)];
    const auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    const auto plain = [&](const char* name) {
        const assets::TextureColor c = layer.value(name).color;
        out = {clamp(c.r), clamp(c.g), clamp(c.b)};
        return true;
    };
    switch (layer.op) {
    case TextureOp::Fill:
    case TextureOp::Rects:
    case TextureOp::Lines:
        return plain("color");
    case TextureOp::Checker: {
        const int cell = static_cast<int>(layer.value("cell").integer);
        return plain(((x / cell) + (y / cell)) % 2 == 0 ? "color_a" : "color_b");
    }
    case TextureOp::Panels: {
        const std::vector<assets::TexturePanel> panels = layer.value("panels").panels;
        if (hit.row < 0 || static_cast<std::size_t>(hit.row) >= panels.size()) {
            return false;
        }
        const assets::TextureColor tint = layer.value("tint").color;
        const int shade = panels[static_cast<std::size_t>(hit.row)].shade;
        out = {clamp(shade + tint.r), clamp(shade + tint.g), clamp(shade + tint.b)};
        return true;
    }
    }
    return false;
}

} // namespace

SOL_TEST(everyPixelIsClaimedByTheRowWhoseColourItWears)
{
    // ⚑⚑ ASSERTION (1) OF STAGE I, AND THE ONE THE WHOLE PICK RESTS ON. Over
    // every pixel of all three committed textures, the row the hit test names
    // must be one that would paint exactly the colour standing there. It
    // catches an off-by-one in the half-open box, a mis-ordered file, a
    // width-2 pen taken from the wrong side, and any drift between the walk
    // that draws and the walk that attributes - which is the failure a
    // geometric hit test would ship in silence.
    for (const std::string& stem : committedTextures()) {
        TextureDoc doc;
        SOL_REQUIRE(parses(readWholeFile(texturePath(stem)), doc));
        TextureImage image;
        SOL_REQUIRE(assets::buildTexture(doc, image, nullptr));
        std::vector<assets::TextureHit> map;
        SOL_REQUIRE(assets::textureAttribution(doc, map));
        SOL_REQUIRE(map.size() == static_cast<std::size_t>(doc.width) * doc.height);

        std::size_t mismatches = 0;
        std::size_t unclaimed = 0;
        for (int y = 0; y < doc.height; ++y) {
            for (int x = 0; x < doc.width; ++x) {
                const assets::TextureHit hit =
                    map[static_cast<std::size_t>(y) * doc.width + static_cast<std::size_t>(x)];
                if (!hit.valid()) {
                    ++unclaimed;
                    continue;
                }
                Rgb expected;
                const Rgb actual = pixelAt(image, x, y);
                if (!rowColor(doc, hit, x, y, expected) || !(expected == actual)) {
                    if (mismatches == 0) {
                        std::printf("  %s.tex (%d, %d): op %d row %d paints (%d, %d, %d) but the "
                                    "image is (%d, %d, %d)\n",
                                    stem.c_str(),
                                    x,
                                    y,
                                    hit.layer,
                                    hit.row,
                                    expected.r,
                                    expected.g,
                                    expected.b,
                                    actual.r,
                                    actual.g,
                                    actual.b);
                    }
                    ++mismatches;
                }
            }
        }
        // Each of these documents opens with a fill or a checker, so there is no
        // unpainted pixel to find; a document without one would be a different
        // assertion.
        SOL_CHECK(unclaimed == 0);
        SOL_CHECK(mismatches == 0);
    }
}

SOL_TEST(aLaterRowCoversAnEarlierOneInTheHitTestAsWellAsInThePicture)
{
    const std::string source = R"(size = [8, 8]

[[op]]
kind = "fill"
color = [1, 1, 1]

[[op]]
kind = "panels"
panels = [
  [0, 0, 6, 6, 40],
  [4, 4, 4, 4, 90],
]
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    // The overlap belongs to the row drawn last, which is the rule the picture
    // already follows.
    const assets::TextureHit overlap = assets::textureHitTest(doc, 5, 5);
    SOL_CHECK(overlap.layer == 1);
    SOL_CHECK(overlap.row == 1);
    const assets::TextureHit earlier = assets::textureHitTest(doc, 1, 1);
    SOL_CHECK(earlier.layer == 1);
    SOL_CHECK(earlier.row == 0);
    // Bare canvas is the fill, which has nothing to grab.
    const assets::TextureHit bare = assets::textureHitTest(doc, 7, 0);
    SOL_CHECK(bare.layer == 0);
    SOL_CHECK(bare.row == -1);
    SOL_CHECK(bare.valid());
    SOL_CHECK(!bare.movable());
}

SOL_TEST(lineRowsAreNumberedVerticalThenHorizontal)
{
    // ⚑ The ordinal is the DRAW order, so `vertical` runs first. Numbering them
    // the other way round drags the wrong seam, and it would read as a
    // coordinate bug rather than as an indexing one.
    const std::string source = R"(size = [16, 16]

[[op]]
kind = "fill"
color = [0, 0, 0]

[[op]]
kind = "lines"
color = [200, 200, 200]
width = 2
vertical = [3, 11]
horizontal = [7]
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_CHECK(assets::textureRowCount(doc.layers[1]) == 3);
    SOL_CHECK(assets::textureHitTest(doc, 3, 0).row == 0);  // first vertical
    SOL_CHECK(assets::textureHitTest(doc, 11, 0).row == 1); // second vertical
    SOL_CHECK(assets::textureHitTest(doc, 0, 7).row == 2);  // first horizontal
}

SOL_TEST(aRowCountIsWhatCanBeAddressedByOrdinal)
{
    const std::string source = R"(size = [8, 8]

[[op]]
kind = "fill"
color = [0, 0, 0]

[[op]]
kind = "checker"
cell = 2

[[op]]
kind = "rects"
rects = [
  [0, 0, 2, 2],
  [4, 4, 2, 2],
]
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    SOL_REQUIRE(doc.layers.size() == 3);
    // The two ops that paint without a list report nothing to address, which is
    // what stops a click on bare hull moving something.
    SOL_CHECK(assets::textureRowCount(doc.layers[0]) == 0);
    SOL_CHECK(assets::textureRowCount(doc.layers[1]) == 0);
    SOL_CHECK(assets::textureRowCount(doc.layers[2]) == 2);
}

SOL_TEST(settingARowPositionMovesThatRowAndRewritesNoOtherLine)
{
    // ⚑⚑ ASSERTION (2). E1's fixed point one format over: a drag writes the
    // line it moved and leaves every other byte alone, comments and blank
    // lines included.
    const std::string original = readWholeFile(texturePath("cockpit"));
    TextureDoc doc;
    SOL_REQUIRE(parses(original, doc));

    // Row 3 of the panel list, moved somewhere it certainly was not.
    const assets::TextureHit hit{1, 3};
    int x = 0;
    int y = 0;
    SOL_REQUIRE(assets::textureRowPosition(doc, hit, x, y));
    SOL_REQUIRE(assets::textureSetRowPosition(doc, hit, x + 5, y - 7));

    // ⚑ Both axes, read back. Without this the "exactly one line changed"
    // assertion below passes just as happily on a writer that moved x and
    // dropped y - the row's line differs either way.
    int movedX = 0;
    int movedY = 0;
    SOL_REQUIRE(assets::textureRowPosition(doc, hit, movedX, movedY));
    SOL_CHECK(movedX == x + 5);
    SOL_CHECK(movedY == y - 7);

    const std::string written = assets::writeTexture(doc);
    SOL_CHECK(written != original);
    std::size_t differing = 0;
    std::istringstream before(original);
    std::istringstream after(written);
    std::string a;
    std::string b;
    while (std::getline(before, a) && std::getline(after, b)) {
        differing += a == b ? 0u : 1u;
    }
    if (differing != 1) {
        reportFirstDifferingLine("cockpit.tex", original, written);
    }
    SOL_CHECK(differing == 1);
}

SOL_TEST(aZeroDistanceDragLeavesEveryCommittedTextureByteIdentical)
{
    // ⚑⚑ ASSERTION (3), AND IT IS PHASE 14's RULE MEETING E4's FOR A THIRD
    // TIME. Pressing on a row and releasing without moving must not rewrite the
    // file - and since stage G's proof is that the cooked bytes are identical
    // to what the game shipped, this is also what stops a stray click undoing
    // it. Every addressable row of every committed texture, set to where it
    // already is.
    for (const std::string& stem : committedTextures()) {
        const std::string original = readWholeFile(texturePath(stem));
        TextureDoc doc;
        SOL_REQUIRE(parses(original, doc));

        std::size_t rowsTried = 0;
        for (std::size_t layer = 0; layer < doc.layers.size(); ++layer) {
            const std::size_t rows = assets::textureRowCount(doc.layers[layer]);
            for (std::size_t row = 0; row < rows; ++row) {
                const assets::TextureHit hit{static_cast<int>(layer), static_cast<int>(row)};
                int x = 0;
                int y = 0;
                SOL_REQUIRE(assets::textureRowPosition(doc, hit, x, y));
                SOL_REQUIRE(assets::textureSetRowPosition(doc, hit, x, y));
                ++rowsTried;
            }
        }
        SOL_CHECK(rowsTried > 0);
        const std::string written = assets::writeTexture(doc);
        if (written != original) {
            reportFirstDifferingLine((stem + ".tex").c_str(), original, written);
        }
        SOL_CHECK(written == original);
    }
}

SOL_TEST(aSeamTakesItsOwnAxisAndDropsTheOther)
{
    // ⚑⚑ ASSERTION (5). A `lines` entry is ONE coordinate, so the axis it does
    // not have must be dropped rather than written somewhere harmless-looking:
    // writing a vertical seam's y into the neighbouring horizontal list is a
    // completely different seam moving.
    const std::string source = R"(size = [16, 16]

[[op]]
kind = "lines"
color = [200, 200, 200]
width = 2
vertical = [3, 11]
horizontal = [7]
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));

    // The second vertical seam, dragged to x = 9 with a y the op cannot use.
    SOL_REQUIRE(assets::textureSetRowPosition(doc, assets::TextureHit{0, 1}, 9, 99));
    SOL_CHECK(doc.layers[0].value("vertical").integers[0] == 3);
    SOL_CHECK(doc.layers[0].value("vertical").integers[1] == 9);
    SOL_REQUIRE(doc.layers[0].value("horizontal").integers.size() == 1);
    SOL_CHECK(doc.layers[0].value("horizontal").integers[0] == 7);

    // And the horizontal one takes y while ignoring x.
    SOL_REQUIRE(assets::textureSetRowPosition(doc, assets::TextureHit{0, 2}, 99, 4));
    SOL_CHECK(doc.layers[0].value("horizontal").integers[0] == 4);
    SOL_CHECK(doc.layers[0].value("vertical").integers[1] == 9);

    // A seam reads its own axis back, so a drag beginning on one does not snap
    // the other axis to the canvas edge.
    int x = 0;
    int y = 0;
    SOL_REQUIRE(assets::textureRowPosition(doc, assets::TextureHit{0, 1}, x, y));
    SOL_CHECK(x == 9);
}

SOL_TEST(anOpWithNoRowListRefusesToBeMoved)
{
    const std::string source = R"(size = [8, 8]

[[op]]
kind = "fill"
color = [5, 6, 7]

[[op]]
kind = "checker"
cell = 4
)";
    TextureDoc doc;
    SOL_REQUIRE(parses(source, doc));
    const std::string before = assets::writeTexture(doc);

    // The honest hit (row -1), a fabricated row and an out-of-range layer are
    // all refused, and the document is untouched by every one of them.
    SOL_CHECK(!assets::textureSetRowPosition(doc, assets::TextureHit{0, -1}, 3, 3));
    SOL_CHECK(!assets::textureSetRowPosition(doc, assets::TextureHit{0, 0}, 3, 3));
    SOL_CHECK(!assets::textureSetRowPosition(doc, assets::TextureHit{1, 0}, 3, 3));
    SOL_CHECK(!assets::textureSetRowPosition(doc, assets::TextureHit{5, 0}, 3, 3));
    SOL_CHECK(assets::writeTexture(doc) == before);
}
