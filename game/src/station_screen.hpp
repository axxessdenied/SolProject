#pragma once

#include "sol/ui/context.hpp"
#include "sol/ui/screens.hpp"

#include <string>

namespace game {

// What the player has open on the station screen, and where each list is
// scrolled. main.cpp owns one across frames: the screen itself is rebuilt every
// frame, so anything the player expects to persist between them lives here.
struct StationScreenState
{
    enum Tab : int
    {
        Trade = 0,
        Outfitting,
        Shipyard,
        Crew,
        Factions,
        Missions,
        Survey,
        Refinery,
        TabCount,
    };

    int tab = Trade;
    float scroll[TabCount] = {}; // per tab, so switching back keeps your place
    int tradeAmount = 1;         // index into the 1/10/100 amounts: starts at 10
    // Which mount the Outfitting tab's catalogs are aimed at, empty for "the
    // first free one that takes it" (Phase 31 stage B). It lives here rather
    // than in the panel because it is a thing the PLAYER is holding, not a
    // thing the world knows - the same reason `tab` and `tradeAmount` do.
    //
    // ⚑ The mount's ID rather than its index, and for decisions/014 rule 1's
    // own reason one layer up: an index is only meaningful against the list
    // that produced it, and this one outlives a switch to a different ship. An
    // id that no longer names a mount simply stops matching.
    std::string selectedMount;
};

// The docked-station screen (engine plan Phase 8d), drawn on the in-repo UI
// stack. Reads the panel the game already fills for the provisional dev screen
// and writes back through the same seam - `panel.action` and
// `panel.trade.action` - so no gameplay logic moves into the UI.
//
// Returns true on the frame the player asked to undock.
[[nodiscard]] bool
buildStationScreen(sol::ui::UiContext& ui, sol::ui::StationPanel& panel, StationScreenState& state);

} // namespace game
