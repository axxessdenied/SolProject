#pragma once

#include "sol/ui/draw_list.hpp"
#include "sol/ui/layout.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sol::ui {

// Widget identity, hashed from the id stack so the same label in two panels
// never collides. Zero means "no widget".
using WidgetId = std::uint64_t;
inline constexpr WidgetId kNoWidget = 0;

// What the player did this frame. The game fills this from the platform
// layer; the UI never reads input hardware itself.
struct InputState
{
    core::Vec2 mousePosition;
    bool mouseDown = false;     // held right now
    bool mousePressed = false;  // went down this frame
    bool mouseReleased = false; // came up this frame
    float scrollDelta = 0.0f;

    // Keyboard navigation. Every screen must be fully operable through these
    // alone - that is a requirement of the phase, and the groundwork for
    // gamepad support later.
    bool navNext = false;
    bool navPrevious = false;
    bool navActivate = false;
    bool navCancel = false;
    bool navLeft = false;
    bool navRight = false;

    // Text entry (Phase 8h). `text` is the UTF-8 typed this frame, straight
    // from Window::textInput(); the rest are key EDGES, because a caret that
    // moved once per frame while a key was held would be unusable.
    std::string_view text;
    bool editLeft = false;
    bool editRight = false;
    bool editHome = false;
    bool editEnd = false;
    bool editBackspace = false;
    bool editDelete = false;
    // Enter ALONE, unlike navActivate which is Enter-or-Space. A prompt with
    // a text field in it cannot accept on Space: that is a character.
    bool editSubmit = false;
};

// Colors, metrics, and font style names in one place, so a restyle is a data
// change rather than a hunt through widget code.
struct Theme
{
    Color background = rgba(0x0A0E13F0u);
    Color panel = rgba(0x0E141ACCu);
    Color panelEdge = rgba(0x22303EFFu);
    Color control = rgba(0x1B2733FFu);
    Color controlHovered = rgba(0x27384AFFu);
    Color controlActive = rgba(0x35506AFFu);
    Color controlDisabled = rgba(0x161E27FFu);
    Color accent = rgba(0x58C8F0FFu);
    Color textPrimary = rgba(0xE8F0F8FFu);
    Color textDim = rgba(0x8FA6BCFFu);
    Color textDisabled = rgba(0x53616EFFu);
    Color focusRing = rgba(0x58C8F0FFu);
    Color meterTrack = rgba(0x1E2A36FFu);
    Color positive = rgba(0x69C48CFFu);
    Color negative = rgba(0xE0704CFFu);

    float padding = 12.0f;
    float spacing = 6.0f;
    float rowHeight = 28.0f;
    float radius = 6.0f;
    float focusRingThickness = 2.0f;
    float scrollbarWidth = 10.0f;

    // Style names as cooked into the font asset.
    const char* headingStyle = "heading";
    const char* bodyStyle = "body";
    const char* strongStyle = "body_strong";
    const char* smallStyle = "hud";
};

// Immediate-mode UI with retained hover/active/focus - engine plan §2.9's
// "retained-or-immediate hybrid". Widgets take explicit rectangles and report
// what the player did, which keeps the game's existing fill-then-execute
// pattern intact.
//
// Holds no GPU state: everything lands in the DrawList, so whole screens can
// be built and asserted on headlessly.
class UiContext
{
public:
    void setFont(const assets::Font* font, std::uint32_t fontTexture);
    void setTheme(const Theme& theme) { m_theme = theme; }
    [[nodiscard]] const Theme& theme() const { return m_theme; }

    // `deltaSeconds` drives only the caret blink; zero (the default) leaves
    // the caret solid, which is what the headless tests want.
    void beginFrame(const InputState& input, core::Vec2 screenSize, float deltaSeconds = 0.0f);
    void endFrame();

    [[nodiscard]] DrawList& drawList() { return m_drawList; }
    [[nodiscard]] const DrawList& drawList() const { return m_drawList; }
    [[nodiscard]] const InputState& input() const { return m_input; }
    [[nodiscard]] core::Vec2 screenSize() const { return m_screenSize; }

    // Identity scoping. Push a panel name (or a row index) before building
    // repeated widgets so their ids stay distinct.
    void pushId(std::string_view name);
    void pushId(int index);
    void popId();
    [[nodiscard]] WidgetId idFor(std::string_view label) const;

    [[nodiscard]] WidgetId focused() const { return m_focusId; }
    void setFocus(WidgetId id) { m_focusId = id; }
    [[nodiscard]] bool isFocused(WidgetId id) const { return id != kNoWidget && id == m_focusId; }

    // True on the frame the player dismissed the current screen (Esc, or the
    // cancel button on a pad).
    [[nodiscard]] bool cancelRequested() const { return m_input.navCancel; }
    // Enter alone; safe to use as "confirm" on a screen that also has a text
    // field, where Space has to stay a character.
    [[nodiscard]] bool submitRequested() const { return m_input.editSubmit; }

    // --- Widgets. Each returns whether the player acted on it this frame. ---

    void label(const Rect& bounds, std::string_view text, TextAlign align = TextAlign::Left);
    void label(const Rect& bounds, std::string_view text, const Color& color,
               const char* styleName = nullptr, TextAlign align = TextAlign::Left);

    void panel(const Rect& bounds, std::string_view title = {});

    [[nodiscard]] bool button(const Rect& bounds, std::string_view label, bool enabled = true);
    [[nodiscard]] bool checkbox(const Rect& bounds, std::string_view label, bool& value);
    [[nodiscard]] bool slider(const Rect& bounds, std::string_view label, float& value, float minimum,
                              float maximum);
    // A selectable row (list entries, tabs when drawn as a strip).
    [[nodiscard]] bool selectable(const Rect& bounds, std::string_view label, bool selected,
                                  bool enabled = true);

    // An editable single-line field (Phase 8h). `id` names the widget - the
    // value is what the player is typing, so it cannot double as the label
    // the way every other widget's text does. Returns true on the frames the
    // value changed. Insertion and deletion only: no selection ranges, no
    // clipboard, no IME.
    //
    // While focused it swallows the nav keys it uses, so typing "s" into a
    // field does not also step the list behind it.
    [[nodiscard]] bool textField(const Rect& bounds, std::string_view id, std::string& value,
                                 std::size_t maxLength = 48);
    // Where the caret sits in the focused field, in bytes. Screens that open a
    // field prefilled want it at the end rather than at the start.
    void setCaret(std::size_t bytes) { m_caret = bytes; }
    // True while a text field holds keyboard focus. Screens use it to keep
    // their own key handling (Esc, Enter, single-letter shortcuts) out of the
    // way of typing.
    [[nodiscard]] bool editingText() const { return m_textFieldFocusedLastFrame; }

    // A tab strip splitting `bounds` evenly. `selected` indexes `labels` and is
    // updated in place; left/right step it while a tab holds focus, so moving
    // between tabs costs one arrow key rather than a full nav cycle. Returns
    // true on the frame the selection changed.
    [[nodiscard]] bool tabs(const Rect& bounds, std::span<const char* const> labels, int& selected);

    void meter(const Rect& bounds, float fraction, const Color& fill);

    // --- Scroll region ---
    //
    // Clips everything built between the calls to `bounds` and shifts it up by
    // `offset`, which the caller owns so a screen keeps its place across
    // frames. Returns the rectangle to lay the content out in: its top is the
    // scrolled origin and its height is `contentHeight`, so a Column built over
    // it needs no offset arithmetic of its own.
    //
    // The wheel scrolls while the cursor is inside, and endScroll() pulls the
    // focused widget back into view - a row reached by keyboard has to be a row
    // the player can see.
    [[nodiscard]] Rect beginScroll(const Rect& bounds, float contentHeight, float& offset);
    void endScroll();

    // Number of interactive widgets built this frame; the nav order.
    [[nodiscard]] std::size_t interactiveCount() const { return m_navItems.size(); }

    // Widgets activated this frame, by click or by nav. Reset every frame.
    // The caller decides what a press sounds like; sol::ui does not know that
    // audio exists, and this is the one seam it needs in order to stay that way.
    [[nodiscard]] std::uint32_t activationsThisFrame() const { return m_activations; }

private:
    // Shared hit-testing and state bookkeeping for every interactive widget.
    struct Interaction
    {
        WidgetId id = kNoWidget;
        bool hovered = false;
        bool held = false;
        bool activated = false;
        bool focused = false;
    };

    // An open scroll region: where it draws, whose offset it moves, and the
    // focused widget inside it (if any), which endScroll() scrolls into view.
    struct ScrollRegion
    {
        Rect bounds;
        float* offset = nullptr;
        float maxOffset = 0.0f;
        Rect focusRect;
        bool hasFocusRect = false;
    };

    [[nodiscard]] Interaction interact(std::string_view label, const Rect& bounds, bool enabled);
    [[nodiscard]] Color controlColor(const Interaction& interaction, bool enabled) const;
    void drawFocusRing(const Rect& bounds, const Interaction& interaction);
    [[nodiscard]] const assets::FontStyleRecord* style(const char* name) const;

    DrawList m_drawList;
    const assets::Font* m_font = nullptr;
    Theme m_theme;

    InputState m_input;
    core::Vec2 m_screenSize;

    std::vector<WidgetId> m_idStack;
    std::vector<WidgetId> m_navItems; // interactive widgets, in build order
    std::vector<ScrollRegion> m_scrollStack;

    // Left/right arrive held (a slider steps while the key is down); discrete
    // controls like the tab strip need the edge, which is derived here rather
    // than asked of every caller.
    bool m_previousNavLeft = false;
    bool m_previousNavRight = false;
    bool m_navLeftEdge = false;
    bool m_navRightEdge = false;
    std::uint32_t m_activations = 0; // widgets pressed this frame

    WidgetId m_hotId = kNoWidget;    // under the cursor
    WidgetId m_activeId = kNoWidget; // being pressed
    WidgetId m_focusId = kNoWidget;  // keyboard focus
    // Caret position, in bytes, within whichever field holds focus. One
    // caret is enough because only one field can be focused at a time.
    std::size_t m_caret = 0;
    WidgetId m_caretField = kNoWidget; // whose caret m_caret is
    float m_caretBlink = 0.0f;
    // Whether focus is in a text field. Set as the field builds, and carried
    // to the next frame because widgets that also want the arrows (the tab
    // strip, sliders) are often built BEFORE the field in the same frame.
    // Focus persists across frames, so last frame's answer is the right one.
    bool m_textFieldFocused = false;
    bool m_textFieldFocusedLastFrame = false;
    bool m_frameOpen = false;
};

} // namespace sol::ui
