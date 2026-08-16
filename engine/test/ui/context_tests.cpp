#include <sol/ui/context.hpp>

#include <sol/test/synthetic_cooked_font.hpp>
#include <sol/test/test.hpp>

#include <cstdint>
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
