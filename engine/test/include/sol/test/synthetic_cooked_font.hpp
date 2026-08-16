#pragma once

// A cooked .sfont assembled directly from its records, for tests that need a
// loadable font without running the cooker (which sits above both the assets
// and ui layers). Glyphs are uniform 2x2 boxes with a known advance, so
// layout assertions can be exact arithmetic rather than font trivia.

#include "sol/assets/formats.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sol::test {

inline constexpr float kSyntheticAdvance = 5.0f;
inline constexpr float kSyntheticSpaceAdvance = 3.0f;
inline constexpr std::uint16_t kSyntheticGlyphSize = 2;

struct CookedFontBuilder
{
    std::vector<assets::FontStyleRecord> styles;
    std::vector<assets::FontGlyphRecord> glyphs;
    std::uint32_t atlasWidth = 16;
    std::uint32_t atlasHeight = 16;

    // `codepoints` must be sorted ascending, as the cooker emits them.
    void addStyle(const char* name, float pixelSize, const std::vector<char32_t>& codepoints)
    {
        assets::FontStyleRecord style = {};
        std::memcpy(style.name, name, std::strlen(name));
        style.pixelSize = pixelSize;
        style.ascent = pixelSize * 0.8f;
        style.descent = -pixelSize * 0.2f;
        style.lineHeight = pixelSize * 1.1f;
        style.firstGlyph = static_cast<std::uint32_t>(glyphs.size());
        style.glyphCount = static_cast<std::uint32_t>(codepoints.size());
        styles.push_back(style);

        for (const char32_t codepoint : codepoints) {
            assets::FontGlyphRecord glyph = {};
            glyph.codepoint = static_cast<std::uint32_t>(codepoint);
            glyph.atlasX = 2;
            glyph.atlasY = 4;
            // Space carries an advance and no ink, like a real font.
            const bool blank = codepoint == U' ';
            glyph.width = blank ? 0 : kSyntheticGlyphSize;
            glyph.height = blank ? 0 : kSyntheticGlyphSize;
            glyph.bearingX = 0;
            glyph.bearingY = -static_cast<std::int16_t>(kSyntheticGlyphSize);
            glyph.advance = blank ? kSyntheticSpaceAdvance : kSyntheticAdvance;
            glyphs.push_back(glyph);
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> encode() const
    {
        assets::FontFileHeader header = {};
        header.styleCount = static_cast<std::uint32_t>(styles.size());
        header.glyphCount = static_cast<std::uint32_t>(glyphs.size());
        header.atlasWidth = atlasWidth;
        header.atlasHeight = atlasHeight;

        const std::size_t styleBytes = styles.size() * sizeof(assets::FontStyleRecord);
        const std::size_t glyphBytes = glyphs.size() * sizeof(assets::FontGlyphRecord);
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

// Codepoints present in the default test font, sorted.
inline const std::vector<char32_t>& syntheticCharset()
{
    static const std::vector<char32_t> charset = {U' ', U'?', U'A', U'B', U'a'};
    return charset;
}

// Two styles: "hud" at 16 px and "heading" at 32 px.
inline std::vector<std::uint8_t> buildSyntheticCookedFont()
{
    CookedFontBuilder builder;
    builder.addStyle("hud", 16.0f, syntheticCharset());
    builder.addStyle("heading", 32.0f, syntheticCharset());
    return builder.encode();
}

} // namespace sol::test
