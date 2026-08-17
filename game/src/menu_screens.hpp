#pragma once

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
};

// Player-facing options, persisted beside the save as TOML. Kept out of the
// save file so a settings change never risks the run.
struct Settings
{
    float uiScale = 1.0f;
    float mouseSensitivity = 1.0f;
    bool invertPitch = false;
    bool vsync = true;

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
    QuitGame,
};

struct MainMenuState
{
    bool hasSave = false;   // enables Continue
    bool hardcore = false;  // new-run flag, was --hardcore only
};

// Each returns the action the player triggered this frame, or None.
[[nodiscard]] MenuAction buildMainMenu(sol::ui::UiContext& ui, MainMenuState& state);
[[nodiscard]] MenuAction buildPauseMenu(sol::ui::UiContext& ui, bool hardcore);
[[nodiscard]] MenuAction buildSettingsScreen(sol::ui::UiContext& ui, Settings& settings);

// The bookmark naming prompt (Phase 8h). Drawn over the flight view rather
// than as its own GameState: the galaxy keeps running, and dropping a
// waypoint should not feel like leaving the cockpit. Writes the decision back
// into `prompt.accepted` / `prompt.cancelled`.
void buildBookmarkPrompt(sol::ui::UiContext& ui, sol::ui::BookmarkPrompt& prompt);

} // namespace game
