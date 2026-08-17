#include <sol/ui/context.hpp>

#include <sol/test/synthetic_cooked_font.hpp>
#include <sol/test/test.hpp>

#include <cstdint>
#include <span>
#include <vector>

using sol::assets::Font;
using sol::ui::InputState;
using sol::ui::Rect;
using sol::ui::UiContext;
using sol::ui::WidgetId;

namespace {

constexpr sol::core::Vec2 kScreen = {1280.0f, 720.0f};

// Two buttons stacked, used by most of the interaction tests.
constexpr Rect kFirst = {{100.0f, 100.0f}, {300.0f, 140.0f}};
constexpr Rect kSecond = {{100.0f, 160.0f}, {300.0f, 200.0f}};

InputState mouseAt(float x, float y)
{
    InputState input;
    input.mousePosition = {x, y};
    return input;
}

InputState pressAt(float x, float y)
{
    InputState input = mouseAt(x, y);
    input.mouseDown = true;
    input.mousePressed = true;
    return input;
}

InputState releaseAt(float x, float y)
{
    InputState input = mouseAt(x, y);
    input.mouseReleased = true;
    return input;
}

// Runs one frame of two buttons; returns which one fired.
struct TwoButtons
{
    bool first = false;
    bool second = false;
};

TwoButtons runTwoButtons(UiContext& ui, const InputState& input)
{
    ui.beginFrame(input, kScreen);
    TwoButtons result;
    result.first = ui.button(kFirst, "Launch");
    result.second = ui.button(kSecond, "Cancel");
    ui.endFrame();
    return result;
}

bool loadFont(Font& font)
{
    return font.loadFromMemory(sol::test::buildSyntheticCookedFont());
}

} // namespace

SOL_TEST(ui_context_ids_are_scoped_by_the_stack)
{
    UiContext ui;
    const WidgetId bare = ui.idFor("Accept");

    ui.pushId("board");
    const WidgetId inBoard = ui.idFor("Accept");
    ui.pushId(3);
    const WidgetId inRow3 = ui.idFor("Accept");
    ui.popId();
    ui.pushId(4);
    const WidgetId inRow4 = ui.idFor("Accept");
    ui.popId();
    ui.popId();

    // The same label in different scopes must never collide, or the buttons
    // on two rows would share one piece of state.
    SOL_CHECK(bare != inBoard);
    SOL_CHECK(inBoard != inRow3);
    SOL_CHECK(inRow3 != inRow4);

    // Leaving a scope restores the previous identity exactly.
    SOL_CHECK(ui.idFor("Accept") == bare);

    // No widget may ever be handed the reserved "nothing" id.
    SOL_CHECK(bare != sol::ui::kNoWidget);
    SOL_CHECK(inRow3 != sol::ui::kNoWidget);

    // Popping an empty stack must not underflow.
    ui.popId();
    SOL_CHECK(ui.idFor("Accept") == bare);
}

SOL_TEST(ui_context_button_fires_on_press_and_release_together)
{
    UiContext ui;

    // Hovering alone does nothing.
    SOL_CHECK(!runTwoButtons(ui, mouseAt(200.0f, 120.0f)).first);
    // Pressing alone does nothing: the click completes on release.
    SOL_CHECK(!runTwoButtons(ui, pressAt(200.0f, 120.0f)).first);
    // Release over the same widget fires it, and only it.
    const TwoButtons released = runTwoButtons(ui, releaseAt(200.0f, 120.0f));
    SOL_CHECK(released.first);
    SOL_CHECK(!released.second);
}

SOL_TEST(ui_context_drag_off_a_button_cancels_the_click)
{
    UiContext ui;
    SOL_CHECK(!runTwoButtons(ui, pressAt(200.0f, 120.0f)).first);

    // Releasing somewhere else must not fire the widget the press started on -
    // dragging off a button is how players change their mind.
    const TwoButtons elsewhere = runTwoButtons(ui, releaseAt(800.0f, 600.0f));
    SOL_CHECK(!elsewhere.first);
    SOL_CHECK(!elsewhere.second);

    // And the press state is not left dangling for the next click.
    SOL_CHECK(!runTwoButtons(ui, releaseAt(200.0f, 120.0f)).first);
}

SOL_TEST(ui_context_release_over_a_different_widget_fires_nothing)
{
    UiContext ui;
    SOL_CHECK(!runTwoButtons(ui, pressAt(200.0f, 120.0f)).first);

    const TwoButtons crossed = runTwoButtons(ui, releaseAt(200.0f, 180.0f));
    SOL_CHECK(!crossed.first);
    SOL_CHECK(!crossed.second);
}

SOL_TEST(ui_context_keyboard_navigation_cycles_widgets)
{
    UiContext ui;
    InputState next;
    next.navNext = true;

    // Nothing is focused until the player asks for it.
    runTwoButtons(ui, InputState{});
    SOL_CHECK(ui.focused() == sol::ui::kNoWidget);

    // First Tab enters the screen at the first widget.
    runTwoButtons(ui, next);
    const WidgetId first = ui.focused();
    SOL_CHECK(first != sol::ui::kNoWidget);

    runTwoButtons(ui, next);
    const WidgetId second = ui.focused();
    SOL_CHECK(second != first);

    // Cycling wraps rather than dead-ending.
    runTwoButtons(ui, next);
    SOL_CHECK(ui.focused() == first);

    // And it runs backwards too.
    InputState previous;
    previous.navPrevious = true;
    runTwoButtons(ui, previous);
    SOL_CHECK(ui.focused() == second);
}

SOL_TEST(ui_context_activate_fires_the_focused_widget)
{
    UiContext ui;
    InputState next;
    next.navNext = true;
    runTwoButtons(ui, next);
    runTwoButtons(ui, next); // focus the second button

    InputState activate;
    activate.navActivate = true;
    const TwoButtons result = runTwoButtons(ui, activate);
    SOL_CHECK(!result.first);
    SOL_CHECK(result.second);
}

SOL_TEST(ui_context_clicking_takes_keyboard_focus)
{
    UiContext ui;
    runTwoButtons(ui, pressAt(200.0f, 180.0f));

    // A mouse press moves keyboard focus, so Tab continues from where the
    // player was rather than jumping back to the top.
    const WidgetId clicked = ui.focused();
    SOL_CHECK(clicked != sol::ui::kNoWidget);
    runTwoButtons(ui, releaseAt(200.0f, 180.0f));

    InputState next;
    next.navNext = true;
    runTwoButtons(ui, next);
    SOL_CHECK(ui.focused() != clicked);
}

SOL_TEST(ui_context_disabled_widgets_do_not_respond_or_take_focus)
{
    UiContext ui;

    const auto frame = [&](const InputState& input) {
        ui.beginFrame(input, kScreen);
        const bool fired = ui.button(kFirst, "Launch", false);
        ui.endFrame();
        return fired;
    };

    SOL_CHECK(!frame(pressAt(200.0f, 120.0f)));
    SOL_CHECK(!frame(releaseAt(200.0f, 120.0f)));
    SOL_CHECK(ui.interactiveCount() == 0);

    // Tab must skip it rather than parking focus on a dead end.
    InputState next;
    next.navNext = true;
    frame(next);
    SOL_CHECK(ui.focused() == sol::ui::kNoWidget);
}

SOL_TEST(ui_context_focus_clears_when_the_widget_disappears)
{
    UiContext ui;
    InputState next;
    next.navNext = true;
    runTwoButtons(ui, next);
    SOL_CHECK(ui.focused() != sol::ui::kNoWidget);

    // A screen change removes the focused widget; keeping focus on it would
    // let a later Enter fire something invisible.
    ui.beginFrame(InputState{}, kScreen);
    (void)ui.button({{0.0f, 0.0f}, {50.0f, 20.0f}}, "Something Else");
    ui.endFrame();
    SOL_CHECK(ui.focused() == sol::ui::kNoWidget);
}

SOL_TEST(ui_context_checkbox_toggles_once_per_activation)
{
    UiContext ui;
    bool value = false;

    const auto frame = [&](const InputState& input) {
        ui.beginFrame(input, kScreen);
        const bool changed = ui.checkbox(kFirst, "Hardcore", value);
        ui.endFrame();
        return changed;
    };

    SOL_CHECK(!frame(pressAt(110.0f, 120.0f)));
    SOL_CHECK(!value);
    SOL_CHECK(frame(releaseAt(110.0f, 120.0f)));
    SOL_CHECK(value);

    // Holding the cursor there must not keep toggling frame after frame.
    SOL_CHECK(!frame(mouseAt(110.0f, 120.0f)));
    SOL_CHECK(value);

    SOL_CHECK(frame(pressAt(110.0f, 120.0f)) == false);
    SOL_CHECK(frame(releaseAt(110.0f, 120.0f)));
    SOL_CHECK(!value);
}

SOL_TEST(ui_context_slider_tracks_the_cursor_and_clamps)
{
    UiContext ui;
    float value = 0.0f;

    const auto drag = [&](float x) {
        InputState input = mouseAt(x, 120.0f);
        input.mouseDown = true;
        ui.beginFrame(input, kScreen);
        const bool changed = ui.slider(kFirst, "Volume", value, 0.0f, 100.0f);
        ui.endFrame();
        return changed;
    };

    // Grabbing already sets the value from where the cursor landed, which is
    // what players expect from clicking a track.
    ui.beginFrame(pressAt(150.0f, 120.0f), kScreen);
    (void)ui.slider(kFirst, "Volume", value, 0.0f, 100.0f);
    ui.endFrame();
    SOL_CHECK(value > 24.0f && value < 26.0f); // a quarter along a 200 px track

    SOL_CHECK(drag(200.0f));
    SOL_CHECK(value > 49.0f && value < 51.0f); // dragged to halfway

    // Holding still reports no change, so callers can treat the return as an
    // edge rather than a level.
    SOL_CHECK(!drag(200.0f));

    // Dragging past either end clamps instead of running away.
    (void)drag(-500.0f);
    SOL_CHECK(value == 0.0f);
    (void)drag(5000.0f);
    SOL_CHECK(value == 100.0f);

    // A degenerate range must not divide by zero or move the value.
    float fixed = 7.0f;
    ui.beginFrame(mouseAt(200.0f, 120.0f), kScreen);
    (void)ui.slider(kFirst, "Fixed", fixed, 5.0f, 5.0f);
    ui.endFrame();
    SOL_CHECK(fixed == 7.0f);
}

SOL_TEST(ui_context_slider_steps_with_arrow_keys)
{
    UiContext ui;
    float value = 50.0f;

    const auto frame = [&](const InputState& input) {
        ui.beginFrame(input, kScreen);
        (void)ui.slider(kFirst, "Volume", value, 0.0f, 100.0f);
        ui.endFrame();
    };

    InputState next;
    next.navNext = true;
    frame(next); // focus it

    InputState right;
    right.navRight = true;
    frame(right);
    SOL_CHECK(value > 50.0f);

    InputState left;
    left.navLeft = true;
    frame(left);
    frame(left);
    SOL_CHECK(value < 50.0f);
}

SOL_TEST(ui_context_builds_geometry_and_resets_each_frame)
{
    Font font;
    SOL_REQUIRE(loadFont(font));

    UiContext ui;
    ui.setFont(&font, 1);

    ui.beginFrame(InputState{}, kScreen);
    ui.panel({{0.0f, 0.0f}, {400.0f, 300.0f}}, "Aa");
    (void)ui.button(kFirst, "A");
    ui.endFrame();
    const std::size_t firstFrame = ui.drawList().vertices().size();
    SOL_CHECK(firstFrame > 0);
    SOL_CHECK(!ui.drawList().overflowed());

    // The same build must produce the same geometry, not accumulate it.
    ui.beginFrame(InputState{}, kScreen);
    ui.panel({{0.0f, 0.0f}, {400.0f, 300.0f}}, "Aa");
    (void)ui.button(kFirst, "A");
    ui.endFrame();
    SOL_CHECK(ui.drawList().vertices().size() == firstFrame);

    // An empty frame clears everything.
    ui.beginFrame(InputState{}, kScreen);
    ui.endFrame();
    SOL_CHECK(ui.drawList().vertices().empty());
}

// --- Tab strip ---

namespace {

constexpr const char* const kTabLabels[] = {"Trade", "Outfitting", "Shipyard"};
// Three tabs across 300 px: 96 px each with the theme's 6 px spacing, so the
// third runs 304..400.
constexpr Rect kTabStrip = {{100.0f, 100.0f}, {400.0f, 134.0f}};

} // namespace

SOL_TEST(ui_context_tabs_select_on_click)
{
    UiContext ui;
    int selected = 0;

    const auto frame = [&](const InputState& input) {
        ui.beginFrame(input, kScreen);
        const bool changed = ui.tabs(kTabStrip, kTabLabels, selected);
        ui.endFrame();
        return changed;
    };

    SOL_CHECK(!frame(pressAt(350.0f, 110.0f))); // the click completes on release
    SOL_CHECK(frame(releaseAt(350.0f, 110.0f)));
    SOL_CHECK(selected == 2);

    // Clicking the tab that is already open changes nothing.
    (void)frame(pressAt(350.0f, 110.0f));
    SOL_CHECK(!frame(releaseAt(350.0f, 110.0f)));
    SOL_CHECK(selected == 2);
}

SOL_TEST(ui_context_tabs_step_once_per_arrow_press)
{
    UiContext ui;
    int selected = 0;

    const auto frame = [&](const InputState& input) {
        ui.beginFrame(input, kScreen);
        (void)ui.tabs(kTabStrip, kTabLabels, selected);
        ui.endFrame();
    };

    InputState next;
    next.navNext = true;
    frame(next); // focus enters the strip

    InputState right;
    right.navRight = true;
    frame(right);
    SOL_CHECK(selected == 1);

    // Still held: a held key must not run the whole strip in one press.
    frame(right);
    SOL_CHECK(selected == 1);

    frame(InputState{}); // release
    frame(right);
    SOL_CHECK(selected == 2);

    // And wraps, in both directions.
    frame(InputState{});
    frame(right);
    SOL_CHECK(selected == 0);

    InputState left;
    left.navLeft = true;
    frame(left);
    SOL_CHECK(selected == 2);
}

SOL_TEST(ui_context_tabs_survive_a_stale_selection_and_an_empty_strip)
{
    UiContext ui;

    // A selection left over from a screen with more tabs must land somewhere
    // real rather than index past the end.
    int selected = 7;
    ui.beginFrame(InputState{}, kScreen);
    (void)ui.tabs(kTabStrip, kTabLabels, selected);
    ui.endFrame();
    SOL_CHECK(selected == 2);

    selected = -3;
    ui.beginFrame(InputState{}, kScreen);
    (void)ui.tabs(kTabStrip, kTabLabels, selected);
    ui.endFrame();
    SOL_CHECK(selected == 0);

    int none = 4;
    ui.beginFrame(InputState{}, kScreen);
    SOL_CHECK(!ui.tabs(kTabStrip, std::span<const char* const>{}, none));
    ui.endFrame();
    SOL_CHECK(none == 4); // nothing to select, nothing touched
}

// --- Scroll regions ---

namespace {

constexpr Rect kScrollView = {{100.0f, 100.0f}, {400.0f, 300.0f}}; // 200 tall

InputState wheelAt(float x, float y, float delta)
{
    InputState input = mouseAt(x, y);
    input.scrollDelta = delta;
    return input;
}

} // namespace

SOL_TEST(ui_context_scroll_clamps_to_the_content)
{
    UiContext ui;
    float offset = 0.0f;

    // Scrolling down past the end stops at exactly one screenful from the
    // bottom, not somewhere below the content.
    ui.beginFrame(wheelAt(200.0f, 200.0f, -100.0f), kScreen);
    const Rect content = ui.beginScroll(kScrollView, 600.0f, offset);
    ui.endScroll();
    ui.endFrame();
    SOL_CHECK(offset == 400.0f);
    SOL_CHECK(content.min.y == kScrollView.min.y - 400.0f);
    SOL_CHECK(content.max.y == content.min.y + 600.0f);
    // A scrollable region gives up width to its bar.
    SOL_CHECK(content.max.x < kScrollView.max.x);

    // Scrolling back up stops at the top.
    ui.beginFrame(wheelAt(200.0f, 200.0f, 100.0f), kScreen);
    (void)ui.beginScroll(kScrollView, 600.0f, offset);
    ui.endScroll();
    ui.endFrame();
    SOL_CHECK(offset == 0.0f);

    // Content that fits never scrolls and keeps the full width.
    ui.beginFrame(wheelAt(200.0f, 200.0f, -100.0f), kScreen);
    const Rect shortContent = ui.beginScroll(kScrollView, 50.0f, offset);
    ui.endScroll();
    ui.endFrame();
    SOL_CHECK(offset == 0.0f);
    SOL_CHECK(shortContent.max.x == kScrollView.max.x);

    // The wheel only moves the region the cursor is over.
    ui.beginFrame(wheelAt(800.0f, 600.0f, -100.0f), kScreen);
    (void)ui.beginScroll(kScrollView, 600.0f, offset);
    ui.endScroll();
    ui.endFrame();
    SOL_CHECK(offset == 0.0f);
}

SOL_TEST(ui_context_scrolled_rows_are_out_of_the_mouse_reach)
{
    UiContext ui;
    float offset = 0.0f;
    bool visibleFired = false;
    bool hiddenFired = false;

    const auto frame = [&](const InputState& input) {
        ui.beginFrame(input, kScreen);
        const Rect content = ui.beginScroll(kScrollView, 800.0f, offset);
        visibleFired = ui.button({content.min, {content.max.x, content.min.y + 40.0f}}, "First");
        // Laid out below the fold: clipped away visually, and it must be out
        // of reach too, or the player clicks a row they cannot see.
        hiddenFired = ui.button({{content.min.x, content.min.y + 300.0f},
                                 {content.max.x, content.min.y + 340.0f}},
                                "Deep");
        ui.endScroll();
        ui.endFrame();
    };

    frame(pressAt(200.0f, 420.0f));
    frame(releaseAt(200.0f, 420.0f));
    SOL_CHECK(!hiddenFired);

    frame(pressAt(200.0f, 120.0f));
    frame(releaseAt(200.0f, 120.0f));
    SOL_CHECK(visibleFired);
}

SOL_TEST(ui_context_scroll_follows_keyboard_focus)
{
    UiContext ui;
    float offset = 0.0f;
    constexpr float kRowPitch = 50.0f;

    const auto frame = [&](const InputState& input) {
        ui.beginFrame(input, kScreen);
        const Rect content = ui.beginScroll(kScrollView, 8 * kRowPitch, offset);
        for (int i = 0; i < 8; ++i) {
            ui.pushId(i);
            const float top = content.min.y + static_cast<float>(i) * kRowPitch;
            (void)ui.button({{content.min.x, top}, {content.max.x, top + 40.0f}}, "Row");
            ui.popId();
        }
        ui.endScroll();
        ui.endFrame();
    };

    InputState next;
    next.navNext = true;
    for (int i = 0; i < 7; ++i) {
        frame(next); // tab down to a row past the fold
    }
    frame(InputState{}); // the correction lands the frame after the focus move

    // Whatever the offset settled on, the focused row has to be on screen.
    SOL_CHECK(offset > 0.0f);
    const float focusedTop = kScrollView.min.y - offset + 6.0f * kRowPitch;
    SOL_CHECK(focusedTop >= kScrollView.min.y - 0.01f);
    SOL_CHECK(focusedTop + 40.0f <= kScrollView.max.y + 0.01f);
}

SOL_TEST(ui_context_endscroll_without_a_region_is_harmless)
{
    UiContext ui;
    ui.beginFrame(InputState{}, kScreen);
    ui.endScroll(); // unbalanced call must not underflow the stack
    (void)ui.button(kFirst, "Launch");
    ui.endFrame();
    SOL_CHECK(ui.interactiveCount() == 1);
}

// --- Text field (Phase 8h) ---------------------------------------------------

namespace {

constexpr Rect kField = {{100.0f, 100.0f}, {400.0f, 140.0f}};

// One frame of a single focused text field. The field takes focus from the
// click in the frame the caller passes mousePressed, and keeps it after.
bool runField(UiContext& ui, const InputState& input, std::string& value)
{
    ui.beginFrame(input, kScreen);
    const bool changed = ui.textField(kField, "name", value);
    ui.endFrame();
    return changed;
}

// A field that already has focus, so the editing keys apply.
void focusField(UiContext& ui, std::string& value)
{
    (void)runField(ui, pressAt(110.0f, 120.0f), value);
    (void)runField(ui, releaseAt(110.0f, 120.0f), value);
}

InputState typing(std::string_view text)
{
    InputState input;
    input.text = text;
    return input;
}

} // namespace

SOL_TEST(ui_text_field_inserts_typed_text_at_the_caret)
{
    Font font;
    SOL_REQUIRE(loadFont(font));
    UiContext ui;
    ui.setFont(&font, 1);

    std::string value = "Ore";
    focusField(ui, value);
    ui.setCaret(value.size());

    SOL_CHECK(runField(ui, typing(" Field"), value));
    SOL_CHECK(value == "Ore Field");

    // Insertion happens at the caret, not always at the end.
    ui.setCaret(3);
    SOL_CHECK(runField(ui, typing("!"), value));
    SOL_CHECK(value == "Ore! Field");

    // A frame with no text typed reports no change.
    SOL_CHECK(!runField(ui, InputState{}, value));
    SOL_CHECK(value == "Ore! Field");
}

SOL_TEST(ui_text_field_backspace_and_delete_respect_both_ends)
{
    Font font;
    SOL_REQUIRE(loadFont(font));
    UiContext ui;
    ui.setFont(&font, 1);

    std::string value = "abc";
    focusField(ui, value);

    // Backspace at the start has nothing to remove and must not report a
    // change or walk off the front of the string.
    ui.setCaret(0);
    InputState back;
    back.editBackspace = true;
    SOL_CHECK(!runField(ui, back, value));
    SOL_CHECK(value == "abc");

    // Delete at the end likewise.
    ui.setCaret(value.size());
    InputState forward;
    forward.editDelete = true;
    SOL_CHECK(!runField(ui, forward, value));
    SOL_CHECK(value == "abc");

    // Backspace removes what is behind the caret; Delete what is ahead.
    ui.setCaret(2);
    SOL_CHECK(runField(ui, back, value));
    SOL_CHECK(value == "ac");
    ui.setCaret(1);
    SOL_CHECK(runField(ui, forward, value));
    SOL_CHECK(value == "a");
}

SOL_TEST(ui_text_field_caret_moves_over_whole_code_points)
{
    Font font;
    SOL_REQUIRE(loadFont(font));
    UiContext ui;
    ui.setFont(&font, 1);

    // "aeb" with a two-byte middle character: stepping by bytes would split
    // it and leave the string invalid UTF-8.
    std::string value = "a\xC3\xA9" "b";
    focusField(ui, value);
    ui.setCaret(value.size());

    InputState left;
    left.editLeft = true;
    (void)runField(ui, left, value); // over 'b'
    (void)runField(ui, left, value); // over the two-byte character
    // A backspace here must remove the whole 'a' before it, leaving the
    // multi-byte character intact.
    InputState back;
    back.editBackspace = true;
    SOL_CHECK(runField(ui, back, value));
    SOL_CHECK(value == "\xC3\xA9" "b");

    // Home and End reach both extremes.
    InputState home;
    home.editHome = true;
    (void)runField(ui, home, value);
    SOL_CHECK(!runField(ui, back, value)); // already at the start
    InputState end;
    end.editEnd = true;
    (void)runField(ui, end, value);
    SOL_CHECK(runField(ui, back, value));
    SOL_CHECK(value == "\xC3\xA9");
}

SOL_TEST(ui_text_field_stops_at_its_length_budget_on_a_boundary)
{
    Font font;
    SOL_REQUIRE(loadFont(font));
    UiContext ui;
    ui.setFont(&font, 1);

    std::string value;
    (void)runField(ui, pressAt(110.0f, 120.0f), value);
    (void)runField(ui, releaseAt(110.0f, 120.0f), value);

    // Four bytes of room, offered two two-byte characters: both fit.
    ui.beginFrame(typing("\xC3\xA9\xC3\xA9"), kScreen);
    (void)ui.textField(kField, "name", value, 4);
    ui.endFrame();
    SOL_CHECK(value == "\xC3\xA9\xC3\xA9");

    // Full now, so further typing is refused rather than truncated mid
    // character.
    ui.beginFrame(typing("\xC3\xA9"), kScreen);
    const bool changed = ui.textField(kField, "name", value, 4);
    ui.endFrame();
    SOL_CHECK(!changed);
    SOL_CHECK(value == "\xC3\xA9\xC3\xA9");
}

SOL_TEST(ui_text_field_takes_the_arrows_away_from_the_tab_strip)
{
    Font font;
    SOL_REQUIRE(loadFont(font));
    UiContext ui;
    ui.setFont(&font, 1);

    static const char* const kTabs[] = {"Galaxy", "System"};
    std::string value = "x";
    int tab = 0;

    // With focus in the field, a right-arrow is the caret's - the tab strip
    // must not step, or typing into a field would change screens underneath.
    focusField(ui, value);
    SOL_CHECK(ui.editingText());

    InputState right;
    right.navRight = true;
    right.editRight = true;
    ui.beginFrame(right, kScreen);
    (void)ui.tabs({{100.0f, 40.0f}, {400.0f, 70.0f}}, std::span<const char* const>(kTabs), tab);
    (void)ui.textField(kField, "name", value);
    ui.endFrame();
    SOL_CHECK(tab == 0);
}
