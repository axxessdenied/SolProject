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
    // Which system the ship was in the last time this screen was built
    // (Phase 10). The map follows the player on a jump, and "has the player
    // moved?" is a question the screen cannot ask without remembering the
    // answer from last time - the out-of-range guards below only ever caught a
    // stale index, never a changed system. -1 means "never reconciled", so the
    // first build after a new game snaps too.
    int followedSystem = -1;

    // Zoom and pan, per tab so the two views keep their own framing
    // (Phase 8h). Applied in SCREEN space, after each view's own projection:
    // it magnifies exactly what the player is looking at, including the
    // crowded playfield bubble, and leaves the two-tier design and its extent
    // maths untouched. Label text stays at a fixed size and the de-collider
    // re-runs at the magnified positions, so zooming in does not merely
    // enlarge a crowded region - it un-crowds it.
    sol::core::Vec2 pan[TabCount] = {};
    float zoom[TabCount] = {1.0f, 1.0f};
    // Drag state; the anchor is where the cursor went down, in screen pixels.
    bool dragging = false;
    sol::core::Vec2 dragAnchor;
    sol::core::Vec2 dragPanStart;
    // Which commodity the galaxy map is colored by (Phase 8g); -1 shows
    // faction ownership, the way the map has always looked. Pure view state,
    // so it lives here rather than in the world.
    int tradeCommodity = -1;
};

// What main.cpp knows about the mouse that the map cannot work out for itself
// (Phase 28 stage D). Both fields exist so a rule is expressed ONCE for the
// whole game rather than a second time in here: this project has paid
// repeatedly for the second expression of a thing that already existed.
struct MapInput
{
    // The click-vs-drag verdict for the right button, reached in main.cpp with
    // the same `kClickSlopPixels` the flight view uses, and true on the frame
    // the button came up as a click rather than a sweep.
    bool rightClicked = false;
    // Where the button went DOWN, in virtual UI pixels - the flight view's
    // rule inherited whole, and the point this screen hit-tests, anchors from
    // and reports about.
    sol::core::Vec2 rightCursor;
    // ⚑⚑ AN OPEN MENU OWNS THE MOUSE. Flight only had a world pick to protect
    // (stage B); the map has a pan, a wheel zoom and a marker pick as well, and
    // all three sit under the box the menu draws in. Without this a click on a
    // menu row also drags the map and selects whatever the row was covering.
    bool commandMenuOpen = false;
};

// The map screen (engine plan Phase 8e, deferred here out of Phase 8d): a
// galaxy view over the lane graph and a system view of the playfield. Reads
// only what the game says the player knows, and reports what they did through
// `panel.action` - the same fill-then-execute seam the station screen uses.
//
// ⚑ That seam is why the context menu is not built in here (Phase 28 stage D):
// this file draws from a MapPanel and knows nothing of SpaceWorld, and the
// menu's rows are composed by asking the world what is selected. So the map
// answers WHAT WAS RIGHT-CLICKED and main.cpp builds the menu about it.
//
// Returns true on the frame the player asked to close the map.
[[nodiscard]] bool buildMapScreen(sol::ui::UiContext& ui,
                                  sol::ui::MapPanel& panel,
                                  MapScreenState& state,
                                  const MapInput& input);

} // namespace game
