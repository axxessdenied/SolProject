#include "font.hpp"

#include "sol/assets/formats.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/test/synthetic_font.hpp"
#include "sol/test/test.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sol;

namespace {

// Drops the shared synthetic TTF next to the test executable so manifests can
// name it, and removes it again afterwards.
class ScopedTestFont
{
public:
    ScopedTestFont()
        : m_directory(platform::executableDirectory()), m_path(m_directory + "test_synthetic.ttf")
    {
        const std::vector<std::uint8_t> bytes = test::buildSyntheticFont();
        m_written = platform::writeFileBytes(m_path.c_str(), bytes.data(), bytes.size());
    }
    ~ScopedTestFont() { std::remove(m_path.c_str()); }

    ScopedTestFont(const ScopedTestFont&) = delete;
    ScopedTestFont& operator=(const ScopedTestFont&) = delete;

    [[nodiscard]] bool written() const { return m_written; }
    [[nodiscard]] const std::string& directory() const { return m_directory; }

private:
    std::string m_directory;
    std::string m_path;
    bool m_written = false;
};

bool bake(const ScopedTestFont& font, const char* manifest, cooker::BakedFont& out,
          std::string* outError = nullptr)
{
    return cooker::bakeFont(manifest, std::strlen(manifest), font.directory(), out, outError);
}

constexpr const char* kTwoStyleManifest = R"(
atlas_width = 128
padding = 1
codepoints = [0x00B0]

[[style]]
name = "small"
source = "test_synthetic.ttf"
size = 12

[[style]]
name = "large"
source = "test_synthetic.ttf"
size = 20
)";

// Printable ASCII plus the one extra codepoint the manifest asks for.
constexpr std::uint32_t kExpectedPerStyle = (126 - 32 + 1) + 1;

assets::FontGlyphRecord findGlyph(const cooker::BakedFont& font, std::uint32_t style,
                                  std::uint32_t codepoint)
{
    const assets::FontStyleRecord& record = font.styles[style];
    for (std::uint32_t i = 0; i < record.glyphCount; ++i) {
        if (font.glyphs[record.firstGlyph + i].codepoint == codepoint) {
            return font.glyphs[record.firstGlyph + i];
        }
    }
    return {};
}

} // namespace

SOL_TEST(fontBakesStylesIntoOneAtlas)
{
    const ScopedTestFont font;
    SOL_REQUIRE(font.written());

    cooker::BakedFont baked;
    std::string error;
    SOL_REQUIRE(bake(font, kTwoStyleManifest, baked, &error));

    SOL_REQUIRE(baked.styles.size() == 2);
    SOL_CHECK(baked.glyphs.size() == 2 * kExpectedPerStyle);
    SOL_CHECK(std::strcmp(baked.styles[0].name, "small") == 0);
    SOL_CHECK(std::strcmp(baked.styles[1].name, "large") == 0);
    SOL_CHECK(baked.styles[0].glyphCount == kExpectedPerStyle);
    SOL_CHECK(baked.styles[0].firstGlyph == 0);
    SOL_CHECK(baked.styles[1].firstGlyph == kExpectedPerStyle);

    // Metrics scale with the requested size (unitsPerEm 1000, ascent 800).
    SOL_CHECK(baked.styles[0].pixelSize == 12.0f);
    SOL_CHECK(baked.styles[0].ascent > 9.5f && baked.styles[0].ascent < 9.7f);
    SOL_CHECK(baked.styles[1].ascent > baked.styles[0].ascent);
    SOL_CHECK(baked.styles[0].descent < 0.0f);

    // One shared atlas, sized as configured and allocated to match.
    SOL_CHECK(baked.atlasWidth == 128);
    SOL_CHECK(baked.atlasHeight > 0);
    SOL_CHECK(baked.atlas.size() == static_cast<std::size_t>(baked.atlasWidth) * baked.atlasHeight);

    // Glyphs sort by codepoint within a style, and every rect fits the sheet.
    for (const assets::FontStyleRecord& style : baked.styles) {
        for (std::uint32_t i = 1; i < style.glyphCount; ++i) {
            SOL_REQUIRE(baked.glyphs[style.firstGlyph + i - 1].codepoint <
                        baked.glyphs[style.firstGlyph + i].codepoint);
        }
    }
    for (const assets::FontGlyphRecord& glyph : baked.glyphs) {
        SOL_REQUIRE(static_cast<std::uint32_t>(glyph.atlasX) + glyph.width <= baked.atlasWidth);
        SOL_REQUIRE(static_cast<std::uint32_t>(glyph.atlasY) + glyph.height <= baked.atlasHeight);
    }

    // 'A' is a plain square in the synthetic font: ink, advance, above the
    // baseline. Space has an advance and nothing else.
    const assets::FontGlyphRecord letter = findGlyph(baked, 0, 'A');
    SOL_CHECK(letter.width > 0 && letter.height > 0);
    SOL_CHECK(letter.advance > 0.0f);
    SOL_CHECK(letter.bearingY < 0);
    const assets::FontGlyphRecord space = findGlyph(baked, 0, ' ');
    SOL_CHECK(space.width == 0 && space.height == 0);
    SOL_CHECK(space.advance > 0.0f);

    // The larger style really is larger, not a rescaled copy of the same box.
    SOL_CHECK(findGlyph(baked, 1, 'A').height > letter.height);
}

SOL_TEST(fontUnmappedCodepointBakesNotdef)
{
    const ScopedTestFont font;
    SOL_REQUIRE(font.written());

    // 'Z' has no cmap entry in the synthetic font, so it must bake as glyph 0
    // rather than disappear from the atlas.
    cooker::BakedFont baked;
    SOL_REQUIRE(bake(font, kTwoStyleManifest, baked));
    const assets::FontGlyphRecord missing = findGlyph(baked, 0, 'Z');
    SOL_CHECK(missing.codepoint == 'Z');
    SOL_CHECK(missing.advance > 0.0f);
}

SOL_TEST(fontGlyphRectsDoNotOverlap)
{
    const ScopedTestFont font;
    SOL_REQUIRE(font.written());

    cooker::BakedFont baked;
    SOL_REQUIRE(bake(font, kTwoStyleManifest, baked));

    // Two glyphs sharing pixels would render text with pieces of its
    // neighbours attached.
    for (std::size_t i = 0; i < baked.glyphs.size(); ++i) {
        const assets::FontGlyphRecord& a = baked.glyphs[i];
        if (a.width == 0 || a.height == 0) {
            continue;
        }
        for (std::size_t j = i + 1; j < baked.glyphs.size(); ++j) {
            const assets::FontGlyphRecord& b = baked.glyphs[j];
            if (b.width == 0 || b.height == 0) {
                continue;
            }
            const bool disjoint = a.atlasX + a.width <= b.atlasX || b.atlasX + b.width <= a.atlasX ||
                                  a.atlasY + a.height <= b.atlasY || b.atlasY + b.height <= a.atlasY;
            SOL_REQUIRE(disjoint);
        }
    }
}

SOL_TEST(fontBakeIsDeterministic)
{
    const ScopedTestFont font;
    SOL_REQUIRE(font.written());

    // Cooked output must be byte-identical run to run, or every build churns
    // the asset and incremental cooking means nothing.
    cooker::BakedFont first;
    cooker::BakedFont second;
    SOL_REQUIRE(bake(font, kTwoStyleManifest, first));
    SOL_REQUIRE(bake(font, kTwoStyleManifest, second));
    SOL_CHECK(cooker::encodeFont(first) == cooker::encodeFont(second));
}

SOL_TEST(fontEncodesExpectedFileLayout)
{
    const ScopedTestFont font;
    SOL_REQUIRE(font.written());

    cooker::BakedFont baked;
    SOL_REQUIRE(bake(font, kTwoStyleManifest, baked));
    const std::vector<std::uint8_t> bytes = cooker::encodeFont(baked);

    assets::FontFileHeader header = {};
    SOL_REQUIRE(bytes.size() >= sizeof(header));
    std::memcpy(&header, bytes.data(), sizeof(header));
    SOL_CHECK(header.magic == assets::kFontMagic);
    SOL_CHECK(header.version == assets::kFormatVersion);
    SOL_CHECK(header.styleCount == baked.styles.size());
    SOL_CHECK(header.glyphCount == baked.glyphs.size());
    SOL_CHECK(header.atlasWidth == baked.atlasWidth);
    SOL_CHECK(header.atlasHeight == baked.atlasHeight);

    const std::size_t expected = sizeof(header) + baked.styles.size() * sizeof(assets::FontStyleRecord) +
                                 baked.glyphs.size() * sizeof(assets::FontGlyphRecord) + baked.atlas.size();
    SOL_CHECK(bytes.size() == expected);
}

SOL_TEST(fontManifestRejectsBadInput)
{
    const ScopedTestFont font;
    SOL_REQUIRE(font.written());

    cooker::BakedFont baked;
    std::string error;

    // Unknown keys are errors, matching the strict-schema defs contract.
    SOL_CHECK(!bake(font, "wobble = 3\n[[style]]\nname=\"a\"\nsource=\"test_synthetic.ttf\"\nsize=10\n",
                    baked, &error));
    SOL_CHECK(!error.empty());
    SOL_CHECK(!bake(font, "[[style]]\nname=\"a\"\nsource=\"test_synthetic.ttf\"\nsize=10\nbold=true\n",
                    baked, &error));

    // A font with no styles produces nothing usable.
    SOL_CHECK(!bake(font, "atlas_width = 128\n", baked, &error));

    // Required fields, sane ranges, unique names, and a readable source.
    SOL_CHECK(!bake(font, "[[style]]\nsource=\"test_synthetic.ttf\"\nsize=10\n", baked, &error));
    SOL_CHECK(!bake(font, "[[style]]\nname=\"a\"\nsize=10\n", baked, &error));
    SOL_CHECK(!bake(font, "[[style]]\nname=\"a\"\nsource=\"test_synthetic.ttf\"\nsize=0\n", baked, &error));
    SOL_CHECK(!bake(font, "[[style]]\nname=\"a\"\nsource=\"test_synthetic.ttf\"\nsize=-4\n", baked, &error));
    SOL_CHECK(!bake(font,
                    "[[style]]\nname=\"a\"\nsource=\"test_synthetic.ttf\"\nsize=10\n"
                    "[[style]]\nname=\"a\"\nsource=\"test_synthetic.ttf\"\nsize=12\n",
                    baked, &error));
    SOL_CHECK(!bake(font, "[[style]]\nname=\"a\"\nsource=\"missing.ttf\"\nsize=10\n", baked, &error));

    // An atlas too narrow for the largest glyph fails loudly rather than
    // silently clipping text.
    SOL_CHECK(!bake(font, "atlas_width = 8\n[[style]]\nname=\"a\"\nsource=\"test_synthetic.ttf\"\nsize=64\n",
                    baked, &error));

    // A file that is not a TrueType font is rejected, not half-parsed.
    const std::string decoy = font.directory() + "not_a_font.ttf";
    SOL_REQUIRE(platform::writeFileBytes(decoy.c_str(), "definitely not a font", 21));
    SOL_CHECK(!bake(font, "[[style]]\nname=\"a\"\nsource=\"not_a_font.ttf\"\nsize=10\n", baked, &error));
    std::remove(decoy.c_str());
}
