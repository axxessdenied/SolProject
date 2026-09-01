#pragma once

#include "space_world.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/ui/screens.hpp"

#include <deque>
#include <span>
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

// How many lines about the wider galaxy this room is worth (Phase 35 stage B),
// derived from the same `power_draw` ladder `stationRoom` ranks by.
//
// ⚑⚑⚑ THE LADDER BUYS SENTENCES, NOT DISTANCE, AND THAT IS A CORRECTION TO
// STAGE A'S OWN NOTE. Stage A wrote that stage B would derive "how far the talk
// reaches" from this number. It cannot: `MissionParams::candidateReach` caps
// every candidate enumerator at THREE JUMPS for the mission board's reasons, so
// a five-rung ladder mapped onto distance would have to be crushed into three
// rungs and could not widen past the cap without moving a number the board also
// reads. What the ladder can buy without touching anything is how much gets
// SAID - which is also the lever this phase actually needs, because the problem
// is never generation. MEASURED: at two sim hours the median room stands on 155
// live candidates and the busiest on 219.
//
// ⚑ THE SHIPPED GALAXY BARELY EXERCISES IT, AND THE HONEST NUMBER IS HERE
// RATHER THAN IN A COMMIT MESSAGE: 60 of the 62 rooms are a bar or a restaurant
// and get one line, one concourse gets two and one resort gets three. Any
// monotone ladder starting at one does that, because 60 of 62 rooms sit on the
// bottom two rungs of an authored ladder this galaxy did not roll the top of.
[[nodiscard]] int roomTalkLines(const sol::assets::ModuleDef& room);

// One thing said in a room: the topic column and the sentence.
//
// ⚑⚑ THE TOPIC IS ALWAYS WRITTEN BY C++, NEVER BY THE `bar_talk` HOOK, AND
// THAT IS NOT THE SAME RULE AS "the engine picks the fact". It is the narrower
// one that the tab draws topics into a fixed 150px column: a closed vocabulary
// is what keeps five sentences of very different lengths reading as a list
// rather than as a paragraph, and a hook free to write it would be free to
// overflow it. The SENTENCE is the hook's, and the fact inside the sentence is
// the engine's.
struct BarLine
{
    std::string topic;
    std::string text;
    // Phase 35 stage D: this line is an offer of work and there is a lead
    // behind it, at `lead` in `MissionSim::leads()`. -1 for the other lines,
    // which is every line stages A to C composed.
    //
    // ⚑⚑ A FIELD ON THE LINE RATHER THAN A SECOND LIST NEXT TO IT, WHICH IS
    // THE OPPOSITE OF THE CALL MADE ONE LAYER DOWN IN `MissionSim` AND FOR THE
    // SAME REASON. Down there a lead needed its own list because the board's
    // list runs on a different clock; up here it needs to be ON a line because
    // the whole point is that the work is something a person in the room SAID.
    // A lead drawn under its own heading would be a second board with a
    // barstool in front of it.
    int lead = -1;
};

// What the house itself has to say about the dock it is standing on (Phase 35
// stage A): the room, then whose law reaches here, what the house has no hold
// for, what the lights run off, and who keeps a back room. `composeRoomLine`
// is separate because it is the establishing line and the talk about the wider
// galaxy is inserted between the two.
//
// ⚑⚑ EVERY LINE IS TRUE AT t=0, WHICH IS THE WHOLE REASON THIS SOURCE SHIPPED
// BEFORE THE LIVE-GALAXY ONE. Measured over two sim hours, the mission sim's
// shortage, raid and contest enumerators are all EMPTY at t=0 - a fresh galaxy
// stocks every market at half capacity and nobody has raided anybody - and even
// with stage B's four sources in, 23 of the 62 rooms have nothing live to say
// the moment a new player first walks into one.
//
// ⚑ It is also the first surface any of Phase 34's composition has ever had:
// what a station cannot hold, what plant it was fitted with, and who runs its
// fence were all measurable and all invisible.
//
// ⚑ PURE, AND CALLED ONCE PER DOCK RATHER THAN PER FRAME (stage B). Both
// halves matter: pure so a test can read the sentences without a screenshot,
// and once per dock because the talk beside it comes from a Lua hook and no
// hook in this game has ever run from a fill.
void composeRoomLine(const SpaceWorld& world,
                     const sol::assets::DefDatabase& defs,
                     std::uint32_t system,
                     std::uint32_t station,
                     std::vector<BarLine>& out);
void composeHouseTalk(const SpaceWorld& world,
                      const sol::assets::DefDatabase& defs,
                      std::uint32_t system,
                      std::uint32_t station,
                      std::vector<BarLine>& out);

// Draws the Bar tab from talk already composed (Phase 35 stage B).
//
// ⚑⚑ A PRESENTER, NOT A FILL, AND THE SEAM MOVED IN STAGE B FOR A REASON THE
// SPEC HAD NOT COUNTED. Every other tab is filled from live world state on
// every frame the panel is drawn, which is correct for a readout and wrong for
// a conversation - and impossible for this one, because `bar_talk` is a Lua
// hook and `GameContent::tick` is the only place this game runs one. RULED BY
// THE USER, 2026-09-01: what the house says is decided WHEN YOU WALK IN and
// does not move until you undock, which is `m_hails`' rule for a pilot who
// repeats themselves. The stated cost is that a system changing hands mid-dock
// leaves the law line and the back-room line stale while the Trade tab's
// jurisdiction header updates live.
void fillStationBar(std::span<const BarLine> talk,
                    const char* room,
                    const char* keeper,
                    std::deque<std::string>& text,
                    sol::ui::StationPanel& panel,
                    std::vector<sol::ui::InfoRow>& talkRows);

// How the room introduces whoever is behind the bar (Phase 35 stage C): a name,
// a trade, and - once you have been in before - the fact that they know it.
//
// ⚑⚑ THE ATTRIBUTION IS C++'s AND THE LINES ARE LUA'S, which is the same split
// stage B drew and is drawn again here for a narrower reason: this string is a
// HEADING and not a `BarLine`, so it is not spent out of the room's line budget
// and a hook cannot overflow it. What a character SAYS is `character_talk`'s.
//
// ⚑ `visits` is what the save carries, so this is where the save becomes
// visible: 0 is a stranger, 1 is somebody you met once, more is a regular.
[[nodiscard]] std::string composeKeeperLine(const char* name, const char* trade, std::uint32_t visits);

// How long ago a market reading was taken, for the intel columns (Phase 8g):
// "just now", "14m ago", "2h ago". Coarse on purpose — the number that
// matters is the order of magnitude, not the second.
[[nodiscard]] std::string formatAge(double seconds);

// Executes the (at most one) station-panel click of this frame.
void executeStationAction(SpaceWorld& world, const sol::ui::StationAction& action);

} // namespace game
