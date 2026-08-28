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

// A hand-edited settings file can name one chord for two actions, which the
// in-game path can never produce because assignment steals. The earlier action
// keeps it and the later one is unbound: a visible "this needs a key" beats
// two actions firing off one press, which is a bug the player did not build.
void repairDuplicateBindings(sol::platform::BindingTable& bindings)
{
    for (std::uint32_t action = 0; action < bindings.actionCount(); ++action) {
        const sol::platform::InputChord chord = bindings.chordFor(action);
        if (!chord.bound()) {
            continue;
        }
        if (bindings.find(chord) != action) {
            SOL_LOG_WARN("settings: '%s' is bound twice - '%s' left unbound",
                         sol::platform::chordName(chord),
                         actionLabel(static_cast<Action>(action)));
            bindings.unbind(action);
        }
    }
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
    if (!sol::core::TomlValue::parse(
            reinterpret_cast<const char*>(bytes.data()), bytes.size(), root, &error)) {
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
    masterVolume = readFloat("master_volume", defaults.masterVolume, 0.0f, 1.0f);
    effectsVolume = readFloat("effects_volume", defaults.effectsVolume, 0.0f, 1.0f);

    // Autosave (Phase 27). Clamped on the way in like every other value here:
    // a hand-edited interval of 0 would autosave every frame, and a negative
    // ring size would index backwards out of the catalog.
    autosaveEnabled = readBool("autosave_enabled", defaults.autosaveEnabled);
    autosaveMinutes = readFloat("autosave_minutes", defaults.autosaveMinutes, 1.0f, 60.0f);
    autosaveOnDock = readBool("autosave_on_dock", defaults.autosaveOnDock);
    autosaveKeep =
        static_cast<int>(readFloat("autosave_keep", static_cast<float>(defaults.autosaveKeep), 1.0f, 10.0f));

    // Bindings (Phase 8k). Defaults are already installed by the constructor,
    // so an absent [bindings] table, an absent action within it, or a line
    // this build does not understand all leave the shipped layout in place -
    // a settings file must never be the reason the game cannot be flown.
    if (const sol::core::TomlValue* table = root.find("bindings"); table != nullptr && table->isTable()) {
        for (const auto& [key, value] : table->members()) {
            if (!value.isString()) {
                SOL_LOG_WARN("settings: binding '%s' is not a string - keeping the default", key.c_str());
                continue;
            }
            bool known = false;
            const Action action = actionFromId(key.c_str(), known);
            if (!known) {
                SOL_LOG_WARN("settings: unknown action '%s' - ignored", key.c_str());
                continue;
            }
            // An empty name is how a deliberately unbound action is written,
            // and is not the same thing as an unreadable one.
            const std::string& name = value.asString();
            const sol::platform::InputChord chord = sol::platform::chordFromName(name);
            if (!chord.bound() && !name.empty()) {
                SOL_LOG_WARN("settings: '%s' is not a key or button ('%s') - keeping the default",
                             name.c_str(),
                             key.c_str());
                continue;
            }
            // bind() rather than assign(): the file is read as written, and a
            // file that names one chord twice is repaired below rather than
            // silently letting the first reader win.
            bindings.bind(static_cast<std::uint32_t>(action), chord);
        }
        repairDuplicateBindings(bindings);
    }
    return true;
}

bool Settings::save(const char* path) const
{
    // A std::string builder rather than the fixed char[512] this used to be:
    // 34 bindings overflow that buffer several times over.
    // Grown for Phase 27's four autosave keys. ⚑ snprintf TRUNCATES rather
    // than failing, and a truncated settings file loses whichever keys fell
    // off the end - silently, and only for players whose values happen to be
    // long. The `written` check below catches a negative return, not a clipped
    // one, so the buffer is the whole guard.
    char scalars[512] = {};
    const int written = std::snprintf(scalars,
                                      sizeof(scalars),
                                      "# The Stars Don't Wait - player settings\n"
                                      "ui_scale = %.3f\n"
                                      "mouse_sensitivity = %.3f\n"
                                      "invert_pitch = %s\n"
                                      "vsync = %s\n"
                                      "master_volume = %.3f\n"
                                      "effects_volume = %.3f\n"
                                      "autosave_enabled = %s\n"
                                      "autosave_minutes = %.1f\n"
                                      "autosave_on_dock = %s\n"
                                      // ⚑ %d, not %.3f. autosave_keep is a
                                      // COUNT, and writing it as "3.000" makes
                                      // a file that is meant to be hand-edited
                                      // read as though a fractional number of
                                      // autosaves were a thing you could ask
                                      // for. The reader accepts either.
                                      "autosave_keep = %d\n",
                                      static_cast<double>(uiScale),
                                      static_cast<double>(mouseSensitivity),
                                      invertPitch ? "true" : "false",
                                      vsync ? "true" : "false",
                                      static_cast<double>(masterVolume),
                                      static_cast<double>(effectsVolume),
                                      autosaveEnabled ? "true" : "false",
                                      static_cast<double>(autosaveMinutes),
                                      autosaveOnDock ? "true" : "false",
                                      autosaveKeep);
    if (written <= 0) {
        return false;
    }

    std::string text(scalars, static_cast<std::size_t>(written));
    text += "\n[bindings]\n";
    for (std::uint32_t i = 0; i < kActionCount; ++i) {
        const Action action = static_cast<Action>(i);
        const sol::platform::InputChord chord = bindings.chordFor(i);
        // An unbound action is written as an empty string, not omitted: the
        // reader treats a missing key as "use the default", so omitting it
        // would silently rebind it on the next load.
        text += actionId(action);
        text += " = \"";
        text += chord.bound() ? sol::platform::chordName(chord) : "";
        text += "\"\n";
    }
    return sol::platform::writeFileBytes(path, text.data(), text.size());
}

MenuAction buildMainMenu(UiContext& ui, MainMenuState& state)
{
    dimBackground(ui);
    ui.pushId("main_menu");

    // ⚑ Sized from its contents, and the contents changed in Phase 27: five
    // buttons where there were four, and no hardcore checkbox (naming a run
    // owns that now). The count here is title + subtitle + a 12 px gap + five
    // buttons, with one spacing after each of the seven rows. Getting this
    // wrong does not fail to build - it pushes the last button out through the
    // bottom of the panel, which is the defect buildSettingsScreen's own
    // comment warns about.
    const float height = kTitleHeight + 28.0f + 12.0f + kButtonHeight * 5.0f + ui.theme().spacing * 7.0f +
                         ui.theme().padding * 2.0f;
    const Rect panel = centeredPanel(ui, kMenuWidth, height);
    ui.panel(panel);

    Column column(panel, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(kTitleHeight),
             "The Stars Don't Wait",
             ui.theme().textPrimary,
             ui.theme().headingStyle,
             TextAlign::Center);
    ui.label(column.row(28.0f),
             "Carve out a life in a galaxy already in motion.",
             ui.theme().textDim,
             ui.theme().smallStyle,
             TextAlign::Center);
    column.skip(12.0f);

    MenuAction action = MenuAction::None;
    if (ui.button(column.row(kButtonHeight), "New Game")) {
        action = MenuAction::OpenNewGame;
    }
    // Continue is present but disabled without a save, so its absence never
    // reshuffles the menu under the player's cursor.
    //
    // ⚑ It NAMES the run it would resume (Phase 27). With one save "Continue"
    // was unambiguous; with several campaigns it is a question, and a button
    // that silently picks one of them is worse than one that says which.
    std::string continueLabel = "Continue";
    if (!state.continueLabel.empty()) {
        continueLabel += " - ";
        continueLabel += state.continueLabel;
    }
    if (ui.button(column.row(kButtonHeight), continueLabel, state.hasSave)) {
        action = MenuAction::ContinueGame;
    }
    if (ui.button(column.row(kButtonHeight), "Load Game", state.hasSave)) {
        action = MenuAction::OpenLoadBrowser;
    }
    if (ui.button(column.row(kButtonHeight), "Settings")) {
        action = MenuAction::OpenSettings;
    }
    if (ui.button(column.row(kButtonHeight), "Quit")) {
        action = MenuAction::QuitGame;
    }

    ui.popId();
    return action;
}

MenuAction buildNewGameScreen(UiContext& ui, NewGameState& state)
{
    dimBackground(ui);
    ui.pushId("new_game");

    constexpr float kRowHeight = 34.0f;
    const float height = kTitleHeight + 26.0f + kRowHeight * 3.0f + kButtonHeight * 2.0f +
                         ui.theme().spacing * 8.0f + ui.theme().padding * 2.0f;
    const Rect panel = centeredPanel(ui, 460.0f, height);
    ui.panel(panel);

    Column column(panel, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(kTitleHeight),
             "New Game",
             ui.theme().textPrimary,
             ui.theme().headingStyle,
             TextAlign::Center);
    ui.label(column.row(26.0f),
             "This run gets its own folder. Saves inside it stay together.",
             ui.theme().textDim,
             ui.theme().smallStyle,
             TextAlign::Center);

    {
        Row row(column.row(kRowHeight), ui.theme().spacing);
        ui.label(row.cell(90.0f), "Name", ui.theme().textDim, ui.theme().bodyStyle);
        const Rect field = row.remaining();
        if (state.focusRequested) {
            ui.setFocus(ui.idFor("campaign_name"));
            ui.setCaret(state.name.size());
            state.focusRequested = false;
        }
        const std::size_t before = state.name.size();
        if (ui.textField(field, "campaign_name", state.name, 48) && state.nameIsSuggestion) {
            // The prefill is a suggestion until the player types, and then the
            // first character REPLACES it rather than appending. Same rule as
            // BookmarkPrompt, and for the same reason: there are no selection
            // ranges to delete a prefill with.
            if (state.name.size() > before) {
                const char typed = state.name.back();
                state.name.assign(1, typed);
                ui.setCaret(state.name.size());
            }
            state.nameIsSuggestion = false;
        }
    }

    // What the folder will actually be called, shown live. A name is reduced
    // to something a directory can be called (see sanitizeCampaignName), and a
    // player typing "Nyx/../etc" deserves to see that before they commit
    // rather than to find a folder they did not name.
    const std::string folder = sanitizeCampaignName(state.name);
    std::string folderLine = "Folder: ";
    folderLine += folder;
    ui.label(column.row(kRowHeight), folderLine, ui.theme().textDim, ui.theme().smallStyle);

    (void)ui.checkbox(column.row(kRowHeight), "Hardcore (death ends the run)", state.hardcore);

    column.skip(6.0f);
    MenuAction action = MenuAction::None;
    // Enter starts the run, but only while the field is not eating the key.
    if (ui.button(column.row(kButtonHeight), "Start") || (ui.submitRequested() && !state.name.empty())) {
        action = MenuAction::StartNewGame;
    }
    if (ui.button(column.row(kButtonHeight), "Back") || ui.cancelRequested()) {
        action = MenuAction::CloseBrowser;
    }

    ui.popId();
    return action;
}

MenuAction buildPauseMenu(UiContext& ui, bool hardcore)
{
    dimBackground(ui);
    ui.pushId("pause_menu");

    const int buttonCount = hardcore ? 5 : 6;
    const float height = kTitleHeight + kButtonHeight * static_cast<float>(buttonCount) +
                         ui.theme().spacing * static_cast<float>(buttonCount) + ui.theme().padding * 2.0f;
    const Rect panel = centeredPanel(ui, kMenuWidth, height);
    ui.panel(panel);

    Column column(panel, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(kTitleHeight),
             "Paused",
             ui.theme().textPrimary,
             ui.theme().headingStyle,
             TextAlign::Center);

    MenuAction action = MenuAction::None;
    if (ui.button(column.row(kButtonHeight), "Resume")) {
        action = MenuAction::Resume;
    }
    // A hardcore run has one save that death deletes; offering "Save" would
    // imply a safety net the mode does not have.
    if (!hardcore && ui.button(column.row(kButtonHeight), "Save Game")) {
        action = MenuAction::OpenSaveBrowser;
    }
    if (ui.button(column.row(kButtonHeight), "Load Game")) {
        action = MenuAction::OpenLoadBrowser;
    }
    if (ui.button(column.row(kButtonHeight), "Settings")) {
        action = MenuAction::OpenSettings;
    }
    // ⚑ Phase 27. This button was missing for nineteen phases and this comment
    // used to explain why: "starting a second run in one process needs a world
    // reset that does not exist". It exists now - SpaceWorld::resetForNewGame
    // move-assigns a default-constructed world, so it cannot go stale as the
    // class grows, and GameContent::restartForNewGame regenerates the galaxy
    // and re-runs the boot scripts.
    if (ui.button(column.row(kButtonHeight), "Quit to Main Menu")) {
        action = MenuAction::QuitToMainMenu;
    }
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
    // Ten rows since Phase 27 added the four autosave controls, and one
    // spacing per item: the panel is sized from its contents, so adding a row
    // without moving these two numbers pushes the buttons out through the
    // bottom. (Phase 27 shipped exactly that defect on the main menu by
    // forgetting this comment lives here.)
    const float height = kTitleHeight + rowHeight * 10.0f + kButtonHeight * 2.0f +
                         ui.theme().spacing * 13.0f + ui.theme().padding * 2.0f;
    const Rect panel = centeredPanel(ui, 540.0f, height);
    ui.panel(panel);

    Column column(panel, ui.theme().padding, ui.theme().spacing);
    ui.label(column.row(kTitleHeight),
             "Settings",
             ui.theme().textPrimary,
             ui.theme().headingStyle,
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
    sliderRow("Master volume", settings.masterVolume, 0.0f, 1.0f);
    sliderRow("Effects volume", settings.effectsVolume, 0.0f, 1.0f);

    (void)ui.checkbox(column.row(rowHeight), "Autosave", settings.autosaveEnabled);
    // The two that only mean anything while autosave is on are DISABLED rather
    // than hidden: a settings panel whose rows move as you toggle things is a
    // panel you cannot learn the shape of, and the same rule already keeps
    // Continue on the main menu when there is nothing to continue.
    {
        Row row(column.row(rowHeight), ui.theme().spacing);
        ui.label(row.cell(190.0f),
                 "Autosave every",
                 settings.autosaveEnabled ? ui.theme().textDim : ui.theme().textDisabled,
                 ui.theme().bodyStyle);
        std::snprintf(buffer, sizeof(buffer), "%.0f min", static_cast<double>(settings.autosaveMinutes));
        const Rect valueBox = row.cellFromRight(56.0f);
        if (settings.autosaveEnabled) {
            (void)ui.slider(row.remaining(), "Autosave every", settings.autosaveMinutes, 1.0f, 30.0f);
        } else {
            (void)row.remaining();
        }
        ui.label(valueBox,
                 buffer,
                 settings.autosaveEnabled ? ui.theme().textPrimary : ui.theme().textDisabled,
                 ui.theme().smallStyle,
                 TextAlign::Right);
    }
    {
        // A count, so it steps rather than slides: an autosave ring of 2.7 is
        // not a thing, and a float slider that renders as "3" while holding
        // 3.4 is the kind of quiet lie the settings file then records.
        Row row(column.row(rowHeight), ui.theme().spacing);
        ui.label(row.cell(190.0f),
                 "Autosaves to keep",
                 settings.autosaveEnabled ? ui.theme().textDim : ui.theme().textDisabled,
                 ui.theme().bodyStyle);
        std::snprintf(buffer, sizeof(buffer), "%d", settings.autosaveKeep);
        const Rect valueBox = row.cellFromRight(56.0f);
        const Rect track = row.remaining();
        float keep = static_cast<float>(settings.autosaveKeep);
        if (settings.autosaveEnabled && ui.slider(track, "Autosaves to keep", keep, 1.0f, 10.0f)) {
            settings.autosaveKeep = static_cast<int>(keep + 0.5f);
        }
        ui.label(valueBox,
                 buffer,
                 settings.autosaveEnabled ? ui.theme().textPrimary : ui.theme().textDisabled,
                 ui.theme().smallStyle,
                 TextAlign::Right);
    }
    if (settings.autosaveEnabled) {
        (void)ui.checkbox(column.row(rowHeight), "Autosave when docking", settings.autosaveOnDock);
    } else {
        ui.label(
            column.row(rowHeight), "Autosave when docking", ui.theme().textDisabled, ui.theme().bodyStyle);
    }

    column.skip(6.0f);
    MenuAction action = MenuAction::None;
    if (ui.button(column.row(kButtonHeight), "Controls...")) {
        action = MenuAction::OpenControls;
    }
    if (ui.button(column.row(kButtonHeight), "Back") || ui.cancelRequested()) {
        action = MenuAction::CloseSettings;
    }

    ui.popId();
    return action;
}

// The CONTENTS of one save row: the name on the left, the date on the right,
// and a dim second line saying where and how far in.
//
// ⚑⚑ THIS DRAWS ONLY. The caller has already put a `selectable` with an empty
// label under this rectangle and read its click; drawing a second interactive
// widget over the same bounds - which the first draft of this function did -
// puts two entries in the nav order for one row and counts one click twice,
// which the UI turns into two ui-click cues and a double activation.
//
// Labels of our own rather than one long selectable label, because a row has
// two lines and three alignments. Cramming it into one string would also lose
// the elision that keeps a long save name inside its box.
void drawSaveRow(UiContext& ui, const Rect& bounds, const SaveSlot& slot)
{
    const float padding = 8.0f;
    const Rect inner = {{bounds.min.x + padding, bounds.min.y + 4.0f},
                        {bounds.max.x - padding, bounds.max.y - 4.0f}};
    Column lines(inner, 0.0f, 2.0f);

    {
        Row top(lines.row(18.0f), ui.theme().spacing);
        // The date first, from the right, so the name gets whatever is left -
        // a name is variable and a date is not, so the date is what should
        // keep its width.
        const std::string date = formatSaveDate(slot.info.savedAtUnix);
        const Rect dateBox = top.cellFromRight(112.0f);
        (void)ui.labelElided(top.remaining(),
                             slot.info.displayName.empty() ? "(unnamed)" : slot.info.displayName,
                             ui.theme().textPrimary,
                             ui.theme().bodyStyle);
        // An unreadable stamp prints NOTHING rather than 1970: an empty cell
        // is honest about not knowing, a wrong date looks like a right one.
        if (!date.empty()) {
            ui.label(dateBox, date, ui.theme().textDim, ui.theme().smallStyle, TextAlign::Right);
        }
    }

    char detail[160] = {};
    const char* kind = slot.kind == SaveKind::Auto    ? "Autosave"
                       : slot.kind == SaveKind::Quick ? "Quicksave"
                                                      : "Manual";
    (void)std::snprintf(detail,
                        sizeof(detail),
                        "%s  -  %s  -  %.0f cr  -  %s%s",
                        kind,
                        slot.info.systemName.c_str(),
                        slot.info.credits,
                        formatPlaytime(slot.info.worldSeconds).c_str(),
                        slot.info.hardcore ? "  -  HARDCORE" : "");
    (void)ui.labelElided(lines.row(16.0f),
                         detail,
                         slot.info.hardcore ? ui.theme().negative : ui.theme().textDim,
                         ui.theme().smallStyle);
}

MenuAction buildSaveBrowser(UiContext& ui,
                            const SaveCatalog& catalog,
                            SaveBrowserState& state,
                            std::string_view activeCampaign)
{
    dimBackground(ui);
    ui.pushId("save_browser");

    const bool saving = state.mode == SaveBrowserMode::Save;
    const float spacing = ui.theme().spacing;
    const float padding = ui.theme().padding;
    constexpr float kPanelWidth = 760.0f;
    constexpr float kListHeight = 340.0f;
    constexpr float kRowHeight = 46.0f;
    constexpr float kCampaignRowHeight = 30.0f;

    const std::vector<Campaign>& campaigns = catalog.campaigns();

    // ⚑ THE SELECTION IS CLAMPED BEFORE ANYTHING READS IT, EVERY FRAME. The
    // catalog is rescanned behind this screen after every save and every
    // delete, so an index that was valid when the player clicked can name a
    // row that no longer exists - which is how a delete button ends up acting
    // on the wrong file. Clamping here means every read below is in range.
    if (saving) {
        // Save mode writes into the run in progress and nowhere else. Pinning
        // the campaign rather than offering the list is the whole guard: there
        // is no sequence of clicks that files run A's save under run B.
        state.campaign = -1;
        for (std::size_t i = 0; i < campaigns.size(); ++i) {
            if (campaigns[i].name == activeCampaign) {
                state.campaign = static_cast<int>(i);
                break;
            }
        }
    }
    if (campaigns.empty()) {
        state.campaign = -1;
    } else if (state.campaign < 0 || state.campaign >= static_cast<int>(campaigns.size())) {
        state.campaign = saving ? -1 : 0;
    }
    const Campaign* campaign =
        state.campaign >= 0 ? &campaigns[static_cast<std::size_t>(state.campaign)] : nullptr;
    const std::size_t saveCount = campaign == nullptr ? 0 : campaign->saves.size();
    if (state.save >= static_cast<int>(saveCount)) {
        state.save = -1;
        state.disarm(); // an armed delete must never survive onto another row
    }

    const float extraRows = saving ? 1.0f : 0.0f; // the name field
    const float height = kTitleHeight + kListHeight + 24.0f + 34.0f * extraRows + kButtonHeight * 2.0f +
                         spacing * (7.0f + extraRows) + padding * 2.0f;
    const Rect panel = centeredPanel(ui, kPanelWidth, height);
    ui.panel(panel);

    Column column(panel, padding, spacing);
    ui.label(column.row(kTitleHeight),
             saving ? "Save Game" : "Load Game",
             ui.theme().textPrimary,
             ui.theme().headingStyle,
             TextAlign::Center);

    MenuAction action = MenuAction::None;

    // --- the two lists ------------------------------------------------------
    const Rect lists = column.row(kListHeight);
    Rect saveList = lists;
    if (!saving) {
        // Load mode gets a campaign column; Save mode does not, because there
        // is nothing to choose.
        constexpr float kCampaignWidth = 230.0f;
        const Rect campaignList = {{lists.min.x, lists.min.y}, {lists.min.x + kCampaignWidth, lists.max.y}};
        saveList = {{lists.min.x + kCampaignWidth + spacing, lists.min.y}, {lists.max.x, lists.max.y}};

        ui.panel(campaignList);
        const Rect inner = sol::ui::inset(campaignList, 4.0f);
        const float contentHeight = static_cast<float>(campaigns.size()) * (kCampaignRowHeight + 4.0f) + 4.0f;
        const Rect content = ui.beginScroll(inner, contentHeight, state.campaignScroll);
        Column rows(content, 0.0f, 4.0f);
        for (std::size_t i = 0; i < campaigns.size(); ++i) {
            ui.pushId(static_cast<int>(i));
            const bool selected = static_cast<int>(i) == state.campaign;
            // A campaign's own row says how many saves it holds, because an
            // empty campaign is listed too and would otherwise look identical
            // to a full one.
            char label[96] = {};
            (void)std::snprintf(
                label, sizeof(label), "%s  (%zu)", campaigns[i].name.c_str(), campaigns[i].saves.size());
            if (ui.selectable(rows.row(kCampaignRowHeight), label, selected)) {
                if (state.campaign != static_cast<int>(i)) {
                    state.campaign = static_cast<int>(i);
                    state.save = -1;
                    state.saveScroll = 0.0f;
                    state.disarm();
                    state.notice.clear();
                }
            }
            ui.popId();
        }
        ui.endScroll();
    }

    ui.panel(saveList);
    if (campaign == nullptr) {
        ui.label(sol::ui::inset(saveList, 12.0f),
                 campaigns.empty() ? "No saved games yet." : "Select a campaign.",
                 ui.theme().textDim,
                 ui.theme().bodyStyle,
                 TextAlign::Center);
    } else if (campaign->saves.empty()) {
        ui.label(sol::ui::inset(saveList, 12.0f),
                 saving ? "No saves in this run yet - name one below." : "This run has no saves.",
                 ui.theme().textDim,
                 ui.theme().bodyStyle,
                 TextAlign::Center);
    } else {
        const Rect inner = sol::ui::inset(saveList, 4.0f);
        const float contentHeight = static_cast<float>(saveCount) * (kRowHeight + 4.0f) + 4.0f;
        const Rect content = ui.beginScroll(inner, contentHeight, state.saveScroll);
        Column rows(content, 0.0f, 4.0f);
        for (std::size_t i = 0; i < saveCount; ++i) {
            const Rect bounds = rows.row(kRowHeight);
            const bool selected = static_cast<int>(i) == state.save;
            ui.pushId(static_cast<int>(i) + 1000); // clear of the campaign ids
            const bool clicked = ui.selectable(bounds, "", selected);
            ui.popId();
            drawSaveRow(ui, bounds, campaign->saves[i]);
            if (clicked && state.save != static_cast<int>(i)) {
                state.save = static_cast<int>(i);
                state.disarm();
                state.notice.clear();
            }
        }
        ui.endScroll();
    }

    // --- the name field, Save mode only -------------------------------------
    if (saving) {
        Row row(column.row(34.0f), spacing);
        ui.label(row.cell(70.0f), "Name", ui.theme().textDim, ui.theme().bodyStyle);
        if (state.focusRequested) {
            ui.setFocus(ui.idFor("save_name"));
            ui.setCaret(state.newSaveName.size());
            state.focusRequested = false;
        }
        (void)ui.textField(row.remaining(), "save_name", state.newSaveName, 40);
    }

    // --- one line of feedback ----------------------------------------------
    ui.label(column.row(24.0f), state.notice, ui.theme().textDim, ui.theme().smallStyle);

    // --- buttons ------------------------------------------------------------
    const SaveSlot* picked = (campaign != nullptr && state.save >= 0)
                                 ? &campaign->saves[static_cast<std::size_t>(state.save)]
                                 : nullptr;
    {
        Row row(column.row(kButtonHeight), spacing);
        if (saving) {
            // Saving needs a campaign to save into; the name may be blank and
            // the save is simply called "(unnamed)" - refusing a blank name
            // would stop somebody hitting Save twice in a hurry.
            if (ui.button(row.cell(160.0f), "Save", campaign != nullptr)) {
                action = MenuAction::SaveSelected;
            }
        } else {
            if (ui.button(row.cell(160.0f), "Load", picked != nullptr)) {
                action = MenuAction::LoadSelected;
            }
        }

        // ⚑ THE TWO DELETES, AND BOTH ARE TWO-PRESS. The label CHANGES to say
        // what the second press will do, rather than a separate confirm dialog
        // - a dialog is another screen to build and another Escape to route,
        // and a button that has visibly changed its mind is a confirmation the
        // player cannot miss and cannot mis-click through.
        const bool armedSave = state.pending == SaveBrowserState::Pending::Save;
        if (ui.button(
                row.cell(190.0f), armedSave ? "Delete - are you sure?" : "Delete Save", picked != nullptr)) {
            if (armedSave) {
                action = MenuAction::DeleteSelectedSave;
            } else {
                state.pending = SaveBrowserState::Pending::Save;
                state.notice = "Press again to delete this save. It cannot be undone.";
            }
        }
        if (!saving) {
            const bool armedCampaign = state.pending == SaveBrowserState::Pending::Campaign;
            if (ui.button(row.cell(220.0f),
                          armedCampaign ? "Delete run - are you sure?" : "Delete Run",
                          campaign != nullptr)) {
                if (armedCampaign) {
                    action = MenuAction::DeleteSelectedCampaign;
                } else {
                    state.pending = SaveBrowserState::Pending::Campaign;
                    state.notice = campaign == nullptr ? std::string()
                                                       : "Press again to delete '" + campaign->name +
                                                             "' and every save in it. It cannot be undone.";
                }
            }
        }
    }

    if (ui.button(column.row(kButtonHeight), "Back") || ui.cancelRequested()) {
        action = MenuAction::CloseBrowser;
    }

    ui.popId();
    return action;
}

MenuAction buildControlsScreen(UiContext& ui,
                               Settings& settings,
                               ControlsScreenState& state,
                               sol::platform::InputChord captured,
                               bool cancel)
{
    using sol::platform::BindingTable;
    using sol::platform::InputChord;

    dimBackground(ui);
    ui.pushId("controls");

    constexpr float kRowHeight = 30.0f;
    constexpr float kGroupHeight = 28.0f;
    constexpr float kPanelWidth = 620.0f;
    const float spacing = ui.theme().spacing;
    const float padding = ui.theme().padding;

    // Escape abandons an armed capture rather than leaving the screen - which
    // is exactly why Escape is a reserved chord and can never be bound.
    if (cancel && state.capturing < kActionCount) {
        state.capturing = kActionCount;
        state.notice = "Rebind cancelled.";
        cancel = false;
    }

    // A capture consumes the first chord that goes down. Reserved chords are
    // refused here rather than assigned: the conflict policy does not apply to
    // them, because the player must never be able to lock themselves out of
    // the screen they are standing in.
    if (state.capturing < kActionCount && captured.bound()) {
        const Action target = static_cast<Action>(state.capturing);
        if (isReservedChord(captured)) {
            state.notice = std::string(sol::platform::chordName(captured)) +
                           " is reserved by the menus and cannot be bound.";
        } else {
            const std::uint32_t stolen = settings.bindings.assign(state.capturing, captured);
            if (stolen == BindingTable::kNoAction) {
                state.notice =
                    std::string(actionLabel(target)) + " is now " + sol::platform::chordName(captured) + ".";
                if (state.stolenFrom == state.capturing) {
                    state.stolenFrom = kActionCount; // this row has a key again
                }
            } else {
                state.stolenFrom = stolen;
                state.notice = std::string(actionLabel(target)) + " took " +
                               sol::platform::chordName(captured) + " from " +
                               actionLabel(static_cast<Action>(stolen)) + ", which now needs a key.";
            }
            state.capturing = kActionCount;
        }
    }

    // Height: one row per action, plus a header per group.
    const float contentHeight = static_cast<float>(kActionCount) * (kRowHeight + 4.0f) +
                                static_cast<float>(ActionGroup::Count) * (kGroupHeight + 4.0f) + 4.0f;

    const float listHeight = 400.0f;
    const float panelHeight =
        kTitleHeight + listHeight + 26.0f + kButtonHeight * 2.0f + spacing * 5.0f + padding * 2.0f;
    const Rect panel = centeredPanel(ui, kPanelWidth, panelHeight);
    ui.panel(panel);

    Column column(panel, padding, spacing);
    ui.label(column.row(kTitleHeight),
             "Controls",
             ui.theme().textPrimary,
             ui.theme().headingStyle,
             TextAlign::Center);

    const Rect listBounds = column.row(listHeight);
    const Rect content = ui.beginScroll(listBounds, contentHeight, state.scroll);
    Column rows(content, 0.0f, 4.0f);

    ActionGroup drawn = ActionGroup::Count;
    for (std::uint32_t i = 0; i < kActionCount; ++i) {
        const Action action = static_cast<Action>(i);
        const ActionGroup group = actionGroup(action);
        if (group != drawn) {
            drawn = group;
            ui.label(
                rows.row(kGroupHeight), actionGroupLabel(group), ui.theme().textDim, ui.theme().smallStyle);
        }

        Row row(rows.row(kRowHeight), spacing);
        ui.label(row.cell(210.0f), actionLabel(action), ui.theme().textPrimary, ui.theme().bodyStyle);

        const InputChord chord = settings.bindings.chordFor(i);
        const bool capturing = state.capturing == i;
        // An unbound action reads as a dash, and one a steal just emptied says
        // so in accent - the player has to be able to see what the last
        // assignment cost them without reading the notice line. The value cell
        // is 260 px because the capture prompt is the longest thing it ever
        // holds and ui.label does not clip: at 220 it ran under the button.
        const char* value = capturing               ? "press any key or button"
                            : chord.bound()         ? sol::platform::chordName(chord)
                            : state.stolenFrom == i ? "needs a key"
                                                    : "--";
        const sol::ui::Color valueColor =
            capturing || !chord.bound() ? ui.theme().accent : ui.theme().textPrimary;
        ui.label(row.cell(260.0f), value, valueColor, ui.theme().bodyStyle);

        ui.pushId(static_cast<int>(i));
        if (ui.button(row.remaining(), capturing ? "Cancel" : "Rebind")) {
            // One capture armed at a time: clicking a second row moves the
            // arm rather than leaving two rows waiting for the same keypress.
            state.capturing = capturing ? kActionCount : i;
            state.notice.clear();
        }
        ui.popId();
    }
    ui.endScroll();

    ui.label(column.row(26.0f), state.notice, ui.theme().textDim, ui.theme().smallStyle);

    MenuAction action = MenuAction::None;
    if (ui.button(column.row(kButtonHeight), "Reset to Defaults")) {
        installDefaultBindings(settings.bindings);
        state.capturing = kActionCount;
        state.stolenFrom = kActionCount;
        state.notice = "Controls reset to defaults.";
    }
    // Backing out is refused while a capture is armed, so the one key that
    // cancels a capture cannot also drop the player out of the screen.
    if (ui.button(column.row(kButtonHeight), "Back") || cancel) {
        if (state.capturing >= kActionCount) {
            state.notice.clear();
            action = MenuAction::CloseControls;
        }
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
    ui.label(column.row(30.0f),
             prompt.full ? "Too many bookmarks here" : "Bookmark this place",
             ui.theme().textPrimary,
             ui.theme().headingStyle);
    ui.label(column.row(20.0f), prompt.whereSummary, ui.theme().textDim, ui.theme().smallStyle);
    column.skip(4.0f);

    if (prompt.full) {
        ui.label(column.row(20.0f),
                 "Delete one from the system map first.",
                 ui.theme().textDim,
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
    if (prompt.nameIsSuggestion && !ui.input().text.empty() && ui.isFocused(ui.idFor("name"))) {
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
