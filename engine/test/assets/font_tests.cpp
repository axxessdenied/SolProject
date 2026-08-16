#include <sol/assets/font.hpp>

#include <sol/test/test.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using sol::assets::Font;
using sol::assets::FontFileHeader;
using sol::assets::FontGlyphRecord;
using sol::assets::FontStyleRecord;

namespace {

// Cooked fonts are plain records, so the tests assemble one directly rather
// than reaching for the cooker (which sits above this layer anyway).
struct FontBuilder
{
    std::vector<FontStyleRecord> styles;
    std::vector<FontGlyphRecord> glyphs;
    std::uint32_t atlasWidth = 16;
    std::uint32_t atlasHeight = 16;

    void addStyle(const char* name, float pixelSize, const std::vector<char32_t>& codepoints)
    {
        FontStyleRecord style = {};
        std::memcpy(style.name, name, std::strlen(name));
        style.pixelSize = pixelSize;
        style.ascent = pixelSize * 0.8f;
        style.descent = -pixelSize * 0.2f;
        style.lineHeight = pixelSize * 1.1f;
        style.firstGlyph = static_cast<std::uint32_t>(glyphs.size());
        style.glyphCount = static_cast<std::uint32_t>(codepoints.size());
        styles.push_back(style);

        for (const char32_t codepoint : codepoints) {
            FontGlyphRecord glyph = {};
            glyph.codepoint = static_cast<std::uint32_t>(codepoint);
            glyph.atlasX = 0;
            glyph.atlasY = 0;
            glyph.width = 2;
            glyph.height = 2;
            glyph.bearingX = 0;
            glyph.bearingY = -2;
            glyph.advance = codepoint == U' ' ? 3.0f : 5.0f;
            glyphs.push_back(glyph);
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> encode() const
    {
        FontFileHeader header = {};
        header.styleCount = static_cast<std::uint32_t>(styles.size());
        header.glyphCount = static_cast<std::uint32_t>(glyphs.size());
        header.atlasWidth = atlasWidth;
        header.atlasHeight = atlasHeight;

        const std::size_t styleBytes = styles.size() * sizeof(FontStyleRecord);
        const std::size_t glyphBytes = glyphs.size() * sizeof(FontGlyphRecord);
        const std::size_t atlasBytes = static_cast<std::size_t>(atlasWidth) * atlasHeight;

        std::vector<std::uint8_t> bytes(sizeof(header) + styleBytes + glyphBytes + atlasBytes, 0);
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (styleBytes != 0) {
            std::memcpy(bytes.data() + sizeof(header), styles.data(), styleBytes);
        }
        if (glyphBytes != 0) {
            std::memcpy(bytes.data() + sizeof(header) + styleBytes, glyphs.data(), glyphBytes);
        }
        return bytes;
    }
};

// Codepoints must be sorted, as the cooker emits them.
const std::vector<char32_t> kAscii = {U' ', U'?', U'A', U'B', U'a'};

std::vector<std::uint8_t> buildTwoStyleFont()
{
    FontBuilder builder;
    builder.addStyle("hud", 16.0f, kAscii);
    builder.addStyle("heading", 32.0f, kAscii);
    return builder.encode();
}

bool nearlyEqual(float a, float b)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < 1.0e-3f;
}

} // namespace

SOL_TEST(font_loads_styles_and_atlas)
{
    Font font;
    SOL_REQUIRE(font.loadFromMemory(buildTwoStyleFont()));
    SOL_CHECK(font.valid());
    SOL_CHECK(font.styles().size() == 2);
    SOL_CHECK(font.atlasWidth() == 16);
    SOL_CHECK(font.atlasHeight() == 16);
    SOL_CHECK(font.atlas().size() == 256);

    const FontStyleRecord* hud = font.style("hud");
    const FontStyleRecord* heading = font.style("heading");
    SOL_REQUIRE(hud != nullptr);
    SOL_REQUIRE(heading != nullptr);
    SOL_CHECK(nearlyEqual(hud->pixelSize, 16.0f));
    SOL_CHECK(nearlyEqual(heading->pixelSize, 32.0f));
    SOL_CHECK(font.style("nope") == nullptr);
    SOL_CHECK(font.style("") == nullptr);
}

SOL_TEST(font_looks_up_glyphs_within_a_style)
{
    Font font;
    SOL_REQUIRE(font.loadFromMemory(buildTwoStyleFont()));
    const FontStyleRecord* hud = font.style("hud");
    SOL_REQUIRE(hud != nullptr);

    const FontGlyphRecord* letter = font.glyph(*hud, U'A');
    SOL_REQUIRE(letter != nullptr);
    SOL_CHECK(letter->codepoint == static_cast<std::uint32_t>(U'A'));

    // A codepoint the font was never baked with shows '?' rather than nothing.
    const FontGlyphRecord* missing = font.glyph(*hud, U'中');
    SOL_REQUIRE(missing != nullptr);
    SOL_CHECK(missing->codepoint == static_cast<std::uint32_t>(U'?'));

    // Lookups stay inside their own style's run.
    const FontStyleRecord* heading = font.style("heading");
    SOL_REQUIRE(heading != nullptr);
    const FontGlyphRecord* headingA = font.glyph(*heading, U'A');
    SOL_REQUIRE(headingA != nullptr);
    SOL_CHECK(headingA != letter);
}

SOL_TEST(font_measures_utf8_runs)
{
    Font font;
    SOL_REQUIRE(font.loadFromMemory(buildTwoStyleFont()));
    const FontStyleRecord* hud = font.style("hud");
    SOL_REQUIRE(hud != nullptr);

    SOL_CHECK(nearlyEqual(font.measureWidth(*hud, ""), 0.0f));
    SOL_CHECK(nearlyEqual(font.measureWidth(*hud, "A"), 5.0f));
    SOL_CHECK(nearlyEqual(font.measureWidth(*hud, "AB"), 10.0f));
    SOL_CHECK(nearlyEqual(font.measureWidth(*hud, "A B"), 13.0f)); // space is narrower

    // Multi-byte input measures per scalar, not per byte.
    SOL_CHECK(nearlyEqual(font.measureWidth(*hud, "°"), 5.0f)); // falls back to '?'
}

SOL_TEST(font_decodes_utf8_including_malformed_input)
{
    using sol::assets::nextCodepoint;

    std::size_t cursor = 0;
    const std::string text = "a°中\U0001F600";
    SOL_CHECK(nextCodepoint(text, cursor) == U'a');
    SOL_CHECK(nextCodepoint(text, cursor) == 0x00B0);
    SOL_CHECK(nextCodepoint(text, cursor) == 0x4E2D);
    SOL_CHECK(nextCodepoint(text, cursor) == 0x1F600);
    SOL_CHECK(cursor == text.size());

    // Malformed bytes must yield U+FFFD and still advance, or a decode loop
    // would spin forever on bad input.
    const std::string broken = "\xFF\x80z";
    cursor = 0;
    SOL_CHECK(nextCodepoint(broken, cursor) == 0xFFFD);
    SOL_CHECK(cursor == 1);
    SOL_CHECK(nextCodepoint(broken, cursor) == 0xFFFD);
    SOL_CHECK(cursor == 2);
    SOL_CHECK(nextCodepoint(broken, cursor) == U'z');
    SOL_CHECK(cursor == 3);

    // A truncated sequence at the end of the buffer does not read past it.
    const std::string truncated = "a\xE4\xB8";
    cursor = 1;
    SOL_CHECK(nextCodepoint(truncated, cursor) == 0xFFFD);
    SOL_CHECK(cursor == 2);
}

SOL_TEST(font_rejects_corrupt_assets)
{
    Font font;

    SOL_CHECK(!font.loadFromMemory({}));
    SOL_CHECK(!font.valid());

    const std::vector<std::uint8_t> good = buildTwoStyleFont();

    // Wrong magic.
    std::vector<std::uint8_t> badMagic = good;
    badMagic[0] ^= 0xFF;
    SOL_CHECK(!font.loadFromMemory(badMagic));

    // Truncated payload: the declared atlas no longer fits.
    std::vector<std::uint8_t> truncated = good;
    truncated.resize(truncated.size() - 8);
    SOL_CHECK(!font.loadFromMemory(truncated));

    // A style whose glyph run runs off the end of the table.
    FontBuilder overrun;
    overrun.addStyle("hud", 16.0f, kAscii);
    overrun.styles[0].glyphCount = 99;
    SOL_CHECK(!font.loadFromMemory(overrun.encode()));

    // A glyph rect that leaves the atlas would index out of bounds at draw time.
    FontBuilder escapes;
    escapes.addStyle("hud", 16.0f, kAscii);
    escapes.glyphs[2].atlasX = static_cast<std::uint16_t>(escapes.atlasWidth);
    escapes.glyphs[2].width = 4;
    SOL_CHECK(!font.loadFromMemory(escapes.encode()));

    // An unterminated style name would run off the end of the char array.
    FontBuilder unterminated;
    unterminated.addStyle("hud", 16.0f, kAscii);
    std::memset(unterminated.styles[0].name, 'x', sol::assets::kFontStyleNameCapacity);
    SOL_CHECK(!font.loadFromMemory(unterminated.encode()));

    // A rejected load leaves the font unusable rather than half-populated.
    SOL_CHECK(!font.valid());
    SOL_CHECK(font.styles().empty());
    SOL_CHECK(font.style("hud") == nullptr);
}
