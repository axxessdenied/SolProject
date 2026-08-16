#include <sol/assets/truetype.hpp>

#include <sol/test/test.hpp>

#include <array>
#include <cstdint>
#include <vector>

using sol::assets::FontMetrics;
using sol::assets::GlyphBitmap;
using sol::assets::GlyphMetrics;
using sol::assets::GlyphOutline;
using sol::assets::TrueTypeFont;

namespace {

// The tests build their own font rather than loading one from disk: no asset
// to license or ship, and every byte the parser sees is authored right here,
// so a failure points at the reader instead of at a mystery file.
struct Bytes
{
    std::vector<std::uint8_t> data;

    void u8(unsigned value) { data.push_back(static_cast<std::uint8_t>(value & 0xFFu)); }
    void u16(unsigned value)
    {
        u8(value >> 8);
        u8(value);
    }
    void s16(int value) { u16(static_cast<unsigned>(static_cast<std::uint16_t>(value))); }
    void u32(std::uint32_t value)
    {
        u16(value >> 16);
        u16(value & 0xFFFFu);
    }
    void tag(const char* text)
    {
        for (int i = 0; i < 4; ++i) {
            u8(static_cast<unsigned char>(text[i]));
        }
    }
    void append(const Bytes& other) { data.insert(data.end(), other.data.begin(), other.data.end()); }
    void pad(std::size_t alignment)
    {
        while (data.size() % alignment != 0) {
            u8(0);
        }
    }
};

struct Point
{
    int x = 0;
    int y = 0;
    bool onCurve = true;
};

using Contour = std::vector<Point>;

Bytes simpleGlyph(const std::vector<Contour>& contours)
{
    int xMin = 32767;
    int yMin = 32767;
    int xMax = -32768;
    int yMax = -32768;
    for (const Contour& contour : contours) {
        for (const Point& point : contour) {
            xMin = point.x < xMin ? point.x : xMin;
            yMin = point.y < yMin ? point.y : yMin;
            xMax = point.x > xMax ? point.x : xMax;
            yMax = point.y > yMax ? point.y : yMax;
        }
    }

    Bytes glyph;
    glyph.s16(static_cast<int>(contours.size()));
    glyph.s16(xMin);
    glyph.s16(yMin);
    glyph.s16(xMax);
    glyph.s16(yMax);

    int endPoint = -1;
    for (const Contour& contour : contours) {
        endPoint += static_cast<int>(contour.size());
        glyph.u16(static_cast<unsigned>(endPoint));
    }
    glyph.u16(0); // instructionLength

    // Flags carry only ON_CURVE, so every coordinate is a plain int16 delta.
    for (const Contour& contour : contours) {
        for (const Point& point : contour) {
            glyph.u8(point.onCurve ? 0x01u : 0x00u);
        }
    }
    int previous = 0;
    for (const Contour& contour : contours) {
        for (const Point& point : contour) {
            glyph.s16(point.x - previous);
            previous = point.x;
        }
    }
    previous = 0;
    for (const Contour& contour : contours) {
        for (const Point& point : contour) {
            glyph.s16(point.y - previous);
            previous = point.y;
        }
    }
    glyph.pad(2);
    return glyph;
}

Bytes compositeGlyph(unsigned componentGlyph, int dx, int dy)
{
    Bytes glyph;
    glyph.s16(-1);
    glyph.s16(0);
    glyph.s16(0);
    glyph.s16(1000);
    glyph.s16(1000);
    glyph.u16(0x0003u); // ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES
    glyph.u16(componentGlyph);
    glyph.s16(dx);
    glyph.s16(dy);
    glyph.pad(2);
    return glyph;
}

// 6 glyphs: .notdef, space, a square 'A', a square-with-hole 'B', a composite
// 'C' repeating 'A' shifted, and a curved 'D' built from off-curve points.
constexpr unsigned kGlyphCount = 6;
constexpr unsigned kHMetricCount = 4; // glyphs 4-5 fall through to the trailing lsb array

std::vector<std::uint8_t> buildTestFont()
{
    std::vector<Bytes> glyphs;
    glyphs.emplace_back();                                                       // 0: .notdef, empty
    glyphs.emplace_back();                                                       // 1: space, empty
    glyphs.push_back(simpleGlyph({{{100, 0}, {100, 700}, {600, 700}, {600, 0}}})); // 2: 'A'
    glyphs.push_back(simpleGlyph({
        {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}},        // outer, clockwise
        {{250, 250}, {750, 250}, {750, 750}, {250, 750}},    // hole, counter-clockwise
    }));                                                                          // 3: 'B'
    glyphs.push_back(compositeGlyph(2, 200, 100));                                // 4: 'C'
    glyphs.push_back(simpleGlyph({{
        {200, 500},
        {200, 800, false},
        {500, 800},
        {800, 800, false},
        {800, 500},
        {800, 200, false},
        {500, 200},
        {200, 200, false},
    }}));                                                                         // 5: 'D'

    Bytes glyf;
    std::vector<std::uint32_t> glyphOffsets;
    for (const Bytes& glyph : glyphs) {
        glyphOffsets.push_back(static_cast<std::uint32_t>(glyf.data.size()));
        glyf.append(glyph);
        glyf.pad(2);
    }
    glyphOffsets.push_back(static_cast<std::uint32_t>(glyf.data.size()));

    Bytes loca; // short format: offsets in words
    for (const std::uint32_t offset : glyphOffsets) {
        loca.u16(offset / 2);
    }

    Bytes head;
    head.u32(0x00010000u); // version
    head.u32(0x00010000u); // fontRevision
    head.u32(0);           // checkSumAdjustment
    head.u32(0x5F0F3CF5u); // magicNumber
    head.u16(0);           // flags
    head.u16(1000);        // unitsPerEm
    head.u32(0);           // created
    head.u32(0);
    head.u32(0); // modified
    head.u32(0);
    head.s16(0);    // xMin
    head.s16(0);    // yMin
    head.s16(1000); // xMax
    head.s16(1000); // yMax
    head.u16(0);    // macStyle
    head.u16(8);    // lowestRecPPEM
    head.s16(2);    // fontDirectionHint
    head.s16(0);    // indexToLocFormat: short
    head.s16(0);    // glyphDataFormat

    Bytes maxp;
    maxp.u32(0x00010000u);
    maxp.u16(kGlyphCount);
    for (int i = 0; i < 13; ++i) {
        maxp.u16(0); // maxPoints .. maxComponentDepth
    }

    Bytes hhea;
    hhea.u32(0x00010000u);
    hhea.s16(800);  // ascender
    hhea.s16(-200); // descender
    hhea.s16(100);  // lineGap
    hhea.u16(1100); // advanceWidthMax
    hhea.s16(0);    // minLeftSideBearing
    hhea.s16(0);    // minRightSideBearing
    hhea.s16(1000); // xMaxExtent
    hhea.s16(1);    // caretSlopeRise
    hhea.s16(0);    // caretSlopeRun
    hhea.s16(0);    // caretOffset
    for (int i = 0; i < 4; ++i) {
        hhea.s16(0); // reserved
    }
    hhea.s16(0); // metricDataFormat
    hhea.u16(kHMetricCount);

    Bytes hmtx;
    const std::array<int, kHMetricCount> advances = {500, 300, 700, 1100};
    const std::array<int, kHMetricCount> bearings = {0, 0, 100, 0};
    for (unsigned i = 0; i < kHMetricCount; ++i) {
        hmtx.u16(static_cast<unsigned>(advances[i]));
        hmtx.s16(bearings[i]);
    }
    hmtx.s16(200); // glyph 4 left side bearing
    hmtx.s16(200); // glyph 5 left side bearing

    Bytes cmapSubtable; // format 4, one real segment plus the required terminator
    cmapSubtable.u16(4);
    cmapSubtable.u16(32); // length
    cmapSubtable.u16(0);  // language
    cmapSubtable.u16(4);  // segCountX2
    cmapSubtable.u16(4);  // searchRange
    cmapSubtable.u16(1);  // entrySelector
    cmapSubtable.u16(0);  // rangeShift
    cmapSubtable.u16(0x44);
    cmapSubtable.u16(0xFFFF); // endCode
    cmapSubtable.u16(0);      // reservedPad
    cmapSubtable.u16(0x41);
    cmapSubtable.u16(0xFFFF); // startCode
    cmapSubtable.s16(2 - 0x41);
    cmapSubtable.s16(1); // idDelta
    cmapSubtable.u16(0);
    cmapSubtable.u16(0); // idRangeOffset

    Bytes cmap;
    cmap.u16(0); // version
    cmap.u16(1); // numTables
    cmap.u16(3); // platformID: Windows
    cmap.u16(1); // encodingID: BMP
    cmap.u32(12);
    cmap.append(cmapSubtable);

    struct Entry
    {
        const char* tag;
        const Bytes* bytes;
    };
    // Table records must be sorted by tag.
    const std::array<Entry, 7> entries = {Entry{"cmap", &cmap}, Entry{"glyf", &glyf},
                                          Entry{"head", &head}, Entry{"hhea", &hhea},
                                          Entry{"hmtx", &hmtx}, Entry{"loca", &loca},
                                          Entry{"maxp", &maxp}};

    std::array<std::uint32_t, entries.size()> offsets = {};
    std::uint32_t cursor = static_cast<std::uint32_t>(12 + 16 * entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        cursor = (cursor + 3u) & ~3u;
        offsets[i] = cursor;
        cursor += static_cast<std::uint32_t>(entries[i].bytes->data.size());
    }

    Bytes font;
    font.u32(0x00010000u);
    font.u16(static_cast<unsigned>(entries.size()));
    font.u16(64); // searchRange
    font.u16(2);  // entrySelector
    font.u16(48); // rangeShift
    for (std::size_t i = 0; i < entries.size(); ++i) {
        font.tag(entries[i].tag);
        font.u32(0); // checkSum, unvalidated
        font.u32(offsets[i]);
        font.u32(static_cast<std::uint32_t>(entries[i].bytes->data.size()));
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        font.pad(4);
        font.append(*entries[i].bytes);
    }
    return font.data;
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
