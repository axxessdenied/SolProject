#include <sol/assets/truetype.hpp>

#include <sol/test/synthetic_font.hpp>
#include <sol/test/test.hpp>

#include <cstdint>
#include <vector>

using sol::assets::FontMetrics;
using sol::assets::GlyphBitmap;
using sol::assets::GlyphOutline;
using sol::assets::TrueTypeFont;

namespace {

// The font under test is assembled byte by byte by the shared builder in
// sol/test/synthetic_font.hpp - no asset to license, and a failure indicts the
// reader rather than a mystery file.
constexpr unsigned kGlyphCount = sol::test::kSyntheticGlyphCount;

std::vector<std::uint8_t> buildTestFont()
{
    return sol::test::buildSyntheticFont();
}

bool nearlyEqual(float a, float b)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < 1.0e-3f;
}

std::uint8_t pixelAt(const GlyphBitmap& bitmap, std::uint32_t x, std::uint32_t y)
{
    if (x >= bitmap.width || y >= bitmap.height) {
        return 0;
    }
    return bitmap.coverage[static_cast<std::size_t>(y) * bitmap.width + x];
}

} // namespace

SOL_TEST(truetype_parses_header_and_metrics)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));
    SOL_CHECK(font.valid());
    SOL_CHECK(font.unitsPerEm() == 1000);
    SOL_CHECK(font.glyphCount() == kGlyphCount);

    const float scale = font.scaleForPixelSize(100.0f);
    SOL_CHECK(nearlyEqual(scale, 0.1f));

    const FontMetrics metrics = font.metricsForScale(scale);
    SOL_CHECK(nearlyEqual(metrics.ascent, 80.0f));
    SOL_CHECK(nearlyEqual(metrics.descent, -20.0f));
    SOL_CHECK(nearlyEqual(metrics.lineGap, 10.0f));
    SOL_CHECK(nearlyEqual(metrics.lineHeight, 110.0f));
}

SOL_TEST(truetype_rejects_malformed_input)
{
    TrueTypeFont font;
    const std::vector<std::uint8_t> garbage(64, 0xAB);
    SOL_CHECK(!font.parse(garbage));
    SOL_CHECK(!font.valid());

    // A rejected font still answers safely rather than reading past the end.
    SOL_CHECK(font.glyphForCodepoint(U'A') == 0);
    GlyphBitmap bitmap;
    SOL_CHECK(!font.rasterizeGlyph(2, 0.1f, bitmap));

    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    for (const std::size_t truncated : {std::size_t{4}, std::size_t{20}, fontBytes.size() / 2}) {
        TrueTypeFont partial;
        const std::vector<std::uint8_t> head(fontBytes.begin(),
                                             fontBytes.begin() + static_cast<std::ptrdiff_t>(truncated));
        SOL_CHECK(!partial.parse(head));
    }
}

SOL_TEST(truetype_maps_codepoints_through_cmap)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    SOL_CHECK(font.glyphForCodepoint(U'A') == 2);
    SOL_CHECK(font.glyphForCodepoint(U'B') == 3);
    SOL_CHECK(font.glyphForCodepoint(U'C') == 4);
    SOL_CHECK(font.glyphForCodepoint(U'D') == 5);

    // Unmapped codepoints fall back to .notdef instead of a bogus glyph.
    SOL_CHECK(font.glyphForCodepoint(U' ') == 0);
    SOL_CHECK(font.glyphForCodepoint(U'Z') == 0);
    SOL_CHECK(font.glyphForCodepoint(0x4E2D) == 0);
    SOL_CHECK(font.glyphForCodepoint(0x1F600) == 0);
}

SOL_TEST(truetype_reads_horizontal_metrics)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    SOL_CHECK(font.glyphMetrics(2).advanceWidth == 700);
    SOL_CHECK(font.glyphMetrics(2).leftSideBearing == 100);
    SOL_CHECK(font.glyphMetrics(3).advanceWidth == 1100);

    // Past numberOfHMetrics the last advance repeats and the bearing comes
    // from the trailing array.
    SOL_CHECK(font.glyphMetrics(4).advanceWidth == 1100);
    SOL_CHECK(font.glyphMetrics(4).leftSideBearing == 200);
    SOL_CHECK(font.glyphMetrics(5).advanceWidth == 1100);
    SOL_CHECK(font.glyphMetrics(5).leftSideBearing == 200);

    // Out of range is clamped to zero, not indexed.
    SOL_CHECK(font.glyphMetrics(99).advanceWidth == 0);
}

SOL_TEST(truetype_extracts_simple_outline)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    GlyphOutline outline;
    SOL_REQUIRE(font.glyphOutline(2, outline));
    SOL_REQUIRE(outline.contourEnds.size() == 1);
    SOL_REQUIRE(outline.points.size() == 4);
    SOL_CHECK(outline.contourEnds[0] == 4);

    SOL_CHECK(nearlyEqual(outline.points[0].x, 100.0f));
    SOL_CHECK(nearlyEqual(outline.points[0].y, 0.0f));
    SOL_CHECK(nearlyEqual(outline.points[1].x, 100.0f));
    SOL_CHECK(nearlyEqual(outline.points[1].y, 700.0f));
    SOL_CHECK(nearlyEqual(outline.points[2].x, 600.0f));
    SOL_CHECK(nearlyEqual(outline.points[2].y, 700.0f));
    SOL_CHECK(nearlyEqual(outline.points[3].x, 600.0f));
    SOL_CHECK(nearlyEqual(outline.points[3].y, 0.0f));
    for (const sol::assets::OutlinePoint& point : outline.points) {
        SOL_CHECK(point.onCurve);
    }

    SOL_CHECK(outline.xMin == 100);
    SOL_CHECK(outline.yMax == 700);

    // An empty glyph is not an error - space has metrics and no contours.
    GlyphOutline empty;
    SOL_CHECK(font.glyphOutline(1, empty));
    SOL_CHECK(empty.points.empty());
    SOL_CHECK(empty.contourEnds.empty());
}

SOL_TEST(truetype_resolves_composite_glyph)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    GlyphOutline base;
    GlyphOutline composite;
    SOL_REQUIRE(font.glyphOutline(2, base));
    SOL_REQUIRE(font.glyphOutline(4, composite));
    SOL_REQUIRE(base.points.size() == composite.points.size());
    SOL_CHECK(composite.contourEnds.size() == base.contourEnds.size());

    // 'C' is 'A' displaced by (200, 100), point for point.
    for (std::size_t i = 0; i < base.points.size(); ++i) {
        SOL_CHECK(nearlyEqual(composite.points[i].x, base.points[i].x + 200.0f));
        SOL_CHECK(nearlyEqual(composite.points[i].y, base.points[i].y + 100.0f));
    }
}

SOL_TEST(truetype_rasterizes_solid_shape)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    const float scale = font.scaleForPixelSize(100.0f);
    GlyphBitmap bitmap;
    SOL_REQUIRE(font.rasterizeGlyph(2, scale, bitmap));

    // The square spans x 100..600, y 0..700 in font units.
    SOL_CHECK(bitmap.width == 50);
    SOL_CHECK(bitmap.height == 70);
    SOL_CHECK(bitmap.bearingX == 10);
    SOL_CHECK(bitmap.bearingY == -70); // top edge sits above the baseline
    SOL_CHECK(nearlyEqual(bitmap.advance, 70.0f));
    SOL_REQUIRE(bitmap.coverage.size() == 50u * 70u);

    // A pixel-aligned rectangle should come out fully opaque everywhere.
    for (std::uint32_t y = 0; y < bitmap.height; ++y) {
        for (std::uint32_t x = 0; x < bitmap.width; ++x) {
            SOL_REQUIRE(pixelAt(bitmap, x, y) == 255);
        }
    }
}

SOL_TEST(truetype_rasterizes_hole_from_opposite_winding)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    const float scale = font.scaleForPixelSize(100.0f);
    GlyphBitmap bitmap;
    SOL_REQUIRE(font.rasterizeGlyph(3, scale, bitmap));
    SOL_REQUIRE(bitmap.width == 100);
    SOL_REQUIRE(bitmap.height == 100);

    // Ring is filled, the counter-wound inner contour is not.
    SOL_CHECK(pixelAt(bitmap, 10, 50) == 255);
    SOL_CHECK(pixelAt(bitmap, 90, 50) == 255);
    SOL_CHECK(pixelAt(bitmap, 50, 10) == 255);
    SOL_CHECK(pixelAt(bitmap, 50, 90) == 255);
    SOL_CHECK(pixelAt(bitmap, 50, 50) == 0);
    SOL_CHECK(pixelAt(bitmap, 30, 30) == 0);
    SOL_CHECK(pixelAt(bitmap, 70, 70) == 0);
}

SOL_TEST(truetype_antialiases_curves)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    const float scale = font.scaleForPixelSize(100.0f);
    GlyphBitmap bitmap;
    SOL_REQUIRE(font.rasterizeGlyph(5, scale, bitmap));
    SOL_REQUIRE(bitmap.width > 0 && bitmap.height > 0);

    bool sawPartial = false;
    bool sawOpaque = false;
    for (const std::uint8_t coverage : bitmap.coverage) {
        if (coverage > 0 && coverage < 255) {
            sawPartial = true;
        }
        if (coverage == 255) {
            sawOpaque = true;
        }
    }
    // Curved edges must produce intermediate coverage, not a stair-step.
    SOL_CHECK(sawPartial);
    SOL_CHECK(sawOpaque);

    // The middle of the rounded shape is solid; the clipped corners are not.
    SOL_CHECK(pixelAt(bitmap, bitmap.width / 2, bitmap.height / 2) == 255);
    SOL_CHECK(pixelAt(bitmap, 0, 0) < 128);
}

SOL_TEST(truetype_rasterization_is_deterministic)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    // Cooked atlases have to be byte-reproducible, so the rasterizer may not
    // depend on anything but its arguments.
    for (const std::uint16_t glyph : {std::uint16_t{2}, std::uint16_t{3}, std::uint16_t{5}}) {
        for (const float pixelSize : {11.0f, 24.0f, 40.0f}) {
            const float scale = font.scaleForPixelSize(pixelSize);
            GlyphBitmap first;
            GlyphBitmap second;
            SOL_REQUIRE(font.rasterizeGlyph(glyph, scale, first));
            SOL_REQUIRE(font.rasterizeGlyph(glyph, scale, second));
            SOL_CHECK(first.width == second.width);
            SOL_CHECK(first.height == second.height);
            SOL_CHECK(first.bearingX == second.bearingX);
            SOL_CHECK(first.bearingY == second.bearingY);
            SOL_CHECK(first.coverage == second.coverage);
        }
    }

    // A second parse of the same bytes yields the same raster too.
    TrueTypeFont reparsed;
    SOL_REQUIRE(reparsed.parse(fontBytes));
    GlyphBitmap original;
    GlyphBitmap again;
    SOL_REQUIRE(font.rasterizeGlyph(5, font.scaleForPixelSize(17.0f), original));
    SOL_REQUIRE(reparsed.rasterizeGlyph(5, reparsed.scaleForPixelSize(17.0f), again));
    SOL_CHECK(original.coverage == again.coverage);
}

SOL_TEST(truetype_rasterizes_empty_and_small_glyphs)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));

    // Whitespace: metrics, no pixels, and not an error.
    GlyphBitmap space;
    SOL_REQUIRE(font.rasterizeGlyph(1, font.scaleForPixelSize(24.0f), space));
    SOL_CHECK(space.width == 0);
    SOL_CHECK(space.height == 0);
    SOL_CHECK(space.coverage.empty());
    SOL_CHECK(nearlyEqual(space.advance, 300.0f * font.scaleForPixelSize(24.0f)));

    // Very small sizes must stay in bounds and stay sane.
    for (const float pixelSize : {1.0f, 2.0f, 3.0f, 6.0f}) {
        GlyphBitmap tiny;
        SOL_REQUIRE(font.rasterizeGlyph(3, font.scaleForPixelSize(pixelSize), tiny));
        SOL_CHECK(tiny.coverage.size() == static_cast<std::size_t>(tiny.width) * tiny.height);
    }

    GlyphBitmap bitmap;
    SOL_CHECK(!font.rasterizeGlyph(2, 0.0f, bitmap));
    SOL_CHECK(!font.rasterizeGlyph(99, 0.1f, bitmap));
}

SOL_TEST(truetype_reports_no_kerning_without_a_kern_table)
{
    const std::vector<std::uint8_t> fontBytes = buildTestFont();
    TrueTypeFont font;
    SOL_REQUIRE(font.parse(fontBytes));
    SOL_CHECK(font.kerning(2, 3) == 0);
}
