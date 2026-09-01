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
                           std::vector<sol::ui::MountRow>& mountRows,
                           std::vector<sol::ui::OutfitRow>& componentRows,
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

// Which room the player is standing in: the recreation module on this station
// with the largest `power_draw`, or nullptr where it has none (Phase 35 stage A).
//
// ⚑⚑⚑ EXPOSED BECAUSE IT IS A RULING RATHER THAN A DETAIL, THE SAME REASON
// `stationTabOnStrip` AND `SpaceWorld::shadowOperatorFor` ARE. Two decisions are
// packed into one line and both are arguable: that a station has ONE room rather
// than a list of them, and that the ladder between rooms is `power_draw` - 2, 3,
// 4, 5, 8 across bar, restaurant, concourse, casino, resort - rather than a
// field of its own. The second is what keeps stage B's "how far the talk
// reaches" derivable from something already authored and already load-bearing,
// instead of from a second number that can drift out of step with the first.
//
// ⚑ A rule this consequential should be callable by a test rather than live
// inside the fill, where only a screenshot could reach it - and the shipped
// galaxy does not exercise the whole ladder, so a test has to be able to hand it
// the cases the galaxy declines to produce.
[[nodiscard]] const sol::assets::ModuleDef* stationRoom(const SpaceWorld& world,
                                                        const sol::assets::DefDatabase& defs,
                                                        std::uint32_t system,
                                                        std::uint32_t station);

// Fills the Bar tab (Phase 35 stage A): the room the player is standing in and
// what the house has to say about the dock it is standing on.
//
// ⚑⚑ EVERY LINE IS TRUE AT t=0, WHICH IS THE WHOLE REASON THIS SOURCE SHIPS
// BEFORE THE LIVE-GALAXY ONE. Measured over two sim hours, the mission sim's
// shortage, raid and contest enumerators are all EMPTY at t=0 - a fresh galaxy
// stocks every market at half capacity and nobody has raided anybody - so a bar
// built only on stage B's sources would be silent at exactly the moment a new
// player first walks into one.
//
// ⚑ It is also the first surface any of Phase 34's composition has ever had:
// what a station cannot hold, what plant it was fitted with, and who runs its
// fence were all measurable and all invisible.
void fillStationBar(const SpaceWorld& world,
                    const sol::assets::DefDatabase& defs,
                    std::deque<std::string>& text,
                    sol::ui::StationPanel& panel,
                    std::vector<sol::ui::InfoRow>& talkRows);

// How long ago a market reading was taken, for the intel columns (Phase 8g):
// "just now", "14m ago", "2h ago". Coarse on purpose — the number that
// matters is the order of magnitude, not the second.
[[nodiscard]] std::string formatAge(double seconds);

// Executes the (at most one) station-panel click of this frame.
void executeStationAction(SpaceWorld& world, const sol::ui::StationAction& action);

} // namespace game
