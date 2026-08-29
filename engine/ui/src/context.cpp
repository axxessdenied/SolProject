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

void UiContext::beginFrame(const InputState& input, core::Vec2 screenSize, float deltaSeconds)
{
    // Caret blink runs on wall time, not frames: at 400 fps a frame-counted
    // blink is a flicker. Zero dt (the default, which the headless tests use)
    // leaves the caret solid.
    m_caretBlink = deltaSeconds > 0.0f ? m_caretBlink + deltaSeconds : 0.0f;
    m_navLeftEdge = input.navLeft && !m_previousNavLeft;
    m_navRightEdge = input.navRight && !m_previousNavRight;
    m_previousNavLeft = input.navLeft;
    m_previousNavRight = input.navRight;

    m_input = input;
    m_screenSize = screenSize;
    m_activations = 0;
    m_drawList.reset();
    m_idStack.clear();
    m_navItems.clear();
    m_scrollStack.clear();
    m_hotId = kNoWidget;
    m_textFieldFocusedLastFrame = m_textFieldFocused;
    m_textFieldFocused = false;
    m_frameOpen = true;
}

void UiContext::endFrame()
{
    // First, because it is the last thing drawn: the DrawList batches in call
    // order and has no z-order, so a tooltip queued during a list would be
    // overdrawn by whatever the screen builds after it. By here every widget
    // is behind us and the clip stack is empty, so this lands on top by
    // construction rather than by a layer the DrawList does not have.
    drawTooltip();

    // Focus moves at the end of the frame, when the full widget order is
    // known; the change lands on the next build, which is invisible at frame
    // rate and keeps nav independent of where the key was pressed.
    if (!m_navItems.empty() && (m_input.navNext || m_input.navPrevious)) {
        const auto found = std::find(m_navItems.begin(), m_navItems.end(), m_focusId);
        const std::size_t count = m_navItems.size();
        std::size_t index =
            found == m_navItems.end() ? count : static_cast<std::size_t>(found - m_navItems.begin());
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
    if (interaction.activated) {
        // Every widget's activation funnels through here, so one counter is
        // enough for the caller to know a control was pressed - which is all
        // a click cue needs, and it keeps sol::ui unaware that audio exists.
        ++m_activations;
    }
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

void UiContext::label(
    const Rect& bounds, std::string_view text, const Color& color, const char* styleName, TextAlign align)
{
    const assets::FontStyleRecord* record = style(styleName != nullptr ? styleName : m_theme.bodyStyle);
    if (record == nullptr) {
        return;
    }
    m_drawList.addTextInBox(*record, bounds, text, color, align);
}

bool UiContext::labelElided(const Rect& bounds,
                            std::string_view text,
                            const Color& color,
                            const char* styleName)
{
    const assets::FontStyleRecord* record = style(styleName != nullptr ? styleName : m_theme.bodyStyle);
    if (record == nullptr || m_font == nullptr) {
        return false;
    }
    const float available = bounds.width();
    if (text.empty() || m_font->measureWidth(*record, text) <= available) {
        m_drawList.addTextInBox(*record, bounds, text, color);
        return false;
    }

    static constexpr std::string_view kEllipsis = "...";
    // Three periods rather than U+2026: the font falls back to '?' for any
    // codepoint it was not baked with, and a horizontal ellipsis is exactly
    // the sort of glyph a subset atlas leaves out.
    const float ellipsisWidth = m_font->measureWidth(*record, kEllipsis);
    if (ellipsisWidth > available) {
        // Not even room to say it was cut. Drawing anyway would put the
        // overflow back, which is the defect this exists to remove.
        return true;
    }

    // Cut whole codepoints, never bytes: a truncation landing mid-sequence
    // decodes to U+FFFD and draws as '?', so a long name would come out
    // looking corrupted rather than long. Summing per-codepoint widths is
    // exact here because measureWidth applies no kerning - see its header.
    std::size_t cursor = 0;
    std::size_t fits = 0;
    float width = 0.0f;
    while (cursor < text.size()) {
        const std::size_t start = cursor;
        (void)assets::nextCodepoint(text, cursor);
        width += m_font->measureWidth(*record, text.substr(start, cursor - start));
        if (width + ellipsisWidth > available) {
            break;
        }
        fits = cursor;
    }
    std::string cut(text.substr(0, fits));
    cut += kEllipsis;
    m_drawList.addTextInBox(*record, bounds, cut, color);
    return true;
}

void UiContext::tooltip(std::string_view text)
{
    if (!text.empty()) {
        m_tooltip.assign(text);
    }
}

void UiContext::drawTooltip()
{
    if (m_tooltip.empty()) {
        return;
    }
    const assets::FontStyleRecord* record = style(m_theme.smallStyle);
    if (record == nullptr || m_font == nullptr) {
        m_tooltip.clear();
        return;
    }
    const float width = m_font->measureWidth(*record, m_tooltip) + m_theme.padding;
    const float height = m_theme.rowHeight;
    // Below and right of the cursor so it does not sit under the hand, then
    // pushed back inside the screen rather than off it - a tooltip that
    // explains a name it has itself scrolled out of view explains nothing.
    core::Vec2 min{m_input.mousePosition.x + 14.0f, m_input.mousePosition.y + 18.0f};
    min.x = std::max(0.0f, std::min(min.x, m_screenSize.x - width));
    min.y = std::max(0.0f, std::min(min.y, m_screenSize.y - height));
    const Rect box{min, {min.x + width, min.y + height}};
    m_drawList.addRoundedRect(box, m_theme.radius, m_theme.control);
    m_drawList.addRectOutline(box, m_theme.panelEdge);
    const float inset = m_theme.padding * 0.5f;
    m_drawList.addTextInBox(*record,
                            {{box.min.x + inset, box.min.y}, {box.max.x - inset, box.max.y}},
                            m_tooltip,
                            m_theme.textPrimary);
    m_tooltip.clear();
}

int UiContext::contextMenu(core::Vec2 anchor, std::span<const MenuItem> items, Rect* boundsOut)
{
    if (boundsOut != nullptr) {
        *boundsOut = Rect{};
    }
    if (items.empty()) {
        return -1;
    }
    const assets::FontStyleRecord* record = style(m_theme.bodyStyle);
    if (record == nullptr || m_font == nullptr) {
        return -1;
    }

    // Sized from the widest row, so no label is ever elided in a menu the
    // player opened to read. The panel that would not fit is clamped onto the
    // screen below, not shrunk.
    float widest = 0.0f;
    for (const MenuItem& item : items) {
        widest = std::max(widest, m_font->measureWidth(*record, item.label));
    }
    const float width = widest + m_theme.padding * 4.0f;
    const float rows = static_cast<float>(items.size());
    const float height = rows * m_theme.rowHeight + (rows - 1.0f) * m_theme.spacing + m_theme.padding * 2.0f;

    // Below and right of the anchor, then pushed back inside the screen — the
    // same rule drawTooltip uses, and for the same reason: a menu that opens
    // half off the edge is a menu with unreachable entries.
    core::Vec2 min{anchor.x + 2.0f, anchor.y + 2.0f};
    min.x = std::max(0.0f, std::min(min.x, m_screenSize.x - width));
    min.y = std::max(0.0f, std::min(min.y, m_screenSize.y - height));
    const Rect box{min, {min.x + width, min.y + height}};
    if (boundsOut != nullptr) {
        *boundsOut = box;
    }

    panel(box);

    // ⚑ Pushed as its own id scope so a menu row's identity cannot collide with
    // a button of the same label on the screen underneath it — which is not
    // hypothetical here, since a menu's entries are named after actions the
    // HUD and the station screens also name.
    pushId("context_menu");
    int activated = -1;
    float y = box.min.y + m_theme.padding;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const MenuItem& item = items[i];
        const Rect row{{box.min.x + m_theme.padding, y},
                       {box.max.x - m_theme.padding, y + m_theme.rowHeight}};
        pushId(static_cast<int>(i));
        if (button(row, item.label, item.enabled)) {
            activated = static_cast<int>(i);
        }
        // The reason a row is unavailable is worth more than the row, and the
        // tooltip facility already puts it where the cursor is.
        if (!item.enabled && !item.reason.empty() && row.contains(m_input.mousePosition)) {
            tooltip(item.reason);
        }
        popId();
        y += m_theme.rowHeight + m_theme.spacing;
    }
    popId();
    return activated;
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
    const Rect titleBox = {
        {bounds.min.x + m_theme.padding, bounds.min.y + m_theme.padding * 0.5f},
        {bounds.max.x - m_theme.padding, bounds.min.y + m_theme.padding * 0.5f + record->lineHeight}};
    m_drawList.addTextInBox(*record, titleBox, title, m_theme.textPrimary);
}

bool UiContext::button(const Rect& bounds, std::string_view text, bool enabled)
{
    const Interaction interaction = interact(text, bounds, enabled);
    m_drawList.addRoundedRect(bounds, m_theme.radius, controlColor(interaction, enabled));
    drawFocusRing(bounds, interaction);
    label(bounds, text, enabled ? m_theme.textPrimary : m_theme.textDisabled, nullptr, TextAlign::Center);
    return interaction.activated;
}

namespace {

// UTF-8 boundaries: a continuation byte is 10xxxxxx, so the caret steps over
// whole code points rather than splitting one and drawing a broken glyph.
[[nodiscard]] bool isContinuation(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u;
}

[[nodiscard]] std::size_t previousBoundary(std::string_view text, std::size_t at)
{
    if (at == 0) {
        return 0;
    }
    std::size_t index = at - 1;
    while (index > 0 && isContinuation(text[index])) {
        --index;
    }
    return index;
}

[[nodiscard]] std::size_t nextBoundary(std::string_view text, std::size_t at)
{
    if (at >= text.size()) {
        return text.size();
    }
    std::size_t index = at + 1;
    while (index < text.size() && isContinuation(text[index])) {
        ++index;
    }
    return index;
}

} // namespace

bool UiContext::textField(const Rect& bounds, std::string_view id, std::string& value, std::size_t maxLength)
{
    const Interaction interaction = interact(id, bounds, true);
    // A missing style stops the field being DRAWN, further down; it must not
    // stop it being edited, or a theme naming a style the font lacks would
    // silently turn a field read-only.
    const assets::FontStyleRecord* record = style(m_theme.bodyStyle);

    // The caret belongs to whichever field holds focus; adopting it on focus
    // change keeps one cursor for the whole UI rather than one per field.
    if (m_caretField != interaction.id && interaction.focused) {
        m_caretField = interaction.id;
        m_caret = value.size();
    }
    if (m_caret > value.size()) {
        m_caret = value.size();
    }

    bool changed = false;
    const bool editing = interaction.focused;
    if (editing) {
        m_textFieldFocused = true;
        // Click inside places the caret at the nearest boundary to the
        // cursor, walking the string once and measuring each prefix.
        if (interaction.hovered && m_input.mousePressed && m_font != nullptr && record != nullptr) {
            const float target = m_input.mousePosition.x - (bounds.min.x + m_theme.padding * 0.5f);
            std::size_t best = 0;
            float bestDistance = std::abs(target);
            for (std::size_t at = nextBoundary(value, 0); at <= value.size(); at = nextBoundary(value, at)) {
                const float width = m_font->measureWidth(*record, std::string_view(value).substr(0, at));
                const float distance = std::abs(target - width);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = at;
                }
                if (at == value.size()) {
                    break;
                }
            }
            m_caret = best;
        }

        if (m_input.editLeft) {
            m_caret = previousBoundary(value, m_caret);
        }
        if (m_input.editRight) {
            m_caret = nextBoundary(value, m_caret);
        }
        if (m_input.editHome) {
            m_caret = 0;
        }
        if (m_input.editEnd) {
            m_caret = value.size();
        }
        if (m_input.editBackspace && m_caret > 0) {
            const std::size_t from = previousBoundary(value, m_caret);
            value.erase(from, m_caret - from);
            m_caret = from;
            changed = true;
        }
        if (m_input.editDelete && m_caret < value.size()) {
            value.erase(m_caret, nextBoundary(value, m_caret) - m_caret);
            changed = true;
        }
        if (!m_input.text.empty() && value.size() < maxLength) {
            // maxLength is a byte budget; truncate on a boundary so a
            // multi-byte character is never cut in half at the limit.
            std::string_view incoming = m_input.text;
            const std::size_t room = maxLength - value.size();
            if (incoming.size() > room) {
                std::size_t keep = 0;
                while (nextBoundary(incoming, keep) <= room) {
                    keep = nextBoundary(incoming, keep);
                    if (keep == incoming.size()) {
                        break;
                    }
                }
                incoming = incoming.substr(0, keep);
            }
            if (!incoming.empty()) {
                value.insert(m_caret, incoming);
                m_caret += incoming.size();
                changed = true;
            }
        }
        if (changed) {
            m_caretBlink = 0.0f; // typing shows the caret rather than hiding it
        }
    }

    // A field reads as editable: a sunken well rather than a raised control.
    m_drawList.addRoundedRect(bounds, m_theme.radius, editing ? m_theme.controlActive : m_theme.control);
    drawFocusRing(bounds, interaction);
    if (record == nullptr) {
        return changed;
    }

    const float inset = m_theme.padding * 0.5f;
    const Rect inner = {{bounds.min.x + inset, bounds.min.y}, {bounds.max.x - inset, bounds.max.y}};
    // Scroll the text so the caret stays visible once the value outruns the
    // box; without this a long name types itself off the right edge.
    float caretX = 0.0f;
    float textWidth = 0.0f;
    if (m_font != nullptr) {
        caretX = m_font->measureWidth(*record, std::string_view(value).substr(0, m_caret));
        textWidth = m_font->measureWidth(*record, value);
    }
    float offset = 0.0f;
    if (textWidth > inner.width()) {
        offset = std::min(textWidth - inner.width(), std::max(0.0f, caretX - inner.width() + 6.0f));
    }

    m_drawList.pushClip(inner);
    m_drawList.addTextInBox(
        *record, {{inner.min.x - offset, inner.min.y}, inner.max}, value, m_theme.textPrimary);
    if (editing) {
        // Half a second on, half off; a solid caret when dt is not supplied.
        const bool visible = m_caretBlink <= 0.0f || std::fmod(m_caretBlink, 1.0f) < 0.5f;
        if (visible) {
            const float x = inner.min.x + caretX - offset;
            const float pad = 4.0f;
            m_drawList.addRect({{x, bounds.min.y + pad}, {x + 1.5f, bounds.max.y - pad}},
                               m_theme.textPrimary);
        }
    }
    m_drawList.popClip();
    return changed;
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

bool UiContext::slider(const Rect& bounds, std::string_view text, float& value, float minimum, float maximum)
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
                              trackHeight * 0.5f,
                              m_theme.accent);

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

    const Color color = !enabled ? m_theme.textDisabled : (selected ? m_theme.textPrimary : m_theme.textDim);
    const Rect textBox = {{bounds.min.x + m_theme.spacing, bounds.min.y},
                          {bounds.max.x - m_theme.spacing, bounds.max.y}};
    // A row that had to hide part of a name owes the player the whole of it
    // (Phase 10). Only then: a tooltip repeating a name already fully on
    // screen is noise on every row of every list.
    if (labelElided(textBox, text, color) && interaction.hovered) {
        tooltip(text);
    }
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

    const float each =
        (bounds.width() - m_theme.spacing * static_cast<float>(count - 1)) / static_cast<float>(count);
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
        // Not while a text field is being typed into - there the arrows are
        // the caret's.
        if (interaction.focused && !m_textFieldFocusedLastFrame && (m_navLeftEdge || m_navRightEdge)) {
            const int step = m_navRightEdge ? 1 : -1;
            selected = (previous + step + count) % count;
            // Focus rides along, or a second press would step from the old tab
            // again and the strip would never move past its neighbour.
            setFocus(idFor(labels[static_cast<std::size_t>(selected)]));
        }

        m_drawList.addRoundedRect(
            tab, m_theme.radius, active ? m_theme.controlActive : controlColor(interaction, true));
        if (active) {
            // An underline keeps the current tab readable even where the
            // active and hovered fills are close in value.
            m_drawList.addRect(
                {{tab.min.x + m_theme.radius, tab.max.y - 2.0f}, {tab.max.x - m_theme.radius, tab.max.y}},
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

    const float viewFraction = region.bounds.height() / (region.bounds.height() + region.maxOffset);
    const float thumbHeight = std::max(track.height() * viewFraction, m_theme.scrollbarWidth * 2.0f);
    const float travel = track.height() - thumbHeight;
    const float thumbTop = track.min.y + travel * (offset / region.maxOffset);
    m_drawList.addRoundedRect({{track.min.x, thumbTop}, {track.max.x, thumbTop + thumbHeight}},
                              m_theme.scrollbarWidth * 0.5f,
                              m_theme.controlHovered);
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
