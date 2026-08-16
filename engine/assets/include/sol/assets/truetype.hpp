#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sol::assets {

// One outline point in font design units. TrueType outlines are quadratic
// B-splines: off-curve points are control points, and two consecutive
// off-curve points imply an on-curve point at their midpoint.
struct OutlinePoint
{
    float x = 0.0f;
    float y = 0.0f; // font units, y up
    bool onCurve = false;
};

struct GlyphOutline
{
    std::vector<OutlinePoint> points;
    std::vector<std::uint32_t> contourEnds; // one past the last point of each contour
    std::int32_t xMin = 0;
    std::int32_t yMin = 0;
    std::int32_t xMax = 0;
    std::int32_t yMax = 0;
};

// Horizontal metrics in font design units.
struct GlyphMetrics
{
    std::int32_t advanceWidth = 0;
    std::int32_t leftSideBearing = 0;
};

// Vertical metrics in pixels, for one scale.
struct FontMetrics
{
    float ascent = 0.0f;  // above the baseline, positive
    float descent = 0.0f; // below the baseline, negative
    float lineGap = 0.0f;
    float lineHeight = 0.0f; // ascent - descent + lineGap
};

// A rasterized glyph: 8-bit coverage, row-major, no padding, placed relative
// to the pen position on the baseline in atlas space (y grows downward).
struct GlyphBitmap
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t bearingX = 0; // left edge relative to the pen
    std::int32_t bearingY = 0; // top edge relative to the baseline; negative is above
    float advance = 0.0f;      // pen movement, pixels
    std::vector<std::uint8_t> coverage;
};

// Reader and rasterizer for TrueType fonts with `glyf`/`loca` outlines
// (engine plan §2.9 / Phase 8d — the game UI's text comes from here, baked to
// atlases by the cooker). CFF/PostScript outlines and font collections are
// deliberately unsupported: this covers the subset the UI needs, nothing more.
//
// Parsing is non-owning — the byte range handed to `parse` must outlive the
// font. Every accessor is pure and safe on an unparsed font, so a rejected
// file degrades to an empty font rather than undefined behavior.
class TrueTypeFont
{
public:
    [[nodiscard]] bool parse(std::span<const std::uint8_t> data);

    [[nodiscard]] bool valid() const { return m_valid; }
    [[nodiscard]] std::uint16_t glyphCount() const { return m_glyphCount; }
    [[nodiscard]] std::uint16_t unitsPerEm() const { return m_unitsPerEm; }

    // Glyph index for a Unicode codepoint; 0 (.notdef) when unmapped.
    [[nodiscard]] std::uint16_t glyphForCodepoint(char32_t codepoint) const;

    // Pixels per font unit, for an em size given in pixels.
    [[nodiscard]] float scaleForPixelSize(float pixelSize) const;

    [[nodiscard]] FontMetrics metricsForScale(float scale) const;
    [[nodiscard]] GlyphMetrics glyphMetrics(std::uint16_t glyph) const;

    // Kerning adjustment in font units for a pair, from a format-0 `kern`
    // table; 0 when the font has none.
    [[nodiscard]] std::int32_t kerning(std::uint16_t left, std::uint16_t right) const;

    // Raw outline in font units, composites resolved. False on a malformed
    // glyph; an empty glyph (space) succeeds with no contours.
    [[nodiscard]] bool glyphOutline(std::uint16_t glyph, GlyphOutline& out) const;

    // Rasterizes to 8-bit coverage with analytic area antialiasing. False on
    // a malformed glyph; an empty glyph succeeds with a zero-sized bitmap.
    [[nodiscard]] bool rasterizeGlyph(std::uint16_t glyph, float scale, GlyphBitmap& out) const;

private:
    struct Table
    {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };

    [[nodiscard]] Table findTable(const char tag[4]) const;
    [[nodiscard]] bool glyphRange(std::uint16_t glyph, std::uint32_t& begin, std::uint32_t& end) const;
    [[nodiscard]] bool appendGlyph(std::uint16_t glyph, int depth, float offsetX, float offsetY,
                                   float xx, float xy, float yx, float yy, GlyphOutline& out) const;
    [[nodiscard]] bool selectCmapSubtable();

    std::span<const std::uint8_t> m_data;
    bool m_valid = false;

    std::size_t m_tableDirectory = 0; // first table record
    std::uint32_t m_tableCount = 0;

    std::uint16_t m_glyphCount = 0;
    std::uint16_t m_unitsPerEm = 1000;
    std::int16_t m_ascent = 0;
    std::int16_t m_descent = 0;
    std::int16_t m_lineGap = 0;
    std::uint16_t m_hMetricCount = 0;
    bool m_longLoca = false;

    Table m_glyf;
    Table m_loca;
    Table m_hmtx;
    Table m_kern;
    std::uint32_t m_cmapOffset = 0; // absolute offset of the chosen subtable
    std::uint16_t m_cmapFormat = 0;
};

} // namespace sol::assets
