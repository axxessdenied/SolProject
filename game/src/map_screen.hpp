#pragma once

#include "sol/ui/context.hpp"
#include "sol/ui/screens.hpp"

namespace game {

// What the player has open on the map, and where each list is scrolled.
// main.cpp owns one across frames; the screen itself is rebuilt every frame.
struct MapScreenState
{
    enum Tab : int
    {
        Galaxy = 0,
        System,
        TabCount,
    };

    int tab = Galaxy;
    float scroll[TabCount] = {};
    int selectedSystem = -1; // row in MapPanel::systems; -1 = the current one
    int selectedMarker = -1;
};

// The map screen (engine plan Phase 8e, deferred here out of Phase 8d): a
// galaxy view over the lane graph and a system view of the playfield. Reads
// only what the game says the player knows, and reports what they did through
// `panel.action` - the same fill-then-execute seam the station screen uses.
//
// Returns true on the frame the player asked to close the map.
[[nodiscard]] bool buildMapScreen(sol::ui::UiContext& ui, sol::ui::MapPanel& panel,
                                  MapScreenState& state);

} // namespace game
