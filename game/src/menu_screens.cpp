#include "menu_screens.hpp"

#include "sol/core/log.hpp"
#include "sol/core/toml.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/ui/layout.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace game {

using sol::ui::Column;
using sol::ui::Rect;
using sol::ui::Row;
using sol::ui::TextAlign;
using sol::ui::UiContext;

namespace {

constexpr float kMenuWidth = 380.0f;
constexpr float kButtonHeight = 44.0f;
constexpr float kTitleHeight = 56.0f;

// Centers a panel of the given size on screen.
Rect centeredPanel(const UiContext& ui, float width, float height)
{
    const float x = (ui.screenSize().x - width) * 0.5f;
    const float y = (ui.screenSize().y - height) * 0.5f;
    return {{x, y}, {x + width, y + height}};
}

void dimBackground(UiContext& ui)
{
    ui.drawList().addRect({{0.0f, 0.0f}, ui.screenSize()}, ui.theme().background);
}

} // namespace

bool Settings::load(const char* path)
{
    std::vector<std::uint8_t> bytes;
    if (!sol::platform::readFileBytes(path, bytes)) {
        return false; // no settings yet is normal, not an error
    }

    sol::core::TomlValue root;
    std::string error;
    if (!sol::core::TomlValue::parse(reinterpret_cast<const char*>(bytes.data()), bytes.size(), root,
                                     &error)) {
        SOL_LOG_WARN("settings: %s (%s) - using defaults", error.c_str(), path);
        return false;
    }

    // Unknown or malformed entries fall back to the default rather than
    // failing the load: a stale settings file must never block startup.
    const Settings defaults;
    const auto readFloat = [&](const char* key, float fallback, float minimum, float maximum) {
        const sol::core::TomlValue* value = root.find(key);
        if (value == nullptr || (!value->isFloat() && !value->isInteger())) {
            return fallback;
        }
        const float parsed = static_cast<float>(value->asFloat(fallback));
        return parsed < minimum ? minimum : (parsed > maximum ? maximum : parsed);
    };
    const auto readBool = [&](const char* key, bool fallback) {
        const sol::core::TomlValue* value = root.find(key);
        return value != nullptr && value->isBool() ? value->asBool(fallback) : fallback;
    };

    uiScale = readFloat("ui_scale", defaults.uiScale, 0.5f, 2.0f);
    mouseSensitivity = readFloat("mouse_sensitivity", defaults.mouseSensitivity, 0.1f, 4.0f);
    invertPitch = readBool("invert_pitch", defaults.invertPitch);
    vsync = readBool("vsync", defaults.vsync);
    return true;
}

bool Settings::save(const char* path) const
{
    char text[512] = {};
    const int written = std::snprintf(text, sizeof(text),
                                      "# The Stars Don't Wait - player settings\n"
                                      "ui_scale = %.3f\n"
                                      "mouse_sensitivity = %.3f\n"
                                      "invert_pitch = %s\n"
                                      "vsync = %s\n",
                                      static_cast<double>(uiScale),
                                      static_cast<double>(mouseSensitivity),
                                      invertPitch ? "true" : "false", vsync ? "true" : "false");
    if (written <= 0) {
        return false;
    }
    return sol::platform::writeFileBytes(path, text, static_cast<std::size_t>(written));
}

MenuAction buildMainMenu(UiContext& ui, MainMenuState& state)
{
    dimBackground(ui);
    ui.pushId("main_menu");

    const float height = kTitleHeight + 40.0f + kButtonHeight * 4.0f + ui.theme().spacing * 5.0f +
                         ui.theme().padding * 2.0f + 34.0f;
    const Rect panel = centeredPanel(ui, kMenuWidth, height);
    ui.panel(panel);

    Column column(panel, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(kTitleHeight), "The Stars Don't Wait", ui.theme().textPrimary,
             ui.theme().headingStyle, TextAlign::Center);
    ui.label(column.row(28.0f), "Carve out a life in a galaxy already in motion.", ui.theme().textDim,
             ui.theme().smallStyle, TextAlign::Center);
    column.skip(12.0f);

    MenuAction action = MenuAction::None;
    if (ui.button(column.row(kButtonHeight), "New Game")) {
        action = MenuAction::NewGame;
    }
    // Continue is present but disabled without a save, so its absence never
    // reshuffles the menu under the player's cursor.
    if (ui.button(column.row(kButtonHeight), "Continue", state.hasSave)) {
        action = MenuAction::ContinueGame;
    }
    if (ui.button(column.row(kButtonHeight), "Settings")) {
        action = MenuAction::OpenSettings;
    }
    if (ui.button(column.row(kButtonHeight), "Quit")) {
        action = MenuAction::QuitGame;
    }

    column.skip(6.0f);
    (void)ui.checkbox(column.row(30.0f), "Hardcore (death ends the run)", state.hardcore);

    ui.popId();
    return action;
}

MenuAction buildPauseMenu(UiContext& ui, bool hardcore)
{
    dimBackground(ui);
    ui.pushId("pause_menu");

    const int buttonCount = hardcore ? 4 : 5;
    const float height = kTitleHeight + kButtonHeight * static_cast<float>(buttonCount) +
                         ui.theme().spacing * static_cast<float>(buttonCount) +
                         ui.theme().padding * 2.0f;
    const Rect panel = centeredPanel(ui, kMenuWidth, height);
    ui.panel(panel);

    Column column(panel, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(kTitleHeight), "Paused", ui.theme().textPrimary, ui.theme().headingStyle,
             TextAlign::Center);

    MenuAction action = MenuAction::None;
    if (ui.button(column.row(kButtonHeight), "Resume")) {
        action = MenuAction::Resume;
    }
    // A hardcore run has one save that death deletes; offering "Save" would
    // imply a safety net the mode does not have.
    if (!hardcore && ui.button(column.row(kButtonHeight), "Save Game")) {
        action = MenuAction::SaveGame;
    }
    if (ui.button(column.row(kButtonHeight), "Load Game")) {
        action = MenuAction::LoadGame;
    }
    if (ui.button(column.row(kButtonHeight), "Settings")) {
        action = MenuAction::OpenSettings;
    }
    // No "quit to main menu" yet: starting a second run in one process needs a
    // world reset that does not exist, and a menu entry that only half works
    // is worse than one that is missing.
    if (ui.button(column.row(kButtonHeight), "Quit to Desktop")) {
        action = MenuAction::QuitGame;
    }

    // Esc backs out the same way the button does.
    if (ui.cancelRequested()) {
        action = MenuAction::Resume;
    }

    ui.popId();
    return action;
}

MenuAction buildSettingsScreen(UiContext& ui, Settings& settings)
{
    dimBackground(ui);
    ui.pushId("settings");

    const float rowHeight = 34.0f;
    const float height = kTitleHeight + rowHeight * 4.0f + kButtonHeight +
                         ui.theme().spacing * 6.0f + ui.theme().padding * 2.0f;
    const Rect panel = centeredPanel(ui, 540.0f, height);
    ui.panel(panel);

    Column column(panel, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(kTitleHeight), "Settings", ui.theme().textPrimary, ui.theme().headingStyle,
             TextAlign::Center);

    char buffer[64] = {};
    const auto sliderRow = [&](const char* label, float& value, float minimum, float maximum) {
        Row row(column.row(rowHeight), ui.theme().spacing);
        // Wide enough for the longest option name at body size; a label that
        // overruns its cell would run under the slider track.
        ui.label(row.cell(190.0f), label, ui.theme().textDim, ui.theme().bodyStyle);
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
        const Rect valueBox = row.cellFromRight(56.0f);
        (void)ui.slider(row.remaining(), label, value, minimum, maximum);
        ui.label(valueBox, buffer, ui.theme().textPrimary, ui.theme().smallStyle, TextAlign::Right);
    };

    sliderRow("UI scale", settings.uiScale, 0.5f, 2.0f);
    sliderRow("Mouse sensitivity", settings.mouseSensitivity, 0.1f, 4.0f);
    (void)ui.checkbox(column.row(rowHeight), "Invert pitch", settings.invertPitch);
    (void)ui.checkbox(column.row(rowHeight), "V-Sync", settings.vsync);

    column.skip(6.0f);
    MenuAction action = MenuAction::None;
    if (ui.button(column.row(kButtonHeight), "Back") || ui.cancelRequested()) {
        action = MenuAction::CloseSettings;
    }

    ui.popId();
    return action;
}

void buildBookmarkPrompt(UiContext& ui, sol::ui::BookmarkPrompt& prompt)
{
    prompt.accepted = false;
    prompt.cancelled = false;
    if (!prompt.open) {
        return;
    }

    constexpr float kWidth = 460.0f;
    constexpr float kHeight = 176.0f;
    // Sits above centre so the crosshair and the thing being bookmarked stay
    // visible behind it - this is a note about what you are looking at.
    const float x = (ui.screenSize().x - kWidth) * 0.5f;
    const float y = ui.screenSize().y * 0.28f;
    const Rect frame = {{x, y}, {x + kWidth, y + kHeight}};
    ui.panel(frame);
    ui.pushId("bookmark_prompt");

    Column column(frame, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(30.0f), prompt.full ? "Too many bookmarks here" : "Bookmark this place",
             ui.theme().textPrimary, ui.theme().headingStyle);
    ui.label(column.row(20.0f), prompt.whereSummary, ui.theme().textDim, ui.theme().smallStyle);
    column.skip(4.0f);

    if (prompt.full) {
        ui.label(column.row(20.0f), "Delete one from the system map first.", ui.theme().textDim,
                 ui.theme().smallStyle);
        column.skip(4.0f);
        Row buttons(column.row(kButtonHeight), ui.theme().spacing);
        if (ui.button(buttons.cellFromRight(120.0f), "Close") || ui.cancelRequested()) {
            prompt.cancelled = true;
        }
        ui.popId();
        return;
    }

    const Rect fieldCell = column.row(34.0f);
    if (prompt.focusRequested) {
        // Focus the field so the player can type immediately; the caret goes
        // to the end, which is where an edit to a suggested name starts.
        ui.setFocus(ui.idFor("name"));
        ui.setCaret(prompt.name.size());
        prompt.focusRequested = false;
    }
    // The suggestion is a default, not a starting point to edit: the first
    // character typed clears it. Without selection ranges this is the only way
    // a prefilled field is not actively in the way. Gated on the field
    // actually holding focus - otherwise a stray character with focus
    // elsewhere wipes the suggested name and puts nothing in its place.
    if (prompt.nameIsSuggestion && !ui.input().text.empty()
        && ui.isFocused(ui.idFor("name"))) {
        prompt.name.clear();
        ui.setCaret(0);
        prompt.nameIsSuggestion = false;
    }
    if (ui.textField(fieldCell, "name", prompt.name)) {
        prompt.nameIsSuggestion = false;
    }
    column.skip(6.0f);

    Row buttons(column.row(kButtonHeight), ui.theme().spacing);
    const Rect saveCell = buttons.cellFromRight(120.0f);
    const Rect cancelCell = buttons.cellFromRight(120.0f);
    // Enter accepts even with the field focused, which is what makes the
    // prefilled name a one-key confirmation; the field ignores navActivate,
    // so this does not fight it.
    if (ui.button(saveCell, "Save", !prompt.name.empty()) || ui.submitRequested()) {
        prompt.accepted = !prompt.name.empty();
    }
    if (ui.button(cancelCell, "Cancel") || ui.cancelRequested()) {
        prompt.cancelled = true;
    }

    ui.popId();
}

} // namespace game
