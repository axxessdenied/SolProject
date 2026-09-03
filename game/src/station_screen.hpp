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
        Bar,
        BlackMarket,
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
    // Which captain the Crew tab's ship list is aimed at, or -1 (Phase 39
    // stage A). An INDEX where `selectedMount` is an id, and the reason is the
    // opposite of that field's: a mount id outlives a switch to a different
    // ship, whereas a captain list is rebuilt every frame from one vector and
    // has no stable name to hold. A stale index simply stops matching and the
    // Give buttons grey themselves, which is the self-healing the fill relies
    // on rather than an invariant anybody has to maintain.
    int selectedCaptain = -1;
    // Which of `kSellFloors` the Crew tab's strip is on (Phase 39 stage E).
    // Screen state for `tradeAmount`'s exact reason - it is a thing the player
    // is holding, not a thing the world knows - and it does double duty: it is
    // the floor a new haul order is given, AND the control that re-aims the
    // floor on an order already standing.
    //
    // ⚑ It starts at 0 ("none"), so a player who never touches the strip gives
    // the order every haul got before this field existed.
    int sellFloor = 0;
};

// Whether a tab belongs on THIS station's strip (Phase 34 stage C).
//
// ⚑⚑⚑ EXPOSED BECAUSE IT IS A RULING RATHER THAN A DETAIL. The rule is one
// sentence - *a tab is on the strip when the station is equipped for it, or
// when the player's own half of it has something in it* - and it decides what
// roughly a third of the docks in the galaxy will and will not offer. A rule
// that consequential should have a name a test can call, rather than living
// inside the loop that draws the strip where only a click could reach it. The
// reasoning, and the two screens no station may withhold, are at the definition.
//
// `tab` is a `StationScreenState::Tab`; anything else is false.
[[nodiscard]] bool stationTabOnStrip(const sol::ui::StationPanel& panel, int tab);

// The docked-station screen (engine plan Phase 8d), drawn on the in-repo UI
// stack. Reads the panel the game already fills for the provisional dev screen
// and writes back through the same seam - `panel.action` and
// `panel.trade.action` - so no gameplay logic moves into the UI.
//
// Returns true on the frame the player asked to undock.
[[nodiscard]] bool
buildStationScreen(sol::ui::UiContext& ui, sol::ui::StationPanel& panel, StationScreenState& state);

} // namespace game
