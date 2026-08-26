#pragma once

#include "sol/ui/draw_list.hpp"

namespace sol::ui {

// Deliberately not a flex engine: two cursors that hand out rectangles, which
// covers every screen in the game and stays obvious to read. Widgets take
// explicit rects, so layout stays a separate, optional concern.

// Stacks rows down a box, insetting by `padding` and leaving `spacing`
// between rows.
class Column
{
public:
    Column(const Rect& bounds, float padding = 0.0f, float spacing = 0.0f)
        : m_bounds{{bounds.min.x + padding, bounds.min.y + padding},
                   {bounds.max.x - padding, bounds.max.y - padding}}
        , m_spacing(spacing)
        , m_cursor(bounds.min.y + padding)
    {
    }

    // Next row of the given height. Rows past the bottom still return a rect
    // so callers stay branch-free; clip or scroll to hide them.
    [[nodiscard]] Rect row(float height)
    {
        const float top = m_cursor;
        m_cursor += height + m_spacing;
        return {{m_bounds.min.x, top}, {m_bounds.max.x, top + height}};
    }

    void skip(float height) { m_cursor += height; }

    // Everything not yet handed out.
    [[nodiscard]] Rect remaining() const
    {
        return {{m_bounds.min.x, m_cursor}, {m_bounds.max.x, m_bounds.max.y}};
    }

    [[nodiscard]] float cursor() const { return m_cursor; }

    [[nodiscard]] float width() const { return m_bounds.width(); }

    // Height consumed so far, spacing after the last row excluded.
    [[nodiscard]] float consumed() const
    {
        const float used = m_cursor - m_bounds.min.y;
        return used > m_spacing ? used - m_spacing : 0.0f;
    }

private:
    Rect m_bounds;
    float m_spacing = 0.0f;
    float m_cursor = 0.0f;
};

// Splits a row into cells from either end, so a label on the left and a value
// on the right can both be placed without measuring the middle.
class Row
{
public:
    Row(const Rect& bounds, float spacing = 0.0f)
        : m_bounds(bounds), m_spacing(spacing), m_left(bounds.min.x), m_right(bounds.max.x)
    {
    }

    [[nodiscard]] Rect cell(float width)
    {
        const float start = m_left;
        m_left += width + m_spacing;
        return {{start, m_bounds.min.y}, {start + width, m_bounds.max.y}};
    }

    [[nodiscard]] Rect cellFromRight(float width)
    {
        const float end = m_right;
        m_right -= width + m_spacing;
        return {{end - width, m_bounds.min.y}, {end, m_bounds.max.y}};
    }

    // The gap between the two cursors: whatever the fixed cells left over.
    [[nodiscard]] Rect remaining() const
    {
        const float right = m_right > m_left ? m_right : m_left;
        return {{m_left, m_bounds.min.y}, {right, m_bounds.max.y}};
    }

    // Splits what is left into `count` equal cells and returns the one at
    // `index`; useful for tab strips and button rows.
    [[nodiscard]] Rect fraction(int index, int count) const
    {
        const Rect free = remaining();
        if (count <= 0) {
            return free;
        }
        const float each =
            (free.width() - m_spacing * static_cast<float>(count - 1)) / static_cast<float>(count);
        const float start = free.min.x + (each + m_spacing) * static_cast<float>(index);
        return {{start, free.min.y}, {start + each, free.max.y}};
    }

private:
    Rect m_bounds;
    float m_spacing = 0.0f;
    float m_left = 0.0f;
    float m_right = 0.0f;
};

[[nodiscard]] inline Rect inset(const Rect& rect, float amount)
{
    return {{rect.min.x + amount, rect.min.y + amount}, {rect.max.x - amount, rect.max.y - amount}};
}

} // namespace sol::ui
