#pragma once

#include "input_actions.hpp"

#include "sol/platform/input_bindings.hpp"
#include "sol/ui/context.hpp"
#include "sol/ui/screens.hpp"

#include <string>

namespace game {

// Where the player is: the shell the game never had while the only surface
// was the flight view. main.cpp owns one of these and the frame loop branches
// on it.
enum class GameState : std::uint32_t
{
    MainMenu,
    Flying,
    Docked,
    Paused,
    Settings,
    // The map (Phase 8e). Owns the keyboard like the station screen does, and
    // like it does *not* stop the clock: a galaxy that pauses while you read
    // the map is not the galaxy this game is selling.
    Map,
    // The ship readout (Phase 8h). Like the map, a screen rather than a pause:
    // reading your own numbers is not a reason for the galaxy to stop, and
    // half of them only mean anything while something is happening to you.
    ShipInfo,
    // The rebind list (Phase 8k). Its own screen rather than four more rows on
    // Settings: 34 actions do not fit a 540 px panel, and this one needs a
    // scroll region and a capture mode that the settings sliders do not.
    Controls,
};

// Player-facing options, persisted beside the save as TOML. Kept out of the
// save file so a settings change never risks the run.
struct Settings
{
    float uiScale = 1.0f;
    float mouseSensitivity = 1.0f;
    bool invertPitch = false;
    bool vsync = true;
    // Audio (Phase 8t). Two sliders rather than three: a music slider with no
    // music to govern is a control that does nothing, which is the class of
    // defect 8k spent a whole item removing.
    float masterVolume = 0.8f;
    float effectsVolume = 1.0f;

    // Controls (Phase 8k). Constructed with the shipped layout so a missing or
    // partial settings file is a complete, playable binding set rather than a
    // ship that cannot be flown.
    sol::platform::BindingTable bindings;

    Settings() { installDefaultBindings(bindings); }

    [[nodiscard]] bool load(const char* path);
    [[nodiscard]] bool save(const char* path) const;
};

// What a menu screen is asking main.cpp to do. The screens never touch the
// world themselves - they report, the frame loop acts.
enum class MenuAction : std::uint32_t
{
    None,
    NewGame,
    ContinueGame,
    Resume,
    SaveGame,
    LoadGame,
    OpenSettings,
    CloseSettings,
    OpenControls,
    CloseControls,
    QuitGame,
};

struct MainMenuState
{
    bool hasSave = false;   // enables Continue
    bool hardcore = false;  // new-run flag, was --hardcore only
};

// Scroll position, which row is capturing, and what the last assignment did.
// Lives across frames in main.cpp beside the other screen states.
struct ControlsScreenState
{
    float scroll = 0.0f;
    // The action waiting for a chord, or kActionCount for "none armed".
    std::uint32_t capturing = kActionCount;
    // The action a steal left unbound, so its row can say so. Cleared when the
    // player rebinds it or resets to defaults.
    std::uint32_t stolenFrom = kActionCount;
    std::string notice; // one line under the list; empty = nothing to say
};

// Each returns the action the player triggered this frame, or None.
[[nodiscard]] MenuAction buildMainMenu(sol::ui::UiContext& ui, MainMenuState& state);
[[nodiscard]] MenuAction buildPauseMenu(sol::ui::UiContext& ui, bool hardcore);
[[nodiscard]] MenuAction buildSettingsScreen(sol::ui::UiContext& ui, Settings& settings);

// The rebind list. `captured` is the chord that went down this frame (from
// BindingTable::captured()), consumed only while a row is armed; `cancel` is
// Escape, which abandons a capture rather than backing out of the screen.
[[nodiscard]] MenuAction buildControlsScreen(sol::ui::UiContext& ui, Settings& settings,
                                             ControlsScreenState& state,
                                             sol::platform::InputChord captured, bool cancel);

// The bookmark naming prompt (Phase 8h). Drawn over the flight view rather
// than as its own GameState: the galaxy keeps running, and dropping a
// waypoint should not feel like leaving the cockpit. Writes the decision back
// into `prompt.accepted` / `prompt.cancelled`.
void buildBookmarkPrompt(sol::ui::UiContext& ui, sol::ui::BookmarkPrompt& prompt);

} // namespace game
