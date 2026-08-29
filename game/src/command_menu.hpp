#pragma once

// The context menu's contents (engine plan Phase 28 stage B).
//
// ⚑ Its own translation unit rather than a corner of game_ui.cpp, because
// game_ui.hpp is deliberately "pure geometry building - no input, no game state
// mutation - so it can be exercised headlessly", and a menu whose entries
// depend on what is targeted is exactly game state. The split is the same one
// the HUD already draws: game_ui builds pictures, this decides what to offer.
//
// ⚑⚑ STAGE B OFFERS THE STAGE-A VOCABULARY AND NOTHING ELSE. Stage C is the one
// that asks the world what was PICKED and composes the entries from it — dock,
// hail, scan, and the commands together. What stage B owns is the seam: a menu
// that opens where you clicked, offers real commands, greys out the ones that
// do not apply and says why, and applies the one you choose.

#include "space_world.hpp"

#include "sol/core/math/vec.hpp"
#include "sol/ui/context.hpp"

#include <span>

namespace game {

// One offer. `mode` is what engaging it does; CommandMode::None means the entry
// cancels whatever is running instead of starting something.
struct CommandMenuEntry
{
    CommandMode mode = CommandMode::None;
    const char* label = "";
};

// The entries, in menu order. A free function returning a fixed span because
// the list is a property of the vocabulary, not of any one menu instance —
// which is what lets a test assert the menu and the key bindings offer the
// same set without duplicating it.
[[nodiscard]] std::span<const CommandMenuEntry> commandMenuEntries();

// Whether an entry can be chosen right now, and the reason when it cannot.
// Separated from the drawing so the rule can be tested without a UI context:
// this is decision 3 ("shown DISABLED with its reason, never hidden") expressed
// as a predicate rather than as a colour.
[[nodiscard]] bool
commandMenuEntryEnabled(const SpaceWorld& world, const CommandMenuEntry& entry, const char*& reason);

// Draws the menu at `anchor` and returns the index of the entry picked this
// frame, or -1. `boundsOut` receives the box it occupied, which the caller
// needs for the close-on-a-click-elsewhere rule.
[[nodiscard]] int buildCommandMenu(sol::ui::UiContext& ui,
                                   const SpaceWorld& world,
                                   sol::core::Vec2 anchor,
                                   sol::ui::Rect& boundsOut);

// Applies the entry at `index`. Out-of-range is a no-op rather than an abort:
// the index came from a menu that was built from the same list, but a caller
// that gets it wrong should not take the game down.
void applyCommandMenu(SpaceWorld& world, int index);

} // namespace game
