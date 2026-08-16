#include <sol/assets/font.hpp>

#include <sol/test/synthetic_cooked_font.hpp>
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

using sol::test::CookedFontBuilder;

std::vector<std::uint8_t> buildTwoStyleFont()
{
    return sol::test::buildSyntheticCookedFont();
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
    CookedFontBuilder overrun;
    overrun.addStyle("hud", 16.0f, sol::test::syntheticCharset());
    overrun.styles[0].glyphCount = 99;
    SOL_CHECK(!font.loadFromMemory(overrun.encode()));

    // A glyph rect that leaves the atlas would index out of bounds at draw time.
    CookedFontBuilder escapes;
    escapes.addStyle("hud", 16.0f, sol::test::syntheticCharset());
    escapes.glyphs[2].atlasX = static_cast<std::uint16_t>(escapes.atlasWidth);
    escapes.glyphs[2].width = 4;
    SOL_CHECK(!font.loadFromMemory(escapes.encode()));

    // An unterminated style name would run off the end of the char array.
    CookedFontBuilder unterminated;
    unterminated.addStyle("hud", 16.0f, sol::test::syntheticCharset());
    std::memset(unterminated.styles[0].name, 'x', sol::assets::kFontStyleNameCapacity);
    SOL_CHECK(!font.loadFromMemory(unterminated.encode()));

    // A rejected load leaves the font unusable rather than half-populated.
    SOL_CHECK(!font.valid());
    SOL_CHECK(font.styles().empty());
    SOL_CHECK(font.style("hud") == nullptr);
}
