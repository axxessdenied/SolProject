#pragma once

// A valid TrueType font assembled byte by byte, for tests that need one.
// Building the font here rather than shipping a file keeps a font-licensing
// question out of the test suite and makes a failure indict the code under
// test instead of a mystery asset.
//
// Six glyphs: 0 .notdef (empty), 1 space (empty), 2 'A' a plain square,
// 3 'B' a square with a counter-wound hole, 4 'C' a composite repeating 'A'
// shifted by (200, 100), and 5 'D' a rounded shape built from off-curve
// points. unitsPerEm is 1000; ascent 800, descent -200, lineGap 100.
// numberOfHMetrics is 4, so glyphs 4-5 exercise the trailing-bearing path.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sol::test {

inline constexpr unsigned kSyntheticGlyphCount = 6;
inline constexpr unsigned kSyntheticHMetricCount = 4;

namespace detail {

struct FontBytes
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
    void append(const FontBytes& other) { data.insert(data.end(), other.data.begin(), other.data.end()); }
    void pad(std::size_t alignment)
    {
        while (data.size() % alignment != 0) {
            u8(0);
        }
    }
};

struct FontPoint
{
    int x = 0;
    int y = 0;
    bool onCurve = true;
};

using FontContour = std::vector<FontPoint>;

inline FontBytes simpleGlyph(const std::vector<FontContour>& contours)
{
    int xMin = 32767;
    int yMin = 32767;
    int xMax = -32768;
    int yMax = -32768;
    for (const FontContour& contour : contours) {
        for (const FontPoint& point : contour) {
            xMin = point.x < xMin ? point.x : xMin;
            yMin = point.y < yMin ? point.y : yMin;
            xMax = point.x > xMax ? point.x : xMax;
            yMax = point.y > yMax ? point.y : yMax;
        }
    }

    FontBytes glyph;
    glyph.s16(static_cast<int>(contours.size()));
    glyph.s16(xMin);
    glyph.s16(yMin);
    glyph.s16(xMax);
    glyph.s16(yMax);

    int endPoint = -1;
    for (const FontContour& contour : contours) {
        endPoint += static_cast<int>(contour.size());
        glyph.u16(static_cast<unsigned>(endPoint));
    }
    glyph.u16(0); // instructionLength

    // Flags carry only ON_CURVE, so every coordinate is a plain int16 delta.
    for (const FontContour& contour : contours) {
        for (const FontPoint& point : contour) {
            glyph.u8(point.onCurve ? 0x01u : 0x00u);
        }
    }
    int previous = 0;
    for (const FontContour& contour : contours) {
        for (const FontPoint& point : contour) {
            glyph.s16(point.x - previous);
            previous = point.x;
        }
    }
    previous = 0;
    for (const FontContour& contour : contours) {
        for (const FontPoint& point : contour) {
            glyph.s16(point.y - previous);
            previous = point.y;
        }
    }
    glyph.pad(2);
    return glyph;
}

inline FontBytes compositeGlyph(unsigned componentGlyph, int dx, int dy)
{
    FontBytes glyph;
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

} // namespace detail

// 'A'..'D' map to glyphs 2..5; everything else falls through to .notdef.
inline std::vector<std::uint8_t> buildSyntheticFont()
{
    using detail::compositeGlyph;
    using detail::FontBytes;
    using detail::simpleGlyph;

    std::vector<FontBytes> glyphs;
    glyphs.emplace_back(); // 0: .notdef
    glyphs.emplace_back(); // 1: space
    glyphs.push_back(simpleGlyph({{{100, 0}, {100, 700}, {600, 700}, {600, 0}}}));
    glyphs.push_back(simpleGlyph({
        {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}},     // outer, clockwise
        {{250, 250}, {750, 250}, {750, 750}, {250, 750}}, // hole, counter-clockwise
    }));
    glyphs.push_back(compositeGlyph(2, 200, 100));
    glyphs.push_back(simpleGlyph({{
        {200, 500},
        {200, 800, false},
        {500, 800},
        {800, 800, false},
        {800, 500},
        {800, 200, false},
        {500, 200},
        {200, 200, false},
    }}));

    FontBytes glyf;
    std::vector<std::uint32_t> glyphOffsets;
    for (const FontBytes& glyph : glyphs) {
        glyphOffsets.push_back(static_cast<std::uint32_t>(glyf.data.size()));
        glyf.append(glyph);
        glyf.pad(2);
    }
    glyphOffsets.push_back(static_cast<std::uint32_t>(glyf.data.size()));

    FontBytes loca; // short format: offsets in words
    for (const std::uint32_t offset : glyphOffsets) {
        loca.u16(offset / 2);
    }

    FontBytes head;
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

    FontBytes maxp;
    maxp.u32(0x00010000u);
    maxp.u16(kSyntheticGlyphCount);
    for (int i = 0; i < 13; ++i) {
        maxp.u16(0); // maxPoints .. maxComponentDepth
    }

    FontBytes hhea;
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
    hhea.u16(kSyntheticHMetricCount);

    FontBytes hmtx;
    const std::array<int, kSyntheticHMetricCount> advances = {500, 300, 700, 1100};
    const std::array<int, kSyntheticHMetricCount> bearings = {0, 0, 100, 0};
    for (unsigned i = 0; i < kSyntheticHMetricCount; ++i) {
        hmtx.u16(static_cast<unsigned>(advances[i]));
        hmtx.s16(bearings[i]);
    }
    hmtx.s16(200); // glyph 4 left side bearing
    hmtx.s16(200); // glyph 5 left side bearing

    FontBytes cmapSubtable; // format 4: one real segment plus the terminator
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

    FontBytes cmap;
    cmap.u16(0); // version
    cmap.u16(1); // numTables
    cmap.u16(3); // platformID: Windows
    cmap.u16(1); // encodingID: BMP
    cmap.u32(12);
    cmap.append(cmapSubtable);

    struct Entry
    {
        const char* tag;
        const FontBytes* bytes;
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

    FontBytes font;
    font.u32(0x00010000u);
    font.u16(static_cast<unsigned>(entries.size()));
    font.u16(64); // searchRange
    font.u16(2);  // entrySelector
    font.u16(48); // rangeShift
    for (std::size_t i = 0; i < entries.size(); ++i) {
        font.tag(entries[i].tag);
        font.u32(0); // checkSum, unvalidated by the reader
        font.u32(offsets[i]);
        font.u32(static_cast<std::uint32_t>(entries[i].bytes->data.size()));
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        font.pad(4);
        font.append(*entries[i].bytes);
    }
    return font.data;
}

} // namespace sol::test
