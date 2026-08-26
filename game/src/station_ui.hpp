#pragma once

#include "space_world.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/ui/screens.hpp"

#include <deque>
#include <string>
#include <vector>

namespace game {

// Fills the Phase 8a/8b tabs of the docked-station panel (outfitting,
// shipyard, crew, factions) from the world and defs. Catalogs show only what
// the owner faction sells the player (Phase 8b gates). `text` backs the
// generated detail strings — a deque so growth never moves entries the rows
// already point at. All row vectors are cleared and rebuilt; spans in
// `panel` are set to them.
void fillStationOutfitting(const SpaceWorld& world,
                           const sol::assets::DefDatabase& defs,
                           std::deque<std::string>& text,
                           sol::ui::StationPanel& panel,
                           std::vector<sol::ui::OutfitRow>& moduleRows,
                           std::vector<sol::ui::OutfitRow>& weaponRows,
                           std::vector<sol::ui::OutfitRow>& crewCatalogRows,
                           std::vector<sol::ui::OutfitRow>& crewAboardRows,
                           std::vector<sol::ui::OutfitRow>& shipRows,
                           std::vector<sol::ui::FleetRow>& fleetRows,
                           std::vector<sol::ui::FactionRow>& factionRows);

// Fills the Missions tab (Phase 8c): the board's offers (with the min_rep
// tier gate evaluated against player standing) and the journal.
void fillStationMissions(const SpaceWorld& world,
                         std::deque<std::string>& text,
                         sol::ui::StationPanel& panel,
                         std::vector<sol::ui::MissionRow>& offerRows,
                         std::vector<sol::ui::MissionRow>& journalRows);

// How long ago a market reading was taken, for the intel columns (Phase 8g):
// "just now", "14m ago", "2h ago". Coarse on purpose — the number that
// matters is the order of magnitude, not the second.
[[nodiscard]] std::string formatAge(double seconds);

// Executes the (at most one) station-panel click of this frame.
void executeStationAction(SpaceWorld& world, const sol::ui::StationAction& action);

} // namespace game
