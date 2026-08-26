#include "font.hpp"

#include "sol/assets/truetype.hpp"
#include "sol/core/toml.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace sol::cooker {

namespace {

using sol::core::TomlValue;

// Printable ASCII is always baked; the manifest only names what to add.
constexpr char32_t kAsciiFirst = 32;
constexpr char32_t kAsciiLast = 126;

constexpr std::uint32_t kDefaultAtlasWidth = 512;
constexpr std::uint32_t kDefaultPadding = 1;

void fail(std::string* outError, std::string message)
{
    if (outError != nullptr) {
        *outError = std::move(message);
    }
}

bool checkKeys(const TomlValue& table,
               std::initializer_list<const char*> allowed,
               const char* context,
               std::string* outError)
{
    for (const std::pair<std::string, TomlValue>& member : table.members()) {
        const bool known =
            std::any_of(allowed.begin(), allowed.end(), [&](const char* key) { return member.first == key; });
        if (!known) {
            fail(outError, std::string(context) + ": unknown key '" + member.first + "'");
            return false;
        }
    }
    return true;
}

std::uint32_t nextPowerOfTwo(std::uint32_t value)
{
    std::uint32_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

// One rasterized glyph waiting to be placed in the atlas.
struct PendingGlyph
{
    char32_t codepoint = 0;
    assets::GlyphBitmap bitmap;
};

} // namespace

bool bakeFont(const char* manifestText,
              std::size_t manifestLength,
              const std::string& sourceDirectory,
              BakedFont& out,
              std::string* outError)
{
    out = BakedFont();

    TomlValue root;
    std::string parseError;
    if (!TomlValue::parse(manifestText, manifestLength, root, &parseError)) {
        fail(outError, "font manifest: " + parseError);
        return false;
    }
    if (!checkKeys(root, {"atlas_width", "padding", "codepoints", "style"}, "font manifest", outError)) {
        return false;
    }

    std::uint32_t atlasWidth = kDefaultAtlasWidth;
    if (const TomlValue* value = root.find("atlas_width"); value != nullptr) {
        if (!value->isInteger() || value->asInteger() <= 0 || value->asInteger() > 8192) {
            fail(outError, "font manifest: atlas_width must be a positive integer up to 8192");
            return false;
        }
        atlasWidth = static_cast<std::uint32_t>(value->asInteger());
    }

    std::uint32_t padding = kDefaultPadding;
    if (const TomlValue* value = root.find("padding"); value != nullptr) {
        if (!value->isInteger() || value->asInteger() < 0 || value->asInteger() > 16) {
            fail(outError, "font manifest: padding must be an integer in 0..16");
            return false;
        }
        padding = static_cast<std::uint32_t>(value->asInteger());
    }

    std::vector<char32_t> codepoints;
    for (char32_t codepoint = kAsciiFirst; codepoint <= kAsciiLast; ++codepoint) {
        codepoints.push_back(codepoint);
    }
    if (const TomlValue* value = root.find("codepoints"); value != nullptr) {
        if (!value->isArray()) {
            fail(outError, "font manifest: codepoints must be an array of integers");
            return false;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            const TomlValue& entry = (*value)[i];
            if (!entry.isInteger() || entry.asInteger() < 0 || entry.asInteger() > 0x10FFFF) {
                fail(outError, "font manifest: codepoints entries must be Unicode scalar values");
                return false;
            }
            codepoints.push_back(static_cast<char32_t>(entry.asInteger()));
        }
        std::sort(codepoints.begin(), codepoints.end());
        codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    }

    const TomlValue* styles = root.find("style");
    if (styles == nullptr || !styles->isArray() || styles->size() == 0) {
        fail(outError, "font manifest: at least one [[style]] is required");
        return false;
    }

    // Rasterize everything first; packing needs all the sizes up front.
    std::map<std::string, std::vector<std::uint8_t>> sourceCache;
    std::vector<PendingGlyph> pending;
    pending.reserve(styles->size() * codepoints.size());

    for (std::size_t i = 0; i < styles->size(); ++i) {
        const TomlValue& style = (*styles)[i];
        if (!style.isTable() || !checkKeys(style, {"name", "source", "size"}, "font style", outError)) {
            if (!style.isTable()) {
                fail(outError, "font manifest: [[style]] entries must be tables");
            }
            return false;
        }

        const TomlValue* name = style.find("name");
        const TomlValue* source = style.find("source");
        const TomlValue* size = style.find("size");
        if (name == nullptr || !name->isString() || name->asString().empty()) {
            fail(outError, "font style: 'name' is required and must be a non-empty string");
            return false;
        }
        if (name->asString().size() >= assets::kFontStyleNameCapacity) {
            fail(outError, "font style '" + name->asString() + "': name is too long");
            return false;
        }
        if (source == nullptr || !source->isString() || source->asString().empty()) {
            fail(outError, "font style '" + name->asString() + "': 'source' is required");
            return false;
        }
        if (size == nullptr || (!size->isInteger() && !size->isFloat()) || size->asFloat() <= 0.0 ||
            size->asFloat() > 512.0) {
            fail(outError, "font style '" + name->asString() + "': 'size' must be in (0, 512]");
            return false;
        }

        for (const assets::FontStyleRecord& existing : out.styles) {
            if (name->asString() == existing.name) {
                fail(outError, "font style '" + name->asString() + "': duplicate name");
                return false;
            }
        }

        const std::string sourcePath =
            sourceDirectory.empty() ? source->asString() : sourceDirectory + "/" + source->asString();
        auto cached = sourceCache.find(sourcePath);
        if (cached == sourceCache.end()) {
            std::vector<std::uint8_t> bytes;
            if (!platform::readFileBytes(sourcePath.c_str(), bytes)) {
                fail(outError, "font style '" + name->asString() + "': cannot read " + sourcePath);
                return false;
            }
            cached = sourceCache.emplace(sourcePath, std::move(bytes)).first;
        }

        assets::TrueTypeFont font;
        if (!font.parse(cached->second)) {
            fail(outError,
                 "font style '" + name->asString() + "': " + sourcePath + " is not a TrueType outline font");
            return false;
        }

        const float pixelSize = static_cast<float>(size->asFloat());
        const float scale = font.scaleForPixelSize(pixelSize);
        const assets::FontMetrics metrics = font.metricsForScale(scale);

        assets::FontStyleRecord record = {};
        std::memcpy(record.name, name->asString().c_str(), name->asString().size());
        record.pixelSize = pixelSize;
        record.ascent = metrics.ascent;
        record.descent = metrics.descent;
        record.lineHeight = metrics.lineHeight;
        record.firstGlyph = static_cast<std::uint32_t>(pending.size());
        record.glyphCount = static_cast<std::uint32_t>(codepoints.size());

        for (const char32_t codepoint : codepoints) {
            // An unmapped codepoint resolves to glyph 0, so missing characters
            // bake as the font's .notdef box instead of vanishing.
            const std::uint16_t glyph = font.glyphForCodepoint(codepoint);
            PendingGlyph entry;
            entry.codepoint = codepoint;
            if (!font.rasterizeGlyph(glyph, scale, entry.bitmap)) {
                fail(outError,
                     "font style '" + name->asString() + "': failed to rasterize U+" +
                         std::to_string(static_cast<std::uint32_t>(codepoint)));
                return false;
            }
            pending.push_back(std::move(entry));
        }

        out.styles.push_back(record);
    }

    // Shelf-pack in bake order: styles as authored, codepoints ascending.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> placements(pending.size(), {0, 0});
    std::uint32_t penX = padding;
    std::uint32_t penY = padding;
    std::uint32_t shelfHeight = 0;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const assets::GlyphBitmap& bitmap = pending[i].bitmap;
        if (bitmap.width == 0 || bitmap.height == 0) {
            continue; // whitespace occupies no atlas space
        }
        if (bitmap.width + 2 * padding > atlasWidth) {
            fail(outError, "font manifest: atlas_width is too small for the largest glyph");
            return false;
        }
        if (penX + bitmap.width + padding > atlasWidth) {
            penX = padding;
            penY += shelfHeight + padding;
            shelfHeight = 0;
        }
        placements[i] = {penX, penY};
        penX += bitmap.width + padding;
        shelfHeight = std::max(shelfHeight, bitmap.height);
    }

    out.atlasWidth = atlasWidth;
    out.atlasHeight = nextPowerOfTwo(penY + shelfHeight + padding);
    out.atlas.assign(static_cast<std::size_t>(out.atlasWidth) * out.atlasHeight, 0);

    out.glyphs.reserve(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const PendingGlyph& entry = pending[i];
        const assets::GlyphBitmap& bitmap = entry.bitmap;

        assets::FontGlyphRecord record = {};
        record.codepoint = static_cast<std::uint32_t>(entry.codepoint);
        record.atlasX = static_cast<std::uint16_t>(placements[i].first);
        record.atlasY = static_cast<std::uint16_t>(placements[i].second);
        record.width = static_cast<std::uint16_t>(bitmap.width);
        record.height = static_cast<std::uint16_t>(bitmap.height);
        record.bearingX = static_cast<std::int16_t>(bitmap.bearingX);
        record.bearingY = static_cast<std::int16_t>(bitmap.bearingY);
        record.advance = bitmap.advance;
        out.glyphs.push_back(record);

        for (std::uint32_t y = 0; y < bitmap.height; ++y) {
            const std::size_t destination =
                static_cast<std::size_t>(placements[i].second + y) * out.atlasWidth + placements[i].first;
            std::memcpy(out.atlas.data() + destination,
                        bitmap.coverage.data() + static_cast<std::size_t>(y) * bitmap.width,
                        bitmap.width);
        }
    }

    return true;
}

std::vector<std::uint8_t> encodeFont(const BakedFont& font)
{
    assets::FontFileHeader header = {};
    header.styleCount = static_cast<std::uint32_t>(font.styles.size());
    header.glyphCount = static_cast<std::uint32_t>(font.glyphs.size());
    header.atlasWidth = font.atlasWidth;
    header.atlasHeight = font.atlasHeight;

    const std::size_t styleBytes = font.styles.size() * sizeof(assets::FontStyleRecord);
    const std::size_t glyphBytes = font.glyphs.size() * sizeof(assets::FontGlyphRecord);

    std::vector<std::uint8_t> bytes(sizeof(header) + styleBytes + glyphBytes + font.atlas.size());
    std::uint8_t* cursor = bytes.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    if (styleBytes != 0) {
        std::memcpy(cursor, font.styles.data(), styleBytes);
        cursor += styleBytes;
    }
    if (glyphBytes != 0) {
        std::memcpy(cursor, font.glyphs.data(), glyphBytes);
        cursor += glyphBytes;
    }
    if (!font.atlas.empty()) {
        std::memcpy(cursor, font.atlas.data(), font.atlas.size());
    }
    return bytes;
}

} // namespace sol::cooker
