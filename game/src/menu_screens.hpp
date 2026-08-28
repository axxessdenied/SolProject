#pragma once

#include "input_actions.hpp"
#include "save_catalog.hpp"

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
    // Naming a run before it starts (Phase 27). Its own state rather than a
    // field on the main menu, because it owns the keyboard: a text field that
    // swallows nav keys cannot share a screen with buttons the player is
    // meant to be able to arrow between.
    NewGame,
    // The save browser (Phase 27), in either of its two modes - see
    // SaveBrowserState. One screen and not two: the list, the rows, the
    // scrolling and the delete are identical, and only what the buttons do
    // differs. Two screens would be two places to fix the same row.
    SaveBrowser,
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
    // Phase 27 split the old NewGame in two: the main menu OPENS the naming
    // screen, and the naming screen STARTS the run. The old single action had
    // nowhere to put a campaign name.
    OpenNewGame,
    StartNewGame,
    ContinueGame,
    Resume,
    OpenSaveBrowser, // pause menu -> browser in Save mode
    OpenLoadBrowser, // either menu -> browser in Load mode
    CloseBrowser,
    // Acting on the browser's selection. The screen never touches a file: it
    // reports, and the frame loop reads SaveBrowserState for what was picked.
    LoadSelected,
    SaveSelected,
    DeleteSelectedSave,
    DeleteSelectedCampaign,
    OpenSettings,
    CloseSettings,
    OpenControls,
    CloseControls,
    // Phase 27. buildPauseMenu has carried a comment since Phase 8d saying
    // this could not exist without a world reset; SpaceWorld::resetForNewGame
    // and GameContent::restartForNewGame are that reset.
    QuitToMainMenu,
    QuitGame,
};

struct MainMenuState
{
    bool hasSave = false; // enables Continue and Load; set from the catalog
    // The campaign Continue would resume, so the button can say which run it
    // means. Empty when there is nothing to continue.
    std::string continueLabel;
};

// Naming a run before it starts (Phase 27).
struct NewGameState
{
    std::string name;
    bool hardcore = false;         // was --hardcore only, then a main-menu checkbox
    bool focusRequested = false;   // consumed by the screen to focus the field
    bool nameIsSuggestion = false; // first keypress REPLACES the prefill, as
                                   // BookmarkPrompt does - there are no
                                   // selection ranges to delete a prefill with
};

// Which job the browser is doing. The rows are the same either way; the
// buttons and the campaign column are not.
enum class SaveBrowserMode : std::uint32_t
{
    Load,
    Save,
};

// Scroll positions, the selection, and what a pending delete is armed on.
struct SaveBrowserState
{
    SaveBrowserMode mode = SaveBrowserMode::Load;
    // Indices into SaveCatalog::campaigns() and that campaign's saves. -1 for
    // "nothing selected", which is the honest state on an empty catalog and
    // the reason these are signed.
    int campaign = -1;
    int save = -1;
    float campaignScroll = 0.0f;
    float saveScroll = 0.0f;
    std::string newSaveName;
    bool focusRequested = false;

    // ⚑ A DELETE TAKES TWO PRESSES AND THE SECOND ONE IS A DIFFERENT BUTTON
    // LABEL. There is no undo behind either delete - one removes a file, the
    // other removes a directory and everything in it - and a menu with no
    // confirmation one mis-click away from a run is the shape of defect that
    // gets reported as "the game deleted my save".
    enum class Pending : std::uint32_t
    {
        None,
        Save,
        Campaign,
    };
    Pending pending = Pending::None;

    std::string notice; // one line under the lists; empty = nothing to say

    // Dropped whenever the browser opens or the selection moves, so an armed
    // delete can never survive into a different row.
    void disarm() { pending = Pending::None; }
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

// The naming screen. `submit` is Enter (from UiContext::submitRequested), which
// starts the run without reaching for the mouse.
[[nodiscard]] MenuAction buildNewGameScreen(sol::ui::UiContext& ui, NewGameState& state);

// The save browser. Reads the catalog and never writes: what the player picked
// is left in `state` for the frame loop to act on, the same fill/execute seam
// every other screen here uses.
//
// `activeCampaign` is the run in progress, which is the only campaign Save mode
// will write into - saving run A into run B's folder is not a thing to offer.
// Empty in Load mode from the main menu, where there is no run yet.
[[nodiscard]] MenuAction buildSaveBrowser(sol::ui::UiContext& ui,
                                          const SaveCatalog& catalog,
                                          SaveBrowserState& state,
                                          std::string_view activeCampaign);

// The rebind list. `captured` is the chord that went down this frame (from
// BindingTable::captured()), consumed only while a row is armed; `cancel` is
// Escape, which abandons a capture rather than backing out of the screen.
[[nodiscard]] MenuAction buildControlsScreen(sol::ui::UiContext& ui,
                                             Settings& settings,
                                             ControlsScreenState& state,
                                             sol::platform::InputChord captured,
                                             bool cancel);

// The bookmark naming prompt (Phase 8h). Drawn over the flight view rather
// than as its own GameState: the galaxy keeps running, and dropping a
// waypoint should not feel like leaving the cockpit. Writes the decision back
// into `prompt.accepted` / `prompt.cancelled`.
void buildBookmarkPrompt(sol::ui::UiContext& ui, sol::ui::BookmarkPrompt& prompt);

} // namespace game
