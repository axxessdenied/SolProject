#include "sol/assets/truetype.hpp"

#include "sol/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace sol::assets {

namespace {

// Big-endian cursor over the font bytes. Reads past the end are not fatal:
// they clear `ok` and yield zero, so a truncated table degrades into a
// rejected parse instead of undefined behavior.
class ByteReader
{
public:
    ByteReader(std::span<const std::uint8_t> data, std::size_t offset) : m_data(data), m_at(offset)
    {
        m_ok = offset <= data.size();
    }

    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] std::size_t at() const { return m_at; }
    void seek(std::size_t offset) { m_at = offset; }
    void skip(std::size_t count) { m_at += count; }

    std::uint8_t u8()
    {
        if (!check(1)) {
            return 0;
        }
        return m_data[m_at++];
    }

    std::uint16_t u16()
    {
        if (!check(2)) {
            return 0;
        }
        const std::uint16_t value =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(m_data[m_at]) << 8) | m_data[m_at + 1]);
        m_at += 2;
        return value;
    }

    std::int16_t s16() { return static_cast<std::int16_t>(u16()); }

    std::uint32_t u32()
    {
        if (!check(4)) {
            return 0;
        }
        const std::uint32_t value = (static_cast<std::uint32_t>(m_data[m_at]) << 24) |
                                    (static_cast<std::uint32_t>(m_data[m_at + 1]) << 16) |
                                    (static_cast<std::uint32_t>(m_data[m_at + 2]) << 8) |
                                    static_cast<std::uint32_t>(m_data[m_at + 3]);
        m_at += 4;
        return value;
    }

    // F2Dot14 fixed point, as used by composite glyph transforms.
    float f2Dot14() { return static_cast<float>(s16()) / 16384.0f; }

private:
    bool check(std::size_t count)
    {
        if (!m_ok || m_at + count > m_data.size()) {
            m_ok = false;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> m_data;
    std::size_t m_at = 0;
    bool m_ok = true;
};

struct Vec2f
{
    float x = 0.0f;
    float y = 0.0f;
};

Vec2f midpoint(const Vec2f& a, const Vec2f& b)
{
    return {0.5f * (a.x + b.x), 0.5f * (a.y + b.y)};
}

// Simple glyph flags.
constexpr std::uint8_t kOnCurve = 0x01;
constexpr std::uint8_t kXShort = 0x02;
constexpr std::uint8_t kYShort = 0x04;
constexpr std::uint8_t kRepeat = 0x08;
constexpr std::uint8_t kXSamePositive = 0x10;
constexpr std::uint8_t kYSamePositive = 0x20;

// Composite glyph flags.
constexpr std::uint16_t kArgsAreWords = 0x0001;
constexpr std::uint16_t kArgsAreXy = 0x0002;
constexpr std::uint16_t kHaveScale = 0x0008;
constexpr std::uint16_t kMoreComponents = 0x0020;
constexpr std::uint16_t kHaveXYScale = 0x0040;
constexpr std::uint16_t kHaveTwoByTwo = 0x0080;

constexpr int kMaxCompositeDepth = 8;

// Flatness tolerance for curve subdivision, in pixels. A quadratic's maximum
// deviation from an n-segment polyline is |p0 - 2c + p1| / (4n^2).
constexpr float kFlattenTolerance = 0.08f;

// Signed-area coverage accumulator (the "cell" rasterizer): each line segment
// deposits, per scanline, the exact area it covers in each pixel plus the
// winding it carries forward. A prefix sum along the row then yields analytic
// coverage, with no supersampling anywhere.
class CoverageAccumulator
{
public:
    CoverageAccumulator(std::uint32_t width, std::uint32_t height)
        : m_stride(width + 2), m_width(width), m_height(height), m_cells(static_cast<std::size_t>(width + 2) *
                                                                          height, 0.0f)
    {
    }

    void line(float x0, float y0, float x1, float y1);

    // Prefix-sums each row into 8-bit coverage. Winding is folded with |.|
    // clamped to 1, which renders overlapping contours the way fonts expect.
    void resolve(std::vector<std::uint8_t>& out) const;

private:
    void add(std::size_t rowStart, int column, float value)
    {
        const int clamped = std::clamp(column, 0, static_cast<int>(m_stride) - 1);
        m_cells[rowStart + static_cast<std::size_t>(clamped)] += value;
    }

    std::uint32_t m_stride = 0;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::vector<float> m_cells;
};

void CoverageAccumulator::line(float x0, float y0, float x1, float y1)
{
    if (y0 == y1 || m_height == 0) {
        return; // horizontal edges contribute no winding
    }

    float direction = 1.0f;
    if (y0 > y1) {
        direction = -1.0f;
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

    const float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    int y = static_cast<int>(std::floor(y0));
    if (y < 0) {
        x -= y0 * dxdy; // advance to the y = 0 crossing
        y = 0;
    }
    const int yEnd = std::min(static_cast<int>(m_height), static_cast<int>(std::ceil(y1)));

    for (; y < yEnd; ++y) {
        const std::size_t rowStart = static_cast<std::size_t>(y) * m_stride;
        const float dy = std::min(static_cast<float>(y + 1), y1) - std::max(static_cast<float>(y), y0);
        const float xNext = x + dxdy * dy;
        const float d = dy * direction;

        float xLeft = x;
        float xRight = xNext;
        if (xLeft > xRight) {
            std::swap(xLeft, xRight);
        }

        const float leftFloor = std::floor(xLeft);
        const int leftIndex = static_cast<int>(leftFloor);
        const float rightCeil = std::ceil(xRight);
        const int rightIndex = static_cast<int>(rightCeil);

        if (rightIndex <= leftIndex + 1) {
            // The span sits inside a single pixel: split by the midpoint.
            const float mid = 0.5f * (x + xNext) - leftFloor;
            add(rowStart, leftIndex, d - d * mid);
            add(rowStart, leftIndex + 1, d * mid);
        } else {
            const float inverseSpan = 1.0f / (xRight - xLeft);
            const float leftFraction = xLeft - leftFloor;
            const float firstArea = 0.5f * inverseSpan * (1.0f - leftFraction) * (1.0f - leftFraction);
            const float rightFraction = xRight - rightCeil + 1.0f;
            const float lastArea = 0.5f * inverseSpan * rightFraction * rightFraction;

            add(rowStart, leftIndex, d * firstArea);
            if (rightIndex == leftIndex + 2) {
                add(rowStart, leftIndex + 1, d * (1.0f - firstArea - lastArea));
            } else {
                const float secondArea = inverseSpan * (1.5f - leftFraction);
                add(rowStart, leftIndex + 1, d * (secondArea - firstArea));
                for (int column = leftIndex + 2; column < rightIndex - 1; ++column) {
                    add(rowStart, column, d * inverseSpan);
                }
                const float beforeLast =
                    secondArea + static_cast<float>(rightIndex - leftIndex - 3) * inverseSpan;
                add(rowStart, rightIndex - 1, d * (1.0f - beforeLast - lastArea));
            }
            add(rowStart, rightIndex, d * lastArea);
        }

        x = xNext;
    }
}

void CoverageAccumulator::resolve(std::vector<std::uint8_t>& out) const
{
    out.assign(static_cast<std::size_t>(m_width) * m_height, 0);
    for (std::uint32_t y = 0; y < m_height; ++y) {
        const std::size_t rowStart = static_cast<std::size_t>(y) * m_stride;
        const std::size_t outStart = static_cast<std::size_t>(y) * m_width;
        float accumulated = 0.0f;
        for (std::uint32_t x = 0; x < m_width; ++x) {
            accumulated += m_cells[rowStart + x];
            const float coverage = std::min(std::fabs(accumulated), 1.0f);
            out[outStart + x] = static_cast<std::uint8_t>(coverage * 255.0f + 0.5f);
        }
    }
}

void flattenQuadratic(const Vec2f& p0, const Vec2f& control, const Vec2f& p1,
                      CoverageAccumulator& accumulator)
{
    const float deviationX = p0.x - 2.0f * control.x + p1.x;
    const float deviationY = p0.y - 2.0f * control.y + p1.y;
    const float deviation = std::sqrt(deviationX * deviationX + deviationY * deviationY);

    int steps = 1;
    if (deviation > 0.0f) {
        steps = static_cast<int>(std::ceil(std::sqrt(deviation / (4.0f * kFlattenTolerance))));
        steps = std::clamp(steps, 1, 64);
    }

    Vec2f previous = p0;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float inverse = 1.0f - t;
        const Vec2f point = {inverse * inverse * p0.x + 2.0f * inverse * t * control.x + t * t * p1.x,
                             inverse * inverse * p0.y + 2.0f * inverse * t * control.y + t * t * p1.y};
        accumulator.line(previous.x, previous.y, point.x, point.y);
        previous = point;
    }
}

} // namespace

bool TrueTypeFont::parse(std::span<const std::uint8_t> data)
{
    *this = TrueTypeFont();
    m_data = data;

    ByteReader reader(data, 0);
    const std::uint32_t sfntVersion = reader.u32();
    if (sfntVersion == 0x74746366u) { // 'ttcf'
        SOL_LOG_ERROR("truetype: font collections are not supported");
        return false;
    }
    if (sfntVersion != 0x00010000u && sfntVersion != 0x74727565u) { // 1.0 or 'true'
        SOL_LOG_ERROR("truetype: not a TrueType outline font (sfnt version %08x)", sfntVersion);
        return false;
    }

    const std::uint16_t tableCount = reader.u16();
    reader.skip(6); // searchRange, entrySelector, rangeShift
    if (!reader.ok() || tableCount == 0) {
        SOL_LOG_ERROR("truetype: bad table directory");
        return false;
    }
    m_tableDirectory = reader.at();
    m_tableCount = tableCount;

    const Table head = findTable("head");
    const Table maxp = findTable("maxp");
    const Table hhea = findTable("hhea");
    m_hmtx = findTable("hmtx");
    m_loca = findTable("loca");
    m_glyf = findTable("glyf");
    m_kern = findTable("kern");
    if (head.length == 0 || maxp.length == 0 || hhea.length == 0 || m_hmtx.length == 0 ||
        m_loca.length == 0 || m_glyf.length == 0) {
        SOL_LOG_ERROR("truetype: missing a required table (head/maxp/hhea/hmtx/loca/glyf)");
        return false;
    }

    ByteReader headReader(data, head.offset);
    headReader.skip(18);
    m_unitsPerEm = headReader.u16();
    headReader.skip(30); // created, modified, xMin..yMax, macStyle, lowestRecPPEM, fontDirectionHint
    const std::int16_t indexToLocFormat = headReader.s16();
    if (!headReader.ok() || m_unitsPerEm == 0) {
        SOL_LOG_ERROR("truetype: bad head table");
        return false;
    }
    m_longLoca = indexToLocFormat != 0;

    ByteReader maxpReader(data, maxp.offset + 4);
    m_glyphCount = maxpReader.u16();

    ByteReader hheaReader(data, hhea.offset + 4);
    m_ascent = hheaReader.s16();
    m_descent = hheaReader.s16();
    m_lineGap = hheaReader.s16();
    hheaReader.skip(24); // advanceWidthMax .. metricDataFormat
    m_hMetricCount = hheaReader.u16();
    if (!maxpReader.ok() || !hheaReader.ok() || m_glyphCount == 0 || m_hMetricCount == 0) {
        SOL_LOG_ERROR("truetype: bad maxp/hhea table");
        return false;
    }

    if (!selectCmapSubtable()) {
        SOL_LOG_ERROR("truetype: no usable cmap subtable (need format 4 or 12)");
        return false;
    }

    m_valid = true;
    return true;
}

TrueTypeFont::Table TrueTypeFont::findTable(const char tag[4]) const
{
    for (std::uint32_t i = 0; i < m_tableCount; ++i) {
        const std::size_t record = m_tableDirectory + static_cast<std::size_t>(i) * 16;
        if (record + 16 > m_data.size()) {
            break;
        }
        if (std::memcmp(m_data.data() + record, tag, 4) == 0) {
            ByteReader reader(m_data, record + 8);
            Table table;
            table.offset = reader.u32();
            table.length = reader.u32();
            if (!reader.ok() || table.offset > m_data.size() ||
                table.length > m_data.size() - table.offset) {
                return {};
            }
            return table;
        }
    }
    return {};
}

bool TrueTypeFont::selectCmapSubtable()
{
    const Table cmap = findTable("cmap");
    if (cmap.length == 0) {
        return false;
    }

    ByteReader reader(m_data, cmap.offset + 2);
    const std::uint16_t subtableCount = reader.u16();

    // Preference order: Windows full-repertoire, Windows BMP, then any
    // Unicode-platform subtable.
    std::uint32_t best = 0;
    int bestScore = -1;
    for (std::uint16_t i = 0; i < subtableCount; ++i) {
        const std::uint16_t platformId = reader.u16();
        const std::uint16_t encodingId = reader.u16();
        const std::uint32_t subtableOffset = reader.u32();
        if (!reader.ok()) {
            return false;
        }

        int score = -1;
        if (platformId == 3 && encodingId == 10) {
            score = 3;
        } else if (platformId == 3 && encodingId == 1) {
            score = 2;
        } else if (platformId == 0) {
            score = 1;
        }
        if (score > bestScore) {
            bestScore = score;
            best = cmap.offset + subtableOffset;
        }
    }

    if (bestScore < 0 || best >= m_data.size()) {
        return false;
    }

    ByteReader formatReader(m_data, best);
    const std::uint16_t format = formatReader.u16();
    if (format != 4 && format != 12) {
        return false;
    }
    m_cmapOffset = best;
    m_cmapFormat = format;
    return true;
}

std::uint16_t TrueTypeFont::glyphForCodepoint(char32_t codepoint) const
{
    if (!m_valid) {
        return 0;
    }

    if (m_cmapFormat == 4) {
        if (codepoint > 0xFFFF) {
            return 0;
        }
        const std::uint16_t code = static_cast<std::uint16_t>(codepoint);
        ByteReader reader(m_data, m_cmapOffset + 6);
        const std::uint16_t segCountX2 = reader.u16();
        const std::uint16_t segCount = static_cast<std::uint16_t>(segCountX2 / 2);
        if (!reader.ok() || segCount == 0) {
            return 0;
        }

        const std::size_t endCodes = m_cmapOffset + 14;
        const std::size_t startCodes = endCodes + segCountX2 + 2; // + reservedPad
        const std::size_t idDeltas = startCodes + segCountX2;
        const std::size_t idRangeOffsets = idDeltas + segCountX2;

        for (std::uint16_t segment = 0; segment < segCount; ++segment) {
            ByteReader endReader(m_data, endCodes + static_cast<std::size_t>(segment) * 2);
            const std::uint16_t endCode = endReader.u16();
            if (!endReader.ok() || code > endCode) {
                continue;
            }

            ByteReader startReader(m_data, startCodes + static_cast<std::size_t>(segment) * 2);
            const std::uint16_t startCode = startReader.u16();
            if (!startReader.ok() || code < startCode) {
                return 0; // segments are sorted; the code falls in a gap
            }

            ByteReader deltaReader(m_data, idDeltas + static_cast<std::size_t>(segment) * 2);
            const std::int16_t idDelta = deltaReader.s16();
            ByteReader rangeReader(m_data, idRangeOffsets + static_cast<std::size_t>(segment) * 2);
            const std::uint16_t idRangeOffset = rangeReader.u16();
            if (!deltaReader.ok() || !rangeReader.ok()) {
                return 0;
            }

            if (idRangeOffset == 0) {
                return static_cast<std::uint16_t>((code + idDelta) & 0xFFFF);
            }

            // The offset is measured from the idRangeOffset slot itself.
            const std::size_t slot = idRangeOffsets + static_cast<std::size_t>(segment) * 2;
            const std::size_t glyphAddress =
                slot + idRangeOffset + static_cast<std::size_t>(code - startCode) * 2;
            ByteReader glyphReader(m_data, glyphAddress);
            const std::uint16_t glyph = glyphReader.u16();
            if (!glyphReader.ok() || glyph == 0) {
                return 0;
            }
            return static_cast<std::uint16_t>((glyph + idDelta) & 0xFFFF);
        }
        return 0;
    }

    // Format 12: sorted groups of contiguous codepoint ranges.
    ByteReader reader(m_data, m_cmapOffset + 12);
    const std::uint32_t groupCount = reader.u32();
    if (!reader.ok()) {
        return 0;
    }
    std::uint32_t low = 0;
    std::uint32_t high = groupCount;
    while (low < high) {
        const std::uint32_t middle = low + (high - low) / 2;
        ByteReader groupReader(m_data, m_cmapOffset + 16 + static_cast<std::size_t>(middle) * 12);
        const std::uint32_t startCharCode = groupReader.u32();
        const std::uint32_t endCharCode = groupReader.u32();
        const std::uint32_t startGlyphId = groupReader.u32();
        if (!groupReader.ok()) {
            return 0;
        }
        if (codepoint < startCharCode) {
            high = middle;
        } else if (codepoint > endCharCode) {
            low = middle + 1;
        } else {
            return static_cast<std::uint16_t>(startGlyphId + (codepoint - startCharCode));
        }
    }
    return 0;
}

float TrueTypeFont::scaleForPixelSize(float pixelSize) const
{
    return m_unitsPerEm == 0 ? 0.0f : pixelSize / static_cast<float>(m_unitsPerEm);
}

FontMetrics TrueTypeFont::metricsForScale(float scale) const
{
    FontMetrics metrics;
    metrics.ascent = static_cast<float>(m_ascent) * scale;
    metrics.descent = static_cast<float>(m_descent) * scale;
    metrics.lineGap = static_cast<float>(m_lineGap) * scale;
    metrics.lineHeight = metrics.ascent - metrics.descent + metrics.lineGap;
    return metrics;
}

GlyphMetrics TrueTypeFont::glyphMetrics(std::uint16_t glyph) const
{
    GlyphMetrics metrics;
    if (!m_valid || glyph >= m_glyphCount) {
        return metrics;
    }

    // Glyphs past the last long metric reuse its advance and carry their own
    // left side bearing in the trailing array.
    if (glyph < m_hMetricCount) {
        ByteReader reader(m_data, m_hmtx.offset + static_cast<std::size_t>(glyph) * 4);
        metrics.advanceWidth = reader.u16();
        metrics.leftSideBearing = reader.s16();
    } else {
        ByteReader advanceReader(m_data, m_hmtx.offset + static_cast<std::size_t>(m_hMetricCount - 1) * 4);
        metrics.advanceWidth = advanceReader.u16();
        const std::size_t bearings = m_hmtx.offset + static_cast<std::size_t>(m_hMetricCount) * 4;
        ByteReader bearingReader(m_data, bearings + static_cast<std::size_t>(glyph - m_hMetricCount) * 2);
        metrics.leftSideBearing = bearingReader.s16();
    }
    return metrics;
}

std::int32_t TrueTypeFont::kerning(std::uint16_t left, std::uint16_t right) const
{
    if (!m_valid || m_kern.length == 0) {
        return 0;
    }

    ByteReader reader(m_data, m_kern.offset + 2);
    const std::uint16_t subtableCount = reader.u16();
    std::size_t subtable = m_kern.offset + 4;

    for (std::uint16_t i = 0; i < subtableCount; ++i) {
        ByteReader headerReader(m_data, subtable + 2);
        const std::uint16_t length = headerReader.u16();
        const std::uint16_t coverage = headerReader.u16();
        if (!headerReader.ok() || length < 14) {
            return 0;
        }

        // Horizontal, non-minimum, format 0 is the only shape worth reading.
        if ((coverage & 0x0001) != 0 && (coverage & 0x0002) == 0 && (coverage >> 8) == 0) {
            ByteReader pairsReader(m_data, subtable + 6);
            const std::uint16_t pairCount = pairsReader.u16();
            const std::size_t pairs = subtable + 14;
            const std::uint32_t key =
                (static_cast<std::uint32_t>(left) << 16) | static_cast<std::uint32_t>(right);

            std::uint32_t low = 0;
            std::uint32_t high = pairCount;
            while (low < high) {
                const std::uint32_t middle = low + (high - low) / 2;
                ByteReader pairReader(m_data, pairs + static_cast<std::size_t>(middle) * 6);
                const std::uint32_t pairKey = pairReader.u32();
                const std::int16_t value = pairReader.s16();
                if (!pairReader.ok()) {
                    return 0;
                }
                if (key < pairKey) {
                    high = middle;
                } else if (key > pairKey) {
                    low = middle + 1;
                } else {
                    return value;
                }
            }
            return 0;
        }
        subtable += length;
    }
    return 0;
}

bool TrueTypeFont::glyphRange(std::uint16_t glyph, std::uint32_t& begin, std::uint32_t& end) const
{
    if (glyph >= m_glyphCount) {
        return false;
    }

    if (m_longLoca) {
        ByteReader reader(m_data, m_loca.offset + static_cast<std::size_t>(glyph) * 4);
        begin = reader.u32();
        end = reader.u32();
        if (!reader.ok()) {
            return false;
        }
    } else {
        ByteReader reader(m_data, m_loca.offset + static_cast<std::size_t>(glyph) * 2);
        begin = static_cast<std::uint32_t>(reader.u16()) * 2;
        end = static_cast<std::uint32_t>(reader.u16()) * 2;
        if (!reader.ok()) {
            return false;
        }
    }
    return end >= begin && end <= m_glyf.length;
}

bool TrueTypeFont::appendGlyph(std::uint16_t glyph, int depth, float offsetX, float offsetY, float xx,
                               float xy, float yx, float yy, GlyphOutline& out) const
{
    if (depth > kMaxCompositeDepth) {
        SOL_LOG_ERROR("truetype: composite glyph nesting too deep");
        return false;
    }

    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    if (!glyphRange(glyph, begin, end)) {
        return false;
    }
    if (begin == end) {
        return true; // empty glyph, e.g. space
    }

    const std::size_t glyphStart = m_glyf.offset + begin;
    ByteReader reader(m_data, glyphStart);
    const std::int16_t contourCount = reader.s16();
    const std::int16_t xMin = reader.s16();
    const std::int16_t yMin = reader.s16();
    const std::int16_t xMax = reader.s16();
    const std::int16_t yMax = reader.s16();
    if (!reader.ok()) {
        return false;
    }

    if (depth == 0) {
        out.xMin = xMin;
        out.yMin = yMin;
        out.xMax = xMax;
        out.yMax = yMax;
    }

    if (contourCount < 0) {
        // Composite: each component is another glyph under a 2x3 transform.
        std::uint16_t flags = 0;
        do {
            flags = reader.u16();
            const std::uint16_t componentGlyph = reader.u16();
            if (!reader.ok()) {
                return false;
            }

            float dx = 0.0f;
            float dy = 0.0f;
            if ((flags & kArgsAreXy) != 0) {
                if ((flags & kArgsAreWords) != 0) {
                    dx = static_cast<float>(reader.s16());
                    dy = static_cast<float>(reader.s16());
                } else {
                    dx = static_cast<float>(static_cast<std::int8_t>(reader.u8()));
                    dy = static_cast<float>(static_cast<std::int8_t>(reader.u8()));
                }
            } else {
                // Point-matching placement: vanishingly rare, and honoring it
                // would mean tracking assembled point indices. Skip the args
                // and place the component unshifted rather than guessing.
                reader.skip((flags & kArgsAreWords) != 0 ? 4 : 2);
            }

            float componentXX = 1.0f;
            float componentXY = 0.0f;
            float componentYX = 0.0f;
            float componentYY = 1.0f;
            if ((flags & kHaveScale) != 0) {
                componentXX = componentYY = reader.f2Dot14();
            } else if ((flags & kHaveXYScale) != 0) {
                componentXX = reader.f2Dot14();
                componentYY = reader.f2Dot14();
            } else if ((flags & kHaveTwoByTwo) != 0) {
                componentXX = reader.f2Dot14();
                componentXY = reader.f2Dot14();
                componentYX = reader.f2Dot14();
                componentYY = reader.f2Dot14();
            }
            if (!reader.ok()) {
                return false;
            }

            // Compose the component transform under the one already in force.
            const float childXX = xx * componentXX + yx * componentXY;
            const float childXY = xy * componentXX + yy * componentXY;
            const float childYX = xx * componentYX + yx * componentYY;
            const float childYY = xy * componentYX + yy * componentYY;
            const float childDx = xx * dx + yx * dy + offsetX;
            const float childDy = xy * dx + yy * dy + offsetY;

            if (!appendGlyph(componentGlyph, depth + 1, childDx, childDy, childXX, childXY, childYX,
                             childYY, out)) {
                return false;
            }
        } while ((flags & kMoreComponents) != 0);
        return true;
    }

    // Simple glyph.
    const std::uint32_t contours = static_cast<std::uint32_t>(contourCount);
    std::vector<std::uint16_t> contourEndPoints(contours);
    for (std::uint32_t i = 0; i < contours; ++i) {
        contourEndPoints[i] = reader.u16();
    }
    const std::uint16_t instructionLength = reader.u16();
    reader.skip(instructionLength);
    if (!reader.ok()) {
        return false;
    }

    const std::uint32_t pointCount = static_cast<std::uint32_t>(contourEndPoints.back()) + 1;
    if (pointCount == 0 || pointCount > 0xFFFF) {
        return false;
    }

    std::vector<std::uint8_t> flags(pointCount);
    for (std::uint32_t i = 0; i < pointCount;) {
        const std::uint8_t flag = reader.u8();
        if (!reader.ok()) {
            return false;
        }
        flags[i++] = flag;
        if ((flag & kRepeat) != 0) {
            std::uint8_t repeats = reader.u8();
            while (repeats-- > 0 && i < pointCount) {
                flags[i++] = flag;
            }
        }
    }

    std::vector<std::int32_t> xs(pointCount);
    std::int32_t coordinate = 0;
    for (std::uint32_t i = 0; i < pointCount; ++i) {
        const std::uint8_t flag = flags[i];
        if ((flag & kXShort) != 0) {
            const std::int32_t delta = reader.u8();
            coordinate += (flag & kXSamePositive) != 0 ? delta : -delta;
        } else if ((flag & kXSamePositive) == 0) {
            coordinate += reader.s16();
        }
        xs[i] = coordinate;
    }

    std::vector<std::int32_t> ys(pointCount);
    coordinate = 0;
    for (std::uint32_t i = 0; i < pointCount; ++i) {
        const std::uint8_t flag = flags[i];
        if ((flag & kYShort) != 0) {
            const std::int32_t delta = reader.u8();
            coordinate += (flag & kYSamePositive) != 0 ? delta : -delta;
        } else if ((flag & kYSamePositive) == 0) {
            coordinate += reader.s16();
        }
        ys[i] = coordinate;
    }

    if (!reader.ok()) {
        return false;
    }

    std::uint32_t pointIndex = 0;
    for (std::uint32_t contour = 0; contour < contours; ++contour) {
        const std::uint32_t contourEnd = static_cast<std::uint32_t>(contourEndPoints[contour]) + 1;
        if (contourEnd > pointCount || contourEnd < pointIndex) {
            return false;
        }
        for (; pointIndex < contourEnd; ++pointIndex) {
            const float x = static_cast<float>(xs[pointIndex]);
            const float y = static_cast<float>(ys[pointIndex]);
            OutlinePoint point;
            point.x = xx * x + yx * y + offsetX;
            point.y = xy * x + yy * y + offsetY;
            point.onCurve = (flags[pointIndex] & kOnCurve) != 0;
            out.points.push_back(point);
        }
        out.contourEnds.push_back(static_cast<std::uint32_t>(out.points.size()));
    }

    return true;
}

bool TrueTypeFont::glyphOutline(std::uint16_t glyph, GlyphOutline& out) const
{
    out = GlyphOutline();
    if (!m_valid) {
        return false;
    }
    return appendGlyph(glyph, 0, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, out);
}

bool TrueTypeFont::rasterizeGlyph(std::uint16_t glyph, float scale, GlyphBitmap& out) const
{
    out = GlyphBitmap();
    if (!m_valid || scale <= 0.0f) {
        return false;
    }

    GlyphOutline outline;
    if (!glyphOutline(glyph, outline)) {
        return false;
    }
    out.advance = static_cast<float>(glyphMetrics(glyph).advanceWidth) * scale;
    if (outline.contourEnds.empty() || outline.points.empty()) {
        return true; // whitespace: metrics only
    }

    // Bounds from the points themselves rather than the header box: composite
    // components can reach outside it, and control points are a conservative
    // (never too small) bound for the curve they steer.
    float minX = outline.points[0].x;
    float maxX = minX;
    float minY = outline.points[0].y;
    float maxY = minY;
    for (const OutlinePoint& point : outline.points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }

    // Font units are y-up; the atlas is y-down, so the top edge comes from maxY.
    const float left = std::floor(minX * scale);
    const float top = std::floor(-maxY * scale);
    const float right = std::ceil(maxX * scale);
    const float bottom = std::ceil(-minY * scale);

    const int width = static_cast<int>(right - left);
    const int height = static_cast<int>(bottom - top);
    if (width <= 0 || height <= 0) {
        return true;
    }

    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.bearingX = static_cast<std::int32_t>(left);
    out.bearingY = static_cast<std::int32_t>(top);

    CoverageAccumulator accumulator(out.width, out.height);

    const auto toPixels = [&](const OutlinePoint& point) {
        return Vec2f{point.x * scale - left, -point.y * scale - top};
    };

    std::uint32_t contourStart = 0;
    for (const std::uint32_t contourEnd : outline.contourEnds) {
        const std::uint32_t count = contourEnd - contourStart;
        if (count < 2) {
            contourStart = contourEnd;
            continue;
        }

        // Walk the contour from an on-curve point. When every point is a
        // control point the format implies an on-curve start at the midpoint
        // of the wrap-around pair.
        std::uint32_t firstOnCurve = count;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (outline.points[contourStart + i].onCurve) {
                firstOnCurve = i;
                break;
            }
        }

        std::vector<Vec2f> walk;
        std::vector<bool> walkOnCurve;
        walk.reserve(count + 1);
        walkOnCurve.reserve(count + 1);

        Vec2f start;
        if (firstOnCurve == count) {
            start = midpoint(toPixels(outline.points[contourEnd - 1]),
                             toPixels(outline.points[contourStart]));
            for (std::uint32_t i = 0; i < count; ++i) {
                walk.push_back(toPixels(outline.points[contourStart + i]));
                walkOnCurve.push_back(false);
            }
            walk.push_back(start);
            walkOnCurve.push_back(true);
        } else {
            start = toPixels(outline.points[contourStart + firstOnCurve]);
            for (std::uint32_t k = 1; k <= count; ++k) {
                const std::uint32_t index = contourStart + (firstOnCurve + k) % count;
                walk.push_back(toPixels(outline.points[index]));
                walkOnCurve.push_back(outline.points[index].onCurve);
            }
        }

        Vec2f cursor = start;
        Vec2f pendingControl;
        bool havePending = false;
        for (std::size_t i = 0; i < walk.size(); ++i) {
            const Vec2f& point = walk[i];
            if (walkOnCurve[i]) {
                if (havePending) {
                    flattenQuadratic(cursor, pendingControl, point, accumulator);
                    havePending = false;
                } else {
                    accumulator.line(cursor.x, cursor.y, point.x, point.y);
                }
                cursor = point;
            } else {
                if (havePending) {
                    const Vec2f implied = midpoint(pendingControl, point);
                    flattenQuadratic(cursor, pendingControl, implied, accumulator);
                    cursor = implied;
                }
                pendingControl = point;
                havePending = true;
            }
        }
        if (havePending) {
            flattenQuadratic(cursor, pendingControl, start, accumulator);
        } else if (cursor.x != start.x || cursor.y != start.y) {
            accumulator.line(cursor.x, cursor.y, start.x, start.y);
        }

        contourStart = contourEnd;
    }

    accumulator.resolve(out.coverage);
    return true;
}

} // namespace sol::assets
