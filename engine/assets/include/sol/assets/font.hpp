#pragma once

#include "sol/assets/formats.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sol::assets {

// Decodes the UTF-8 scalar starting at `cursor` and advances it. Malformed
// bytes decode to U+FFFD and advance by one, so bad input can never stall a
// caller mid-string.
[[nodiscard]] char32_t nextCodepoint(std::string_view text, std::size_t& cursor);

// A cooked font (.sfont): one shared glyph atlas plus every style baked into
// it. Owns its pixels; the renderer uploads the atlas once and the UI asks
// this for placement.
class Font
{
public:
    [[nodiscard]] bool load(const char* path);
    [[nodiscard]] bool loadFromMemory(std::span<const std::uint8_t> bytes);

    [[nodiscard]] bool valid() const { return m_valid; }
    [[nodiscard]] std::uint32_t atlasWidth() const { return m_atlasWidth; }
    [[nodiscard]] std::uint32_t atlasHeight() const { return m_atlasHeight; }
    [[nodiscard]] std::span<const std::uint8_t> atlas() const { return m_atlas; }

    [[nodiscard]] std::span<const FontStyleRecord> styles() const { return m_styles; }

    // Style by cooked name ("hud", "body", ...); nullptr when absent.
    [[nodiscard]] const FontStyleRecord* style(std::string_view name) const;

    // Glyph for a codepoint within a style. Codepoints the font was not baked
    // with fall back to '?' so missing text is visible rather than silent;
    // nullptr only when even that is absent.
    [[nodiscard]] const FontGlyphRecord* glyph(const FontStyleRecord& style, char32_t codepoint) const;

    // Advance width of a UTF-8 run, in pixels. Kerning is not applied - see
    // the Phase 8d note on GPOS.
    [[nodiscard]] float measureWidth(const FontStyleRecord& style, std::string_view text) const;

private:
    bool m_valid = false;
    std::uint32_t m_atlasWidth = 0;
    std::uint32_t m_atlasHeight = 0;
    std::vector<std::uint8_t> m_atlas;
    std::vector<FontStyleRecord> m_styles;
    std::vector<FontGlyphRecord> m_glyphs;
};

} // namespace sol::assets
