#include "sol/ui/context.hpp"

#include "sol/core/hash.hpp"

#include <algorithm>
#include <cstdio>

namespace sol::ui {

namespace {

constexpr WidgetId kRootId = sol::core::fnv1a("sol.ui");

float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

void UiContext::setFont(const assets::Font* font, std::uint32_t fontTexture)
{
    m_font = font;
    m_drawList.setFont(font, fontTexture);
}

void UiContext::beginFrame(const InputState& input, core::Vec2 screenSize)
{
    m_navLeftEdge = input.navLeft && !m_previousNavLeft;
    m_navRightEdge = input.navRight && !m_previousNavRight;
    m_previousNavLeft = input.navLeft;
    m_previousNavRight = input.navRight;

    m_input = input;
    m_screenSize = screenSize;
    m_drawList.reset();
    m_idStack.clear();
    m_navItems.clear();
    m_scrollStack.clear();
    m_hotId = kNoWidget;
    m_frameOpen = true;
}

void UiContext::endFrame()
{
    // Focus moves at the end of the frame, when the full widget order is
    // known; the change lands on the next build, which is invisible at frame
    // rate and keeps nav independent of where the key was pressed.
    if (!m_navItems.empty() && (m_input.navNext || m_input.navPrevious)) {
        const auto found = std::find(m_navItems.begin(), m_navItems.end(), m_focusId);
        const std::size_t count = m_navItems.size();
        std::size_t index = found == m_navItems.end() ? count : static_cast<std::size_t>(
                                                                    found - m_navItems.begin());
        if (index == count) {
            // Nothing focused yet: enter the screen at either end.
            index = m_input.navNext ? 0 : count - 1;
        } else if (m_input.navNext) {
            index = (index + 1) % count;
        } else {
            index = (index + count - 1) % count;
        }
        m_focusId = m_navItems[index];
    }

    // A focus target that vanished (a screen changed, a row scrolled away)
    // must not keep receiving activation.
    if (m_focusId != kNoWidget &&
        std::find(m_navItems.begin(), m_navItems.end(), m_focusId) == m_navItems.end()) {
        m_focusId = kNoWidget;
    }

    if (m_input.mouseReleased) {
        m_activeId = kNoWidget;
    }
    m_frameOpen = false;
}

void UiContext::pushId(std::string_view name)
{
    const WidgetId seed = m_idStack.empty() ? kRootId : m_idStack.back();
    m_idStack.push_back(core::fnv1a(name, seed));
}

void UiContext::pushId(int index)
{
    const WidgetId seed = m_idStack.empty() ? kRootId : m_idStack.back();
    m_idStack.push_back(core::hashCombine(seed, static_cast<std::uint64_t>(index)));
}

void UiContext::popId()
{
    if (!m_idStack.empty()) {
        m_idStack.pop_back();
    }
}

WidgetId UiContext::idFor(std::string_view label) const
{
    const WidgetId seed = m_idStack.empty() ? kRootId : m_idStack.back();
    const WidgetId id = core::fnv1a(label, seed);
    return id == kNoWidget ? kRootId : id; // never hand out the "nothing" id
}

const assets::FontStyleRecord* UiContext::style(const char* name) const
{
    if (m_font == nullptr || !m_font->valid() || name == nullptr) {
        return nullptr;
    }
    return m_font->style(name);
}

UiContext::Interaction UiContext::interact(std::string_view label, const Rect& bounds, bool enabled)
{
    Interaction interaction;
    interaction.id = idFor(label);
    if (!enabled) {
        // Disabled widgets are skipped by keyboard nav rather than focusable
        // dead ends.
        return interaction;
    }

    m_navItems.push_back(interaction.id);
    interaction.focused = isFocused(interaction.id);
    // Inside a scroll region the cursor must be over the region as well as the
    // widget: a row scrolled out of view is still at its layout position, and
    // clipping it visually is no help if it can still be clicked.
    interaction.hovered =
        bounds.contains(m_input.mousePosition) &&
        (m_scrollStack.empty() || m_scrollStack.back().bounds.contains(m_input.mousePosition));
    if (interaction.focused && !m_scrollStack.empty()) {
        ScrollRegion& region = m_scrollStack.back();
        region.focusRect = bounds;
        region.hasFocusRect = true;
    }

    if (interaction.hovered) {
        m_hotId = interaction.id;
        if (m_input.mousePressed) {
            m_activeId = interaction.id;
            m_focusId = interaction.id; // clicking also takes keyboard focus
            interaction.focused = true;
        }
    }
    interaction.held = m_activeId == interaction.id;

    // A click counts only if it went down and came up on the same widget.
    const bool clicked = interaction.held && interaction.hovered && m_input.mouseReleased;
    interaction.activated = clicked || (interaction.focused && m_input.navActivate);
    return interaction;
}

Color UiContext::controlColor(const Interaction& interaction, bool enabled) const
{
    if (!enabled) {
        return m_theme.controlDisabled;
    }
    if (interaction.held && interaction.hovered) {
        return m_theme.controlActive;
    }
    if (interaction.hovered) {
        return m_theme.controlHovered;
    }
    return m_theme.control;
}

void UiContext::drawFocusRing(const Rect& bounds, const Interaction& interaction)
{
    if (!interaction.focused) {
        return;
    }
    m_drawList.addRectOutline(bounds, m_theme.focusRing, m_theme.focusRingThickness);
}

void UiContext::label(const Rect& bounds, std::string_view text, TextAlign align)
{
    label(bounds, text, m_theme.textPrimary, nullptr, align);
}

void UiContext::label(const Rect& bounds, std::string_view text, const Color& color,
                      const char* styleName, TextAlign align)
{
    const assets::FontStyleRecord* record =
        style(styleName != nullptr ? styleName : m_theme.bodyStyle);
    if (record == nullptr) {
        return;
    }
    m_drawList.addTextInBox(*record, bounds, text, color, align);
}

void UiContext::panel(const Rect& bounds, std::string_view title)
{
    m_drawList.addRoundedRect(bounds, m_theme.radius, m_theme.panel);
    if (title.empty()) {
        return;
    }
    const assets::FontStyleRecord* record = style(m_theme.headingStyle);
    if (record == nullptr) {
        return;
    }
    const Rect titleBox = {{bounds.min.x + m_theme.padding, bounds.min.y + m_theme.padding * 0.5f},
                           {bounds.max.x - m_theme.padding,
                            bounds.min.y + m_theme.padding * 0.5f + record->lineHeight}};
    m_drawList.addTextInBox(*record, titleBox, title, m_theme.textPrimary);
}

bool UiContext::button(const Rect& bounds, std::string_view text, bool enabled)
{
    const Interaction interaction = interact(text, bounds, enabled);
    m_drawList.addRoundedRect(bounds, m_theme.radius, controlColor(interaction, enabled));
    drawFocusRing(bounds, interaction);
    label(bounds, text, enabled ? m_theme.textPrimary : m_theme.textDisabled, nullptr,
          TextAlign::Center);
    return interaction.activated;
}

bool UiContext::checkbox(const Rect& bounds, std::string_view text, bool& value)
{
    const Interaction interaction = interact(text, bounds, true);
    if (interaction.activated) {
        value = !value;
    }

    const float boxSize = std::min(bounds.height(), m_theme.rowHeight) - 8.0f;
    const Rect box = {{bounds.min.x, bounds.min.y + (bounds.height() - boxSize) * 0.5f},
                      {bounds.min.x + boxSize, bounds.min.y + (bounds.height() + boxSize) * 0.5f}};
    m_drawList.addRoundedRect(box, 3.0f, controlColor(interaction, true));
    if (value) {
        m_drawList.addRoundedRect(inset(box, 4.0f), 2.0f, m_theme.accent);
    }
    drawFocusRing(box, interaction);

    const Rect textBox = {{box.max.x + m_theme.spacing, bounds.min.y}, {bounds.max.x, bounds.max.y}};
    label(textBox, text, m_theme.textPrimary);
    return interaction.activated;
}

bool UiContext::slider(const Rect& bounds, std::string_view text, float& value, float minimum,
                       float maximum)
{
    const Interaction interaction = interact(text, bounds, true);
    const float range = maximum - minimum;
    const bool usable = range > 0.0f;

    bool changed = false;
    if (usable) {
        // Dragging sets the value directly from the cursor; the arrow keys
        // step it, so the control is fully usable without a mouse.
        if (interaction.held && m_input.mouseDown) {
            const float fraction = clamp01((m_input.mousePosition.x - bounds.min.x) / bounds.width());
            const float next = minimum + fraction * range;
            changed = next != value;
            value = next;
        } else if (interaction.focused && (m_input.navLeft || m_input.navRight)) {
            const float step = range * 0.05f;
            const float next = std::clamp(value + (m_input.navRight ? step : -step), minimum, maximum);
            changed = next != value;
            value = next;
        }
    }

    const float fraction = usable ? clamp01((value - minimum) / range) : 0.0f;
    const float trackHeight = 6.0f;
    const Rect track = {{bounds.min.x, bounds.min.y + (bounds.height() - trackHeight) * 0.5f},
                        {bounds.max.x, bounds.min.y + (bounds.height() + trackHeight) * 0.5f}};
    m_drawList.addRoundedRect(track, trackHeight * 0.5f, m_theme.meterTrack);
    m_drawList.addRoundedRect({track.min, {track.min.x + track.width() * fraction, track.max.y}},
                              trackHeight * 0.5f, m_theme.accent);

    const float knobRadius = 7.0f;
    const float knobX = track.min.x + track.width() * fraction;
    const Rect knob = {{knobX - knobRadius, bounds.min.y + bounds.height() * 0.5f - knobRadius},
                       {knobX + knobRadius, bounds.min.y + bounds.height() * 0.5f + knobRadius}};
    m_drawList.addRoundedRect(knob, knobRadius, controlColor(interaction, true));
    drawFocusRing(bounds, interaction);
    return changed;
}

bool UiContext::selectable(const Rect& bounds, std::string_view text, bool selected, bool enabled)
{
    const Interaction interaction = interact(text, bounds, enabled);

    if (selected) {
        m_drawList.addRoundedRect(bounds, m_theme.radius, m_theme.controlActive);
    } else if (enabled && interaction.hovered) {
        m_drawList.addRoundedRect(bounds, m_theme.radius, m_theme.controlHovered);
    }
    drawFocusRing(bounds, interaction);

    const Color color = !enabled ? m_theme.textDisabled
                                 : (selected ? m_theme.textPrimary : m_theme.textDim);
    const Rect textBox = {{bounds.min.x + m_theme.spacing, bounds.min.y},
                          {bounds.max.x - m_theme.spacing, bounds.max.y}};
    label(textBox, text, color);
    return interaction.activated;
}

bool UiContext::tabs(const Rect& bounds, std::span<const char* const> labels, int& selected)
{
    if (labels.empty()) {
        return false;
    }
    const int count = static_cast<int>(labels.size());
    selected = std::clamp(selected, 0, count - 1);
    const int previous = selected;

    const float each = (bounds.width() - m_theme.spacing * static_cast<float>(count - 1)) /
                       static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        const char* text = labels[static_cast<std::size_t>(i)];
        const float left = bounds.min.x + (each + m_theme.spacing) * static_cast<float>(i);
        const Rect tab = {{left, bounds.min.y}, {left + each, bounds.max.y}};
        const bool active = i == previous;

        const Interaction interaction = interact(text, tab, true);
        if (interaction.activated) {
            selected = i;
        }
        // Arrows step the strip, so a screen's tabs are reachable without
        // cycling focus through every widget one of them contains. On the edge
        // only: holding the key must not run the whole strip in one frame.
        if (interaction.focused && (m_navLeftEdge || m_navRightEdge)) {
            const int step = m_navRightEdge ? 1 : -1;
            selected = (previous + step + count) % count;
            // Focus rides along, or a second press would step from the old tab
            // again and the strip would never move past its neighbour.
            setFocus(idFor(labels[static_cast<std::size_t>(selected)]));
        }

        m_drawList.addRoundedRect(tab, m_theme.radius,
                                  active ? m_theme.controlActive : controlColor(interaction, true));
        if (active) {
            // An underline keeps the current tab readable even where the
            // active and hovered fills are close in value.
            m_drawList.addRect({{tab.min.x + m_theme.radius, tab.max.y - 2.0f},
                                {tab.max.x - m_theme.radius, tab.max.y}},
                               m_theme.accent);
        }
        drawFocusRing(tab, interaction);
        label(tab, text, active ? m_theme.textPrimary : m_theme.textDim, nullptr, TextAlign::Center);
    }
    return selected != previous;
}

Rect UiContext::beginScroll(const Rect& bounds, float contentHeight, float& offset)
{
    ScrollRegion region;
    region.bounds = bounds;
    region.offset = &offset;
    region.maxOffset = std::max(0.0f, contentHeight - bounds.height());

    if (region.maxOffset > 0.0f && bounds.contains(m_input.mousePosition)) {
        offset -= m_input.scrollDelta * m_theme.rowHeight * 3.0f;
    }
    offset = std::clamp(offset, 0.0f, region.maxOffset);

    m_scrollStack.push_back(region);
    m_drawList.pushClip(bounds);

    // The bar eats width only when there is something to scroll, so a short
    // list is not laid out narrower than a long one for no reason.
    const float barWidth = region.maxOffset > 0.0f ? m_theme.scrollbarWidth + m_theme.spacing : 0.0f;
    const float top = bounds.min.y - offset;
    return {{bounds.min.x, top}, {bounds.max.x - barWidth, top + contentHeight}};
}

void UiContext::endScroll()
{
    if (m_scrollStack.empty()) {
        return;
    }
    const ScrollRegion region = m_scrollStack.back();
    m_scrollStack.pop_back();
    m_drawList.popClip();

    float& offset = *region.offset;
    if (region.hasFocusRect) {
        // Focus rects are in screen space at the offset they were built with;
        // undo it to get content space, then move the window onto them. The
        // correction lands next frame, like the focus change that caused it.
        const float top = region.focusRect.min.y - region.bounds.min.y + offset;
        const float bottom = top + region.focusRect.height();
        if (top < offset) {
            offset = top;
        } else if (bottom > offset + region.bounds.height()) {
            offset = bottom - region.bounds.height();
        }
        offset = std::clamp(offset, 0.0f, region.maxOffset);
    }

    if (region.maxOffset <= 0.0f) {
        return;
    }
    const Rect track = {{region.bounds.max.x - m_theme.scrollbarWidth, region.bounds.min.y},
                        {region.bounds.max.x, region.bounds.max.y}};
    m_drawList.addRoundedRect(track, m_theme.scrollbarWidth * 0.5f, m_theme.meterTrack);

    const float viewFraction =
        region.bounds.height() / (region.bounds.height() + region.maxOffset);
    const float thumbHeight = std::max(track.height() * viewFraction, m_theme.scrollbarWidth * 2.0f);
    const float travel = track.height() - thumbHeight;
    const float thumbTop = track.min.y + travel * (offset / region.maxOffset);
    m_drawList.addRoundedRect({{track.min.x, thumbTop}, {track.max.x, thumbTop + thumbHeight}},
                              m_theme.scrollbarWidth * 0.5f, m_theme.controlHovered);
}

void UiContext::meter(const Rect& bounds, float fraction, const Color& fill)
{
    const float radius = bounds.height() * 0.5f;
    m_drawList.addRoundedRect(bounds, radius, m_theme.meterTrack);
    const float clamped = clamp01(fraction);
    if (clamped <= 0.0f) {
        return;
    }
    // Keep the filled end round rather than a sliver when nearly empty.
    const float width = std::max(bounds.width() * clamped, bounds.height());
    m_drawList.addRoundedRect({bounds.min, {bounds.min.x + width, bounds.max.y}}, radius, fill);
}

} // namespace sol::ui
