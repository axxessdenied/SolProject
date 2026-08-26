#include "sol/assets/font.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <cstring>
#include <utility>

namespace sol::assets {

namespace {

constexpr char32_t kReplacement = 0xFFFD;
constexpr char32_t kFallbackCodepoint = U'?';

bool isContinuation(unsigned char byte)
{
    return (byte & 0xC0u) == 0x80u;
}

} // namespace

char32_t nextCodepoint(std::string_view text, std::size_t& cursor)
{
    if (cursor >= text.size()) {
        return 0;
    }

    const auto byteAt = [&](std::size_t index) { return static_cast<unsigned char>(text[index]); };

    const unsigned char lead = byteAt(cursor);
    if (lead < 0x80u) {
        ++cursor;
        return lead;
    }

    std::size_t length = 0;
    char32_t codepoint = 0;
    if ((lead & 0xE0u) == 0xC0u) {
        length = 2;
        codepoint = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
        length = 3;
        codepoint = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
        length = 4;
        codepoint = lead & 0x07u;
    } else {
        ++cursor; // stray continuation or invalid lead
        return kReplacement;
    }

    if (cursor + length > text.size()) {
        ++cursor;
        return kReplacement;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if (!isContinuation(byteAt(cursor + i))) {
            ++cursor;
            return kReplacement;
        }
        codepoint = (codepoint << 6) | (byteAt(cursor + i) & 0x3Fu);
    }

    cursor += length;
    return codepoint > 0x10FFFF ? kReplacement : codepoint;
}

bool Font::load(const char* path)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path, bytes)) {
        SOL_LOG_ERROR("Failed to read font: %s", path);
        return false;
    }
    if (!loadFromMemory(bytes)) {
        SOL_LOG_ERROR("Bad font asset: %s", path);
        return false;
    }
    return true;
}

bool Font::loadFromMemory(std::span<const std::uint8_t> bytes)
{
    m_valid = false;
    m_styles.clear();
    m_glyphs.clear();
    m_atlas.clear();

    FontFileHeader header = {};
    if (bytes.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kFontMagic || header.version != kFormatVersion || header.styleCount == 0) {
        return false;
    }

    const std::size_t styleBytes = static_cast<std::size_t>(header.styleCount) * sizeof(FontStyleRecord);
    const std::size_t glyphBytes = static_cast<std::size_t>(header.glyphCount) * sizeof(FontGlyphRecord);
    const std::size_t atlasBytes = static_cast<std::size_t>(header.atlasWidth) * header.atlasHeight;
    if (bytes.size() != sizeof(header) + styleBytes + glyphBytes + atlasBytes) {
        return false;
    }

    // Validate into locals and commit only on success, so a rejected asset
    // leaves an empty font rather than a half-populated one.
    std::vector<FontStyleRecord> styles(header.styleCount);
    std::vector<FontGlyphRecord> glyphs(header.glyphCount);
    std::memcpy(styles.data(), bytes.data() + sizeof(header), styleBytes);
    if (glyphBytes != 0) {
        std::memcpy(glyphs.data(), bytes.data() + sizeof(header) + styleBytes, glyphBytes);
    }

    // Every style must name a real, in-bounds run, and every glyph rect must
    // sit inside the atlas: the drawing code indexes these without checking.
    for (const FontStyleRecord& style : styles) {
        if (style.name[kFontStyleNameCapacity - 1] != '\0') {
            return false;
        }
        if (static_cast<std::size_t>(style.firstGlyph) + style.glyphCount > glyphs.size()) {
            return false;
        }
    }
    for (const FontGlyphRecord& glyph : glyphs) {
        if (static_cast<std::uint32_t>(glyph.atlasX) + glyph.width > header.atlasWidth ||
            static_cast<std::uint32_t>(glyph.atlasY) + glyph.height > header.atlasHeight) {
            return false;
        }
    }

    m_styles = std::move(styles);
    m_glyphs = std::move(glyphs);
    m_atlasWidth = header.atlasWidth;
    m_atlasHeight = header.atlasHeight;
    m_atlas.assign(bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(header) + styleBytes + glyphBytes),
                   bytes.end());
    m_valid = true;
    return true;
}

const FontStyleRecord* Font::style(std::string_view name) const
{
    for (const FontStyleRecord& candidate : m_styles) {
        if (name == candidate.name) {
            return &candidate;
        }
    }
    return nullptr;
}

const FontGlyphRecord* Font::glyph(const FontStyleRecord& style, char32_t codepoint) const
{
    const auto search = [&](char32_t wanted) -> const FontGlyphRecord* {
        std::uint32_t low = style.firstGlyph;
        std::uint32_t high = style.firstGlyph + style.glyphCount;
        while (low < high) {
            const std::uint32_t middle = low + (high - low) / 2;
            const std::uint32_t found = m_glyphs[middle].codepoint;
            if (found < wanted) {
                low = middle + 1;
            } else if (found > wanted) {
                high = middle;
            } else {
                return &m_glyphs[middle];
            }
        }
        return nullptr;
    };

    if (const FontGlyphRecord* found = search(codepoint); found != nullptr) {
        return found;
    }
    return codepoint == kFallbackCodepoint ? nullptr : search(kFallbackCodepoint);
}

float Font::measureWidth(const FontStyleRecord& style, std::string_view text) const
{
    float width = 0.0f;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const char32_t codepoint = nextCodepoint(text, cursor);
        if (const FontGlyphRecord* record = glyph(style, codepoint); record != nullptr) {
            width += record->advance;
        }
    }
    return width;
}

} // namespace sol::assets
