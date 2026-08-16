#pragma once

#include "sol/assets/font.hpp"
#include "sol/core/math/vec.hpp"
#include "sol/renderer/ui_renderer.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sol::ui {

// Straight-alpha RGBA, 0..1. The renderer premultiplies.
struct Color
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    [[nodiscard]] constexpr Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
};

[[nodiscard]] constexpr Color rgba(std::uint32_t hex)
{
    return {static_cast<float>((hex >> 24) & 0xFFu) / 255.0f,
            static_cast<float>((hex >> 16) & 0xFFu) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFFu) / 255.0f,
            static_cast<float>(hex & 0xFFu) / 255.0f};
}

struct Rect
{
    core::Vec2 min;
    core::Vec2 max;

    [[nodiscard]] constexpr float width() const { return max.x - min.x; }
    [[nodiscard]] constexpr float height() const { return max.y - min.y; }
    [[nodiscard]] constexpr bool empty() const { return max.x <= min.x || max.y <= min.y; }
    [[nodiscard]] constexpr bool contains(core::Vec2 point) const
    {
        return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
    }
};

enum class TextAlign
{
    Left,
    Center,
    Right,
};

// Immediate-mode geometry builder: widgets call these, and the accumulated
// vertices/indices/batches go straight to renderer::UiRenderer. Holds no GPU
// state, so it is fully testable without a device - which is where the layout
// and clipping tests live.
class DrawList
{
public:
    using Vertex = renderer::UiRenderer::Vertex;
    using Batch = renderer::UiRenderer::Batch;

    // Matches the renderer's per-frame capacity. Indices are 16-bit, so this
    // is a correctness limit, not a tuning knob.
    static constexpr std::size_t kMaxVertices = renderer::UiRenderer::kMaxVertices;
    static constexpr std::size_t kMaxIndices = renderer::UiRenderer::kMaxIndices;

    // The font whose atlas backs text drawing, and the texture slot it was
    // registered in. Text is dropped (not crashed on) when unset.
    void setFont(const assets::Font* font, std::uint32_t fontTexture);
    [[nodiscard]] const assets::Font* font() const { return m_font; }

    // Clears the frame's geometry; call once per frame before building.
    void reset();

    void pushClip(const Rect& rect);
    void popClip();
    [[nodiscard]] Rect currentClip() const;

    void addRect(const Rect& rect, const Color& color);
    void addRectOutline(const Rect& rect, const Color& color, float thickness = 1.0f);
    void addRoundedRect(const Rect& rect, float radius, const Color& color, int cornerSegments = 4);
    void addLine(core::Vec2 from, core::Vec2 to, const Color& color, float thickness = 1.0f);
    void addTriangle(core::Vec2 a, core::Vec2 b, core::Vec2 c, const Color& color);

    // Stroked ring segment: angles are radians measured from +x and sweep the
    // way the screen runs, so -pi/2 is up. Drawn as one strip between an inner
    // and an outer radius rather than as joined line segments, which keeps a
    // thick arc free of gaps at the joints. `segments` covers the whole sweep.
    void addArc(core::Vec2 center, float radius, float startAngle, float endAngle,
                const Color& color, float thickness = 1.0f, int segments = 24);
    void addCircle(core::Vec2 center, float radius, const Color& color, float thickness = 1.0f,
                   int segments = 24);

    // Draws UTF-8 text with `position` at the left end of the baseline.
    // Returns the advance width consumed.
    float addText(const assets::FontStyleRecord& style, core::Vec2 position, std::string_view text,
                  const Color& color);

    // Draws inside `box` using the style's ascent/descent to center vertically.
    float addTextInBox(const assets::FontStyleRecord& style, const Rect& box, std::string_view text,
                       const Color& color, TextAlign align = TextAlign::Left);

    [[nodiscard]] std::span<const Vertex> vertices() const { return m_vertices; }
    [[nodiscard]] std::span<const std::uint16_t> indices() const { return m_indices; }
    [[nodiscard]] std::span<const Batch> batches() const { return m_batches; }

    // True when a build overflowed the 16-bit index space and geometry was
    // dropped; the frame is still drawable, just incomplete.
    [[nodiscard]] bool overflowed() const { return m_overflowed; }

private:
    // Starts or extends the batch matching the current texture and clip.
    void selectBatch(std::uint32_t texture);
    // True when the current clip has collapsed to nothing. Geometry under it
    // must be dropped here: a zero-area clip rect reaches the renderer looking
    // exactly like "no clip", and would be drawn over the whole screen.
    [[nodiscard]] bool clipIsEmpty() const;
    [[nodiscard]] bool reserve(std::size_t vertexCount, std::size_t indexCount);
    void addQuad(core::Vec2 topLeft, core::Vec2 bottomRight, core::Vec2 uvMin, core::Vec2 uvMax,
                 const Color& color, std::uint32_t texture);

    const assets::Font* m_font = nullptr;
    std::uint32_t m_fontTexture = 0;

    std::vector<Vertex> m_vertices;
    std::vector<std::uint16_t> m_indices;
    std::vector<Batch> m_batches;
    std::vector<Rect> m_clipStack;
    bool m_overflowed = false;
};

} // namespace sol::ui
