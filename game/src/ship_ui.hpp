#pragma once

#include "space_world.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/ui/screens.hpp"

#include <deque>
#include <string>
#include <vector>

namespace game {

// Fills the ship information screen (engine plan Phase 8h) from the world -
// the same fill-then-execute seam the station and map screens use, except
// this one has nothing to execute: the screen is read-only.
//
// `text` backs every prebuilt string the panel points at and must outlive the
// draw; the row vectors likewise back the panel's spans.
void fillShipInfoPanel(const SpaceWorld& world, const sol::assets::DefDatabase& defs,
                       std::deque<std::string>& text, sol::ui::ShipInfoPanel& panel,
                       std::vector<sol::ui::InfoRow>& flightRows,
                       std::vector<sol::ui::InfoRow>& defenceRows,
                       std::vector<sol::ui::InfoRow>& utilityRows,
                       std::vector<sol::ui::InfoRow>& fittedRows,
                       std::vector<sol::ui::InfoRow>& cargoRows);

// The same numbers as one block of text, for `sol.ship_info` - which is how
// the screen gets verified without reading pixels.
[[nodiscard]] std::string shipInfoReport(const SpaceWorld& world,
                                        const sol::assets::DefDatabase& defs);

} // namespace game
