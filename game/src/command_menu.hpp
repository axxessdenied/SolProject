#pragma once

// The context menu's contents (engine plan Phase 28 stages B and C).
//
// ⚑ Its own translation unit rather than a corner of game_ui.cpp, because
// game_ui.hpp is deliberately "pure geometry building - no input, no game state
// mutation - so it can be exercised headlessly", and a menu whose entries
// depend on what is targeted is exactly game state. The split is the same one
// the HUD already draws: game_ui builds pictures, this decides what to offer.
//
// ⚑⚑ STAGE C IS THE ONE THAT ASKS THE WORLD WHAT WAS PICKED. Stage B offered
// the stage-A vocabulary and nothing else; the menu now composes itself from
// what is selected — dock, hail and the scan pulse alongside the commands, each
// row carrying the fact that governs it.
//
// ⚑⚑⚑ APPLICABILITY IS NOT AVAILABILITY, AND THE DIFFERENCE IS WHAT MAKES THIS
// A CONTEXT MENU. Phase 28 decision 3 says a row that does not apply is shown
// DISABLED with its reason, never hidden — and that rule governs whether a row
// can be chosen RIGHT NOW ("Request Docking - 412.4 km", greyed out, teaches
// the 20 km rule). It does not govern whether the row belongs to this thing at
// all: "Hail - not a ship" greyed out on every station for ever teaches
// nothing, and a menu that is identical everywhere is not a context menu. So a
// verb is OFFERED when it applies to the kind of thing picked, and among the
// verbs offered, decision 3 decides which are live.

#include "space_world.hpp"

#include "sol/core/math/vec.hpp"
#include "sol/ui/context.hpp"

#include <span>
#include <string>
#include <vector>

namespace game {

// What choosing a row does. Everything but Command is a world verb that already
// existed and already has a key: the menu is a second way of reaching them, not
// a second implementation of them.
enum class CommandMenuAction : std::uint32_t
{
    Command = 0,    // engage `mode`, or cancel when it is CommandMode::None
    RequestDocking, // SpaceWorld::requestDocking
    Hail,           // SpaceWorld::hailTarget
    ScanPulse,      // SpaceWorld::pulseScan
};

// One offer. `mode` is meaningful only for CommandMenuAction::Command, where
// CommandMode::None means the entry cancels whatever is running instead of
// starting something.
struct CommandMenuEntry
{
    CommandMenuAction action = CommandMenuAction::Command;
    CommandMode mode = CommandMode::None;
    const char* label = "";
};

// One composed row: what it does, how it reads against the world right now, and
// whether it can be chosen. The label carries the FACT ("Request Docking -
// 412.4 km") and the reason carries the RULE ("within 20 km"), because the fact
// is what the player is watching change as they close and the rule is what they
// need explained once.
struct CommandMenuRow
{
    CommandMenuEntry entry;
    std::string label;
    bool enabled = true;
    std::string reason; // tooltip while the row is disabled
};

// The command vocabulary, in menu order. A free function returning a fixed span
// because the list is a property of the vocabulary, not of any one menu
// instance — which is what lets a test assert the menu and the key bindings
// offer the same set without duplicating it. These rows are in EVERY menu: a
// manoeuvre is about the ship, so there is no thing it fails to apply to.
[[nodiscard]] std::span<const CommandMenuEntry> commandMenuEntries();

// Whether an entry belongs in a menu opened on what is currently selected —
// the KIND rule. Docking is a thing you ask a station; hailing is a thing you
// ask a pilot; a manoeuvre and a scan pulse are things you tell your own ship,
// so they apply to everything, including a right-click that hit nothing.
[[nodiscard]] bool commandMenuEntryApplies(const SpaceWorld& world, const CommandMenuEntry& entry);

// Whether an entry that applies can be chosen right now, and the reason when it
// cannot — the RIGHT-NOW rule. Separated from the drawing so it can be tested
// without a UI context: this is decision 3 expressed as a predicate rather than
// as a colour.
[[nodiscard]] bool
commandMenuEntryEnabled(const SpaceWorld& world, const CommandMenuEntry& entry, std::string& reason);

// The whole menu for whatever is selected: the verbs that apply to it, then the
// command block. Public so the composition can be asserted headlessly — a test
// can right-click a station and a ship and read what each offers with no font,
// no context and no window.
void fillCommandMenu(const SpaceWorld& world, std::vector<CommandMenuRow>& out);

// What the menu says it is about. The selection's name, or "" with nothing
// selected — a menu with no heading is still a menu of manoeuvres.
[[nodiscard]] std::string commandMenuTitle(const SpaceWorld& world);

// What the player chose this frame, if anything.
//
// ⚑ THE ENTRY TRAVELS, NOT AN INDEX. Stage B returned a row number and had the
// caller pass it back to be applied against a list rebuilt from scratch. That
// was safe only because the world could not change between the two calls;
// composing the rows from the selection makes the list vary, and a stale index
// into a varying list is the bug that picks the wrong verb. There is now no
// index outside the one function that draws.
struct CommandMenuPick
{
    bool picked = false;
    CommandMenuEntry entry;
};

// Composes, draws at `anchor` and answers. `boundsOut` receives the box it
// occupied, which the caller needs for the close-on-a-click-elsewhere rule.
[[nodiscard]] CommandMenuPick buildCommandMenu(sol::ui::UiContext& ui,
                                               const SpaceWorld& world,
                                               sol::core::Vec2 anchor,
                                               sol::ui::Rect& boundsOut);

// Performs an entry. An entry the switch does not know is a no-op rather than
// an abort.
void applyCommandMenu(SpaceWorld& world, const CommandMenuEntry& entry);

} // namespace game
