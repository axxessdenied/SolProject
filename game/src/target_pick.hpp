#pragma once

#include "sol/core/math/vec.hpp"
#include "sol/ui/screens.hpp"

#include <cstddef>
#include <vector>

namespace game {

class SpaceWorld;

// Click-to-select (engine plan Phase 8j), and the radar fill it has to agree
// with. The two live in one file deliberately: a click on the disc can only
// be answered against the exact list of contacts the disc was drawn from —
// nearest-first, capped, same skips — so the fill and the hit test are kept
// where they can be read together rather than drifting apart in two files.

// Everything around the ship, in ship-local meters (+x right, +y up, -z
// forward). Individual rocks are deliberately absent: a field of forty-eight
// is one contact, the same ruling Phase 8f made when it put rock on the
// boresight instead of the target cycle.
void fillRadarContacts(const SpaceWorld& world, std::vector<sol::ui::RadarContact>& out);

// Which surface a click was answered by, so the console can report the routing
// and not just the result.
enum class PickRoute
{
    Miss,
    Radar,
    Space,
};

struct PickResult
{
    PickRoute route = PickRoute::Miss;
    std::size_t target = 0; // a selection index, valid only when route != Miss
};

// What a click at `cursor` (VIRTUAL UI pixels, i.e. the raw cursor divided by
// the UI scale) would select, read against the world's current ViewFrame.
// Inside the radar disc it is a radar pick and outside it a world pick;
// nothing is selected here.
[[nodiscard]] PickResult pickTarget(const SpaceWorld& world, sol::core::Vec2 cursor);

// The same pick at the boresight — what the click does while the cursor is
// captured for mouse-look, where the cursor position is meaningless by
// contract and "target what I am pointing at" is what the player wants.
[[nodiscard]] PickResult pickBoresight(const SpaceWorld& world);

// Performs `result` against the world and logs the same one-line selection
// message T/C/O/H print. A miss changes nothing — losing a target to a stray
// click is worse than a click that does nothing — and returns false.
bool selectPicked(SpaceWorld& world, const PickResult& result);

} // namespace game
