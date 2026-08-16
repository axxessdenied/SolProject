#pragma once

#include "sol/ui/context.hpp"

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

} // namespace game
