#include "sol/ui/draw_list.hpp"

#include <algorithm>
#include <cmath>

namespace sol::ui {

namespace {

// The renderer's vertex format is R8G8B8A8_UNORM, so bytes read r,g,b,a.
std::uint32_t packColor(const Color& color)
{
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return channel(color.r) | (channel(color.g) << 8) | (channel(color.b) << 16) |
           (channel(color.a) << 24);
}

// A 1x1 white texture samples the same everywhere; the middle avoids any
// filtering surprise at the edges.
constexpr core::Vec2 kWhiteUv = {0.5f, 0.5f};

constexpr Rect kUnclipped = {{0.0f, 0.0f}, {0.0f, 0.0f}};

} // namespace

void DrawList::setFont(const assets::Font* font, std::uint32_t fontTexture)
{
    m_font = font;
    m_fontTexture = fontTexture;
}

void DrawList::reset()
{
    m_vertices.clear();
    m_indices.clear();
    m_batches.clear();
    m_clipStack.clear();
    m_overflowed = false;
}

void DrawList::pushClip(const Rect& rect)
{
    // Nested clips intersect, so a child can never draw outside its parent.
    Rect clipped = rect;
    if (!m_clipStack.empty()) {
        const Rect& parent = m_clipStack.back();
        clipped.min.x = std::max(clipped.min.x, parent.min.x);
        clipped.min.y = std::max(clipped.min.y, parent.min.y);
        clipped.max.x = std::min(clipped.max.x, parent.max.x);
        clipped.max.y = std::min(clipped.max.y, parent.max.y);
    }
    clipped.max.x = std::max(clipped.max.x, clipped.min.x);
    clipped.max.y = std::max(clipped.max.y, clipped.min.y);
    m_clipStack.push_back(clipped);
}

void DrawList::popClip()
{
    if (!m_clipStack.empty()) {
        m_clipStack.pop_back();
    }
}

Rect DrawList::currentClip() const
{
    return m_clipStack.empty() ? kUnclipped : m_clipStack.back();
}

void DrawList::selectBatch(std::uint32_t texture)
{
    const Rect clip = currentClip();
    if (!m_batches.empty()) {
        Batch& last = m_batches.back();
        const bool sameClip = last.clipMin.x == clip.min.x && last.clipMin.y == clip.min.y &&
                              last.clipMax.x == clip.max.x && last.clipMax.y == clip.max.y;
        if (last.texture == texture && sameClip) {
            return; // keep appending to it
        }
    }

    Batch batch = {};
    batch.firstIndex = static_cast<std::uint32_t>(m_indices.size());
    batch.indexCount = 0;
    batch.texture = texture;
    batch.clipMin = clip.min;
    batch.clipMax = clip.max;
    m_batches.push_back(batch);
}

bool DrawList::reserve(std::size_t vertexCount, std::size_t indexCount)
{
    // Vertex indices are 16-bit, so the cap is a hard correctness limit rather
    // than a tuning knob: overflowing would silently draw garbage.
    if (m_vertices.size() + vertexCount > kMaxVertices ||
        m_indices.size() + indexCount > kMaxIndices) {
        m_overflowed = true;
        return false;
    }
    return true;
}

bool DrawList::clipIsEmpty() const
{
    if (m_clipStack.empty()) {
        return false;
    }
    const Rect& clip = m_clipStack.back();
    return clip.max.x <= clip.min.x || clip.max.y <= clip.min.y;
}

void DrawList::addQuad(core::Vec2 topLeft, core::Vec2 bottomRight, core::Vec2 uvMin, core::Vec2 uvMax,
                       const Color& color, std::uint32_t texture)
{
    if (color.a <= 0.0f || bottomRight.x <= topLeft.x || bottomRight.y <= topLeft.y ||
        clipIsEmpty()) {
        return;
    }
    if (!reserve(4, 6)) {
        return;
    }
    selectBatch(texture);

    const std::uint16_t base = static_cast<std::uint16_t>(m_vertices.size());
    const std::uint32_t packed = packColor(color);
    m_vertices.push_back({{topLeft.x, topLeft.y}, {uvMin.x, uvMin.y}, packed});
    m_vertices.push_back({{bottomRight.x, topLeft.y}, {uvMax.x, uvMin.y}, packed});
    m_vertices.push_back({{bottomRight.x, bottomRight.y}, {uvMax.x, uvMax.y}, packed});
    m_vertices.push_back({{topLeft.x, bottomRight.y}, {uvMin.x, uvMax.y}, packed});

    const std::uint16_t order[6] = {0, 1, 2, 0, 2, 3};
    for (const std::uint16_t index : order) {
        m_indices.push_back(static_cast<std::uint16_t>(base + index));
    }
    m_batches.back().indexCount += 6;
}

void DrawList::addRect(const Rect& rect, const Color& color)
{
    addQuad(rect.min, rect.max, kWhiteUv, kWhiteUv, color, 0);
}

void DrawList::addRectOutline(const Rect& rect, const Color& color, float thickness)
{
    if (rect.empty() || thickness <= 0.0f) {
        return;
    }
    const float t = std::min(thickness, std::min(rect.width(), rect.height()) * 0.5f);
    addRect({{rect.min.x, rect.min.y}, {rect.max.x, rect.min.y + t}}, color);         // top
    addRect({{rect.min.x, rect.max.y - t}, {rect.max.x, rect.max.y}}, color);         // bottom
    addRect({{rect.min.x, rect.min.y + t}, {rect.min.x + t, rect.max.y - t}}, color); // left
    addRect({{rect.max.x - t, rect.min.y + t}, {rect.max.x, rect.max.y - t}}, color); // right
}

void DrawList::addTriangle(core::Vec2 a, core::Vec2 b, core::Vec2 c, const Color& color)
{
    if (color.a <= 0.0f || clipIsEmpty() || !reserve(3, 3)) {
        return;
    }
    selectBatch(0);
    const std::uint16_t base = static_cast<std::uint16_t>(m_vertices.size());
    const std::uint32_t packed = packColor(color);
    m_vertices.push_back({a, kWhiteUv, packed});
    m_vertices.push_back({b, kWhiteUv, packed});
    m_vertices.push_back({c, kWhiteUv, packed});
    for (std::uint16_t i = 0; i < 3; ++i) {
        m_indices.push_back(static_cast<std::uint16_t>(base + i));
    }
    m_batches.back().indexCount += 3;
}

void DrawList::addLine(core::Vec2 from, core::Vec2 to, const Color& color, float thickness)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f || thickness <= 0.0f || color.a <= 0.0f || clipIsEmpty()) {
        return;
    }
    if (!reserve(4, 6)) {
        return;
    }
    selectBatch(0);

    // Expand the segment into a quad around its own normal.
    const float half = thickness * 0.5f;
    const float nx = -dy / length * half;
    const float ny = dx / length * half;

    const std::uint16_t base = static_cast<std::uint16_t>(m_vertices.size());
    const std::uint32_t packed = packColor(color);
    m_vertices.push_back({{from.x + nx, from.y + ny}, kWhiteUv, packed});
    m_vertices.push_back({{to.x + nx, to.y + ny}, kWhiteUv, packed});
    m_vertices.push_back({{to.x - nx, to.y - ny}, kWhiteUv, packed});
    m_vertices.push_back({{from.x - nx, from.y - ny}, kWhiteUv, packed});

    const std::uint16_t order[6] = {0, 1, 2, 0, 2, 3};
    for (const std::uint16_t index : order) {
        m_indices.push_back(static_cast<std::uint16_t>(base + index));
    }
    m_batches.back().indexCount += 6;
}

void DrawList::addRoundedRect(const Rect& rect, float radius, const Color& color, int cornerSegments)
{
    if (rect.empty() || color.a <= 0.0f || clipIsEmpty()) {
        return;
    }
    const float limit = std::min(rect.width(), rect.height()) * 0.5f;
    const float r = std::clamp(radius, 0.0f, limit);
    if (r <= 0.0f) {
        addRect(rect, color);
        return;
    }

    const int segments = std::clamp(cornerSegments, 1, 16);
    // Corner centers in draw order: top-left, top-right, bottom-right, bottom-left.
    const core::Vec2 centers[4] = {{rect.min.x + r, rect.min.y + r},
                                   {rect.max.x - r, rect.min.y + r},
                                   {rect.max.x - r, rect.max.y - r},
                                   {rect.min.x + r, rect.max.y - r}};
    const float startAngles[4] = {3.14159265f, 4.71238898f, 0.0f, 1.57079633f};

    std::vector<core::Vec2> perimeter;
    perimeter.reserve(static_cast<std::size_t>(4 * (segments + 1)));
    for (int corner = 0; corner < 4; ++corner) {
        for (int step = 0; step <= segments; ++step) {
            const float angle =
                startAngles[corner] + 1.57079633f * static_cast<float>(step) / static_cast<float>(segments);
            perimeter.push_back({centers[corner].x + std::cos(angle) * r,
                                 centers[corner].y + std::sin(angle) * r});
        }
    }

    const std::size_t fanVertices = perimeter.size() + 1;
    const std::size_t fanIndices = perimeter.size() * 3;
    if (!reserve(fanVertices, fanIndices)) {
        return;
    }
    selectBatch(0);

    const std::uint16_t base = static_cast<std::uint16_t>(m_vertices.size());
    const std::uint32_t packed = packColor(color);
    m_vertices.push_back({{(rect.min.x + rect.max.x) * 0.5f, (rect.min.y + rect.max.y) * 0.5f}, kWhiteUv,
                          packed});
    for (const core::Vec2& point : perimeter) {
        m_vertices.push_back({point, kWhiteUv, packed});
    }
    for (std::size_t i = 0; i < perimeter.size(); ++i) {
        const std::size_t next = (i + 1) % perimeter.size();
        m_indices.push_back(base);
        m_indices.push_back(static_cast<std::uint16_t>(base + 1 + i));
        m_indices.push_back(static_cast<std::uint16_t>(base + 1 + next));
    }
    m_batches.back().indexCount += static_cast<std::uint32_t>(fanIndices);
}

float DrawList::addText(const assets::FontStyleRecord& style, core::Vec2 position, std::string_view text,
                        const Color& color)
{
    if (m_font == nullptr || !m_font->valid() || text.empty() || color.a <= 0.0f) {
        return 0.0f;
    }

    const float atlasWidth = static_cast<float>(m_font->atlasWidth());
    const float atlasHeight = static_cast<float>(m_font->atlasHeight());
    if (atlasWidth <= 0.0f || atlasHeight <= 0.0f) {
        return 0.0f;
    }

    float pen = position.x;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const char32_t codepoint = assets::nextCodepoint(text, cursor);
        const assets::FontGlyphRecord* glyph = m_font->glyph(style, codepoint);
        if (glyph == nullptr) {
            continue;
        }
        if (glyph->width != 0 && glyph->height != 0) {
            // bearingY is measured from the baseline, negative upward.
            const core::Vec2 topLeft = {pen + static_cast<float>(glyph->bearingX),
                                        position.y + static_cast<float>(glyph->bearingY)};
            const core::Vec2 bottomRight = {topLeft.x + static_cast<float>(glyph->width),
                                            topLeft.y + static_cast<float>(glyph->height)};
            const core::Vec2 uvMin = {static_cast<float>(glyph->atlasX) / atlasWidth,
                                      static_cast<float>(glyph->atlasY) / atlasHeight};
            const core::Vec2 uvMax = {static_cast<float>(glyph->atlasX + glyph->width) / atlasWidth,
                                      static_cast<float>(glyph->atlasY + glyph->height) / atlasHeight};
            addQuad(topLeft, bottomRight, uvMin, uvMax, color, m_fontTexture);
        }
        pen += glyph->advance;
    }
    return pen - position.x;
}

float DrawList::addTextInBox(const assets::FontStyleRecord& style, const Rect& box,
                             std::string_view text, const Color& color, TextAlign align)
{
    if (m_font == nullptr || !m_font->valid()) {
        return 0.0f;
    }
    const float width = m_font->measureWidth(style, text);

    float x = box.min.x;
    if (align == TextAlign::Center) {
        x = box.min.x + (box.width() - width) * 0.5f;
    } else if (align == TextAlign::Right) {
        x = box.max.x - width;
    }

    // Center the em box vertically, then sit the baseline under the ascent.
    const float textHeight = style.ascent - style.descent;
    const float baseline = box.min.y + (box.height() - textHeight) * 0.5f + style.ascent;
    return addText(style, {std::round(x), std::round(baseline)}, text, color);
}

} // namespace sol::ui
