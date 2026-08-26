#pragma once

#include "space_world.hpp"

#include "sol/ui/screens.hpp"

#include <deque>
#include <string>
#include <vector>

namespace game {

// Fills the map screen (engine plan Phase 8e) from the world and, crucially,
// from what SurveySim says the player knows: an unknown system contributes a
// row the screen refuses to draw, and ownership stays hidden until the player
// has actually been there. `text` backs the generated detail strings — a deque
// so growth never moves entries the rows already point at.
void fillMapPanel(const SpaceWorld& world,
                  std::deque<std::string>& text,
                  sol::ui::MapPanel& panel,
                  std::vector<sol::ui::MapSystemRow>& systemRows,
                  std::vector<sol::ui::MapLaneRow>& laneRows,
                  std::vector<sol::ui::MapMarkerRow>& markerRows);

// Executes the (at most one) map click of this frame. Returns true when the
// action engaged the autopilot, which the caller uses to leave the map.
bool executeMapAction(SpaceWorld& world, const sol::ui::MapAction& action);

} // namespace game
