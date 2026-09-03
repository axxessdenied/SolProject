// The fence (engine plan Phase 37 stage C): a back room inside somebody else's
// station, and the second owner the catalogue gate had no way to ask about.

#include "content.hpp"
#include "space_world.hpp"
#include "station_screen.hpp"
#include "station_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/synthetic_cooked_font.hpp>
#include <sol/test/test.hpp>
#include <sol/ui/context.hpp>

using sol::assets::DefDatabase;
using sol::assets::ModuleFamily;

namespace {

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] bool buildShippedGalaxy(const DefDatabase& defs, game::SpaceWorld& world)
{
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    if (!world.generateUniverse(defs)) {
        std::printf("  generateUniverse refused the shipped defs\n");
        return false;
    }
    return true;
}

// Where a new pilot wakes up, derived by `SpaceWorld`'s own rule rather than
// written down as an index: the first CORE system that has a station. Recomputed
// here for the reason every index in this directory is recomputed — assuming it
// is exactly the mistake a reachability probe would be measuring against.
[[nodiscard]] std::uint32_t startSystemOf(const game::SpaceWorld& world)
{
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        if (world.galaxy().systems[s].region == sol::sim::Region::Core &&
            !world.galaxy().systems[s].stations.empty()) {
            return s;
        }
    }
    return sol::sim::kNoFaction;
}

// ⚑⚑⚑⚑ ITS OWN BFS, AND THE REASON IS A TRAP WORTH THE FIFTEEN LINES.
// `MissionSim::jumpDepths` is the obvious tool and it is the WRONG ONE: it is
// capped at `MissionParams::candidateReach`, which is **3**, and it writes the
// same `0xff` for "further than that" as for "no route at all". Pointed at this
// probe it reported **7 of 8 fences in no-gate systems** in a galaxy of 81
// systems and 159 lanes - a number that reads as a broken generator and is
// actually a mission board's horizon. `castCandidates` calls that sentinel
// "jumpDepths' own out of reach", which is true for a mission board and false
// for anybody asking whether a place can be got to.
//
// ⚑ Uncapped, so `kUnreachable` here means what it says.
constexpr std::uint32_t kUnreachable = 0xffffffffu;

void gateDepthsFrom(const sol::sim::Galaxy& galaxy, std::uint32_t from, std::vector<std::uint32_t>& out)
{
    out.assign(galaxy.systems.size(), kUnreachable);
    if (from >= galaxy.systems.size()) {
        return;
    }
    out[from] = 0;
    std::vector<std::uint32_t> frontier{from};
    std::vector<std::uint32_t> next;
    for (std::uint32_t d = 1; !frontier.empty(); ++d) {
        next.clear();
        for (const std::uint32_t index : frontier) {
            for (const sol::sim::GateSpec& gate : galaxy.systems[index].gates) {
                if (out[gate.toSystem] == kUnreachable) {
                    out[gate.toSystem] = d;
                    next.push_back(gate.toSystem);
                }
            }
        }
        frontier.swap(next);
    }
}

[[nodiscard]] bool hasShadowModule(const game::SpaceWorld& world,
                                   const DefDatabase& defs,
                                   std::uint32_t system,
                                   std::uint32_t station)
{
    for (const std::uint32_t module : world.stationModules(system, station)) {
        if (module < defs.modules().size() && defs.modules()[module].family == ModuleFamily::Shadow) {
            return true;
        }
    }
    return false;
}

} // namespace

// ⚑⚑⚑⚑ THE REACHABILITY PROBE, AND IT COMES FIRST IN THE STAGE BECAUSE IF THE
// ANSWER IS BAD THE FIX IS CONTENT. A secret nobody can find is indistinguishable
// from a feature that does not work, and the remedy — a recipe chance in
// `stations.toml` — RESAMPLES the galaxy, so it has to happen before anything
// else in this phase is measured against a number.
//
// ⚑⚑⚑ IT MEASURES DISTANCE, NOT JUST COUNT, WHICH IS THE HALF THE STAGE A
// CENSUS COULD NOT SEE. `contraband_tests.cpp` already counts the docks and
// `sol.fences()` already lists them; neither answers the question a player asks,
// which is "could I have found one". Phase 36 set the precedent and the bar: it
// measured "3 of 11 docks sell a dampener over a counter, the first being Lyrioa
// Gamma in the starting system", and that is what made its exit criterion
// honest rather than hopeful.
//
// ⚑⚑ THE ASSERTION IS DERIVED FROM WHAT THIS PRINTS, and the print is the point.
// A guard that only says "more than zero" would pass on a galaxy where every
// fence sits eight jumps out in clan space, which is the failure this probe
// exists to catch.
SOL_TEST(a_player_can_reach_a_fence_from_where_the_game_starts_them)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t start = startSystemOf(world);
    SOL_REQUIRE(start < world.galaxy().systems.size());

    std::vector<std::uint32_t> depth;
    gateDepthsFrom(world.galaxy(), start, depth);
    SOL_REQUIRE(depth.size() == world.galaxy().systems.size());

    std::uint32_t docks = 0;
    std::uint32_t fences = 0;
    std::uint32_t unreachable = 0;
    std::uint32_t nearest = kUnreachable;
    std::string nearestName;
    std::uint32_t histogram[32] = {};
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            ++docks;
            if (!hasShadowModule(world, defs, s, t)) {
                continue;
            }
            ++fences;
            if (depth[s] == kUnreachable) {
                ++unreachable;
                continue;
            }
            if (depth[s] < 32) {
                ++histogram[depth[s]];
            }
            if (depth[s] < nearest) {
                nearest = depth[s];
                nearestName = system.stations[t].name + " in " + system.name;
            }
        }
    }

    std::printf("  start is '%s' (system %u); %u of %u dock(s) carry a shadow module\n",
                world.galaxy().systems[start].name.c_str(),
                start,
                fences,
                docks);
    for (std::uint32_t j = 0; j < 32; ++j) {
        if (histogram[j] != 0) {
            std::printf("    %u jump(s): %u fence(s)\n", j, histogram[j]);
        }
    }
    std::printf("  nearest fence: %s, %u jump(s) out\n", nearestName.c_str(), nearest);
    if (unreachable != 0) {
        std::printf("  %u fence(s) in no-gate systems\n", unreachable);
    }

    SOL_REQUIRE(fences > 0);
    SOL_CHECK(unreachable == 0);
    // The number is the measurement's, not a wish: see the printout above. Three
    // jumps is inside the range the game's own missions post work at, so a
    // player who never goes looking still passes one.
    SOL_CHECK(nearest <= 3);
}

// ⚑⚑⚑⚑ A DOCK THAT CAN WAREHOUSE CONTRABAND HAS A COUNTER TO SELL IT OVER,
// AND THAT RULE IS DERIVED RATHER THAN CHOSEN. `modules.toml` gives the
// `black_market` screen to ALL FOUR shadow modules and not only to
// `sol.mod_fence`, because each of them carries an `illicit` hold and a hold is
// what lets a station stock the goods at all. Giving the screen to the fence
// alone would have left the ghost docks and the clinics holding stock with no
// counter to sell it over - stock in a warehouse the player cannot reach.
//
// ⚑⚑⚑ IT IS NOT A HYPOTHETICAL: the four modules roll INDEPENDENTLY, and the
// shipped galaxy has five fences against two ghost docks and about three
// clinics, so a dock with a clinic and no fence is an ordinary composition
// rather than an edge case. This is the same shape as the rate-line-needs-a-hold
// rule in `contraband_tests.cpp`, one layer up: there, a module that MAKES a good
// needs somewhere to put it; here, a station that HOLDS one needs a way to trade
// it.
SOL_TEST(every_dock_that_can_warehouse_contraband_has_a_counter_to_sell_it_over)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const auto screenBit = 1u << static_cast<std::uint32_t>(sol::assets::StationScreen::BlackMarket);
    std::uint32_t holders = 0;
    std::uint32_t counters = 0;
    std::uint32_t lawfulWithCounter = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            bool holds = false;
            for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
                holds = holds || (world.commodityClass(c) == sol::assets::GoodsClass::Illicit &&
                                  world.stationStocks(s, t, c));
            }
            const bool offers = (world.stationScreens(s, t) & screenBit) != 0u;
            holders += holds ? 1u : 0u;
            counters += offers ? 1u : 0u;
            if (holds && !offers) {
                std::printf("  %s holds contraband and has no counter for it\n",
                            system.stations[t].name.c_str());
            }
            SOL_CHECK(!holds || offers);
            // And the other direction: no lawful dock grew the tab.
            lawfulWithCounter += offers && !hasShadowModule(world, defs, s, t) ? 1u : 0u;
        }
    }
    std::printf("  %u dock(s) hold contraband, %u offer the counter, %u of those are lawful\n",
                holders,
                counters,
                lawfulWithCounter);
    SOL_REQUIRE(holders > 0);
    SOL_CHECK(lawfulWithCounter == 0);
}

// ⚑⚑⚑⚑ THE TWO WAYS OF NAMING THE OPERATOR MUST BE THE SAME NUMBER, AND ONE
// OF THEM RUNS BEFORE THE OTHER EXISTS. `assignShadowOwners` writes the field
// from inside `composeStations`, which runs BEFORE `initializeFactions` - so
// there is no `m_factionTable` to look the black market up in, and the pass
// computes `factionCount + clanCount` by hand. Phase 34 stage E called that
// "the arithmetic is the table lookup, without the table" about clan indices;
// this stage made a second index depend on it.
//
// ⚑⚑⚑ IT IS ALSO WHERE STAGE B'S "APPENDED LAST" IS LOAD-BEARING FOR THE
// SECOND TIME. The shadow rows sit past the clans, so the index is knowable from
// the galaxy alone; slotted among the majors there would be no arithmetic to do
// it with. Reaching for the accessor here was tried and it silently assigned
// NOBODY - a whole column of `kNoFaction` with the suite still green, because
// every shadow test would have been looking at an empty galaxy.
SOL_TEST(the_arithmetic_that_names_the_operator_agrees_with_the_table)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t byTable = world.shadowFactionIndex();
    const std::uint32_t byArithmetic =
        world.galaxyParams().factionCount + static_cast<std::uint32_t>(world.galaxy().clans.size());
    std::printf("  table says %u, arithmetic says %u\n", byTable, byArithmetic);
    SOL_REQUIRE(byTable != sol::sim::kNoFaction);
    SOL_CHECK(byTable == byArithmetic);

    // And the field actually carries it - the check that would have caught the
    // silent empty column.
    std::uint32_t named = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        for (const sol::sim::StationSpec& station : world.galaxy().systems[s].stations) {
            if (station.shadowOwner != sol::sim::kNoFaction) {
                ++named;
                SOL_CHECK(station.shadowOwner == byTable);
            }
        }
    }
    std::printf("  %u station(s) name it\n", named);
    SOL_REQUIRE(named > 0);
}

// ⚑⚑⚑⚑ WHOSE COUNTER IS THIS - THE ONE GENUINELY NEW MECHANISM IN THE PHASE,
// ASSERTED AS A DISAGREEMENT BETWEEN TWO SELLERS STANDING IN THE SAME ROOM.
// `stationSells` opened with `systemOwnerFaction(m_currentSystem)` and had
// exactly one notion of who was selling: the holder of the SYSTEM. A back room
// inside somebody else's walls is the case that question cannot express, and
// this is the case made concrete - the same gate, the same dock, the same
// player, and two different answers depending on which counter is asked.
SOL_TEST(a_fence_and_the_law_stand_in_one_room_and_answer_the_same_gate_differently)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    // ⚑ `spawn` FIRST: it is what registers the component storages, and
    // `tryDockNearestStation` walks them. Without it the registry asserts on a
    // null pool, which reads as an engine fault rather than a fixture in the
    // wrong order - the same order `station_bar_tests.cpp` uses and says so.
    world.spawn(game::kDefaultUniverseSeed);
    game::GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    const sol::assets::ComponentDef* suite = content.defs().findComponent("sol.signature_dampener_mk2");
    SOL_REQUIRE(suite != nullptr);
    SOL_REQUIRE(suite->gate.factions.size() == 1);

    // Stand in a back room.
    bool docked = false;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && !docked; ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (!hasShadowModule(world, content.defs(), s, t)) {
                continue;
            }
            SOL_REQUIRE(world.enterSystem(s));
            SOL_REQUIRE(world.warpToStationOffset(t, {150.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(2000.0));
            docked = true;
            break;
        }
    }
    SOL_REQUIRE(docked);

    const std::uint32_t law = world.systemOwnerFaction(world.currentSystemIndex());
    const std::uint32_t fence = world.dockedFenceFaction();
    SOL_REQUIRE(fence != sol::sim::kNoFaction);
    SOL_CHECK(fence != law); // a back room, not a facility
    std::printf("  %s: the law is %s, the counter is %s\n",
                world.dockedStationName(),
                law < world.factions().size() ? world.factions()[law].name.c_str() : "nobody",
                world.factions()[fence].name.c_str());

    // The row is the fence's, whatever anyone's standing is.
    SOL_CHECK(world.stationFenceCarries(suite->gate));

    // ⚑⚑⚑ THE LAW CANNOT BE BRIBED INTO IT. Buy the system holder's standing
    // to the top of the scale and the suite is still refused, because the
    // allowlist names somebody else - which is the half `min_rep` alone could
    // never have expressed, and the half Phase 36 measured going wrong in the
    // opposite direction.
    if (law < world.factions().size()) {
        world.factionSim().setStanding(law, 100.0f);
    }
    SOL_CHECK(!world.stationSells(suite->gate));
    SOL_CHECK(!world.stationSellsAtFence(suite->gate));

    // ⚑⚑ AND THE FENCE CAN. Same gate, same room, same frame - only the
    // counter differs.
    world.factionSim().setStanding(fence, suite->gate.minRep);
    SOL_CHECK(world.stationSellsAtFence(suite->gate));
    SOL_CHECK(world.stationSells(suite->gate));

    // ⚑ One notch under and it is refused again, so the number on the def is
    // the number being read rather than a threshold that happens to be passed.
    world.factionSim().setStanding(fence, suite->gate.minRep - 1.0f);
    SOL_CHECK(!world.stationSells(suite->gate));
}

// ⚑⚑⚑⚑ THE FENCE'S ONE ROW IS ON THE SCREEN AND REFUSED, WHICH IS THE SHAPE
// THE USER RULED FOR (2026-09-01) WITH ITS COST STATED. Player standing with the
// black market is 0 and NOTHING in the game moves it until Phase 37 stage E, so
// the Null Signature Suite is visible and unbuyable. That is deliberate: the
// alternative was a fence with nothing to want.
//
// ⚑⚑⚑ SO THE ASSERTION IS THAT THE REFUSAL IS LEGIBLE, NOT THAT IT EXISTS.
// Every other catalogue in this game answers a gate by leaving the row OFF - an
// outfitting list shorter at a frontier station is Phase 33 stage B's whole
// design - and an omitted row here would be indistinguishable from a fence with
// an empty shelf, which is what "a secret nobody can find is indistinguishable
// from a feature that does not work" looks like at the counter rather than on
// the map. The row stays and names its price in standing.
SOL_TEST(the_locked_row_is_on_the_shelf_and_says_what_it_would_take)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    // ⚑ `spawn` FIRST: it is what registers the component storages, and
    // `tryDockNearestStation` walks them. Without it the registry asserts on a
    // null pool, which reads as an engine fault rather than a fixture in the
    // wrong order - the same order `station_bar_tests.cpp` uses and says so.
    world.spawn(game::kDefaultUniverseSeed);
    game::GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    bool docked = false;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && !docked; ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (!hasShadowModule(world, content.defs(), s, t)) {
                continue;
            }
            SOL_REQUIRE(world.enterSystem(s));
            SOL_REQUIRE(world.warpToStationOffset(t, {150.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(2000.0));
            docked = true;
            break;
        }
    }
    SOL_REQUIRE(docked);

    std::deque<std::string> text;
    sol::ui::StationPanel panel;
    std::vector<sol::ui::MountRow> mounts;
    std::vector<sol::ui::OutfitRow> components;
    std::vector<sol::ui::OutfitRow> blackMarket;
    std::vector<sol::ui::OutfitRow> blackMarketShips;
    std::vector<sol::ui::OutfitRow> weapons;
    std::vector<sol::ui::OutfitRow> crewCatalog;
    std::vector<sol::ui::OutfitRow> crewAboard;
    std::vector<sol::ui::OutfitRow> ships;
    std::vector<sol::ui::FleetRow> fleet;
    std::vector<sol::ui::CaptainRow> captains;
    std::vector<sol::ui::CaptainRow> captainHires;
    std::vector<sol::ui::CaptainRow> haulRows;
    std::vector<sol::ui::FactionRow> factions;
    game::fillStationOutfitting(world,
                                content.defs(),
                                text,
                                panel,
                                mounts,
                                components,
                                blackMarket,
                                blackMarketShips,
                                weapons,
                                crewCatalog,
                                crewAboard,
                                ships,
                                fleet,
                                captains,
                                captainHires,
                                haulRows,
                                factions);

    // The heading is the fence identity the ordinary Trade tab could not carry.
    SOL_REQUIRE(panel.fenceOperator[0] != '\0');
    std::printf("  the back room reads '%s'\n", panel.fenceOperator);

    SOL_REQUIRE(!panel.blackMarketCatalog.empty());
    std::size_t locked = 0;
    for (const sol::ui::OutfitRow& row : panel.blackMarketCatalog) {
        std::printf("  %-24s %8.0f cr  %s\n",
                    row.name,
                    static_cast<double>(row.price),
                    row.lockedReason[0] != '\0' ? row.lockedReason : "(for sale)");
        locked += row.lockedReason[0] != '\0' ? 1u : 0u;
        if (row.lockedReason[0] != '\0') {
            // It names the faction, so a player reads a destination rather than
            // a number with no address.
            SOL_CHECK(std::string(row.lockedReason).find(panel.fenceOperator) != std::string::npos);
            SOL_CHECK(row.targetMount[0] == '\0'); // and the Buy is not offered
        }
    }
    SOL_CHECK(locked == 1); // exactly the one stage E opens

    // ⚑⚑ AND IT FITS THE CELL IT IS DRAWN INTO. The reason is drawn into a
    // 3.2x button cell rather than a Buy-button one precisely because it is a
    // sentence; this is the measurement that keeps the two in step, and it is
    // the third time this project has paid for a readout that elided its own
    // point. 3.2 * 78 px at the small style is about 40 characters.
    for (const sol::ui::OutfitRow& row : panel.blackMarketCatalog) {
        if (row.lockedReason[0] != '\0') {
            std::printf("  reason is %zu characters\n", std::strlen(row.lockedReason));
            SOL_CHECK(std::strlen(row.lockedReason) <= 40);
        }
    }

    // ⚑ And it is NOT on the lawful shelf. `stationSells` is the OR over both
    // counters, so the moment stage E moves this standing the row would appear
    // in Outfitting too unless the fill sorted it - which it does.
    for (const sol::ui::OutfitRow& row : panel.components) {
        SOL_CHECK(std::string(row.id) != "sol.signature_dampener_mk2");
    }
}

// ⚑⚑⚑⚑ THE CONTRABAND CAME OFF THE ORDINARY TRADE TAB, WHICH IS THE PLAYTEST
// NOTE THIS STAGE WAS ASKED TO ANSWER. As shipped through stage B, Combat Stims
// and Stripped Components were listed on the Trade tab at the eight fence docks -
// market price, a Buy button, no legality label and nothing at all saying whose
// counter it was. The rows are the same rows; `backRoom` is what puts them
// behind the curtain, and the flag comes off the GOODS CLASS rather than off any
// jurisdiction's table, because "no lawful module can warehouse this" is a fact
// about the crate and not about where you are standing.
SOL_TEST(the_illicit_rows_are_the_back_rooms_and_the_lawful_board_never_shows_them)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::uint32_t illicit = 0;
    std::uint32_t lawful = 0;
    for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
        (world.commodityClass(c) == sol::assets::GoodsClass::Illicit ? illicit : lawful) += 1u;
    }
    std::printf("  %u illicit good(s) against %u lawful\n", illicit, lawful);
    SOL_REQUIRE(illicit > 0);
    SOL_REQUIRE(lawful > 0);

    // Every dock that stocks an illicit good is a dock with a back room, which
    // is what makes the goods class enough to sort the row by.
    std::uint32_t stocking = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            bool holds = false;
            for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
                holds = holds || (world.commodityClass(c) == sol::assets::GoodsClass::Illicit &&
                                  world.stationStocks(s, t, c));
            }
            if (!holds) {
                continue;
            }
            ++stocking;
            SOL_CHECK(world.stationHasShadowPresence(s, t));
        }
    }
    std::printf("  %u dock(s) stock one, and every one of them has a back room\n", stocking);
    SOL_REQUIRE(stocking > 0);
}

// ⚑⚑⚑⚑ THE SPLIT IS MEASURED AS GEOMETRY, AND THIS TEST EXISTS BECAUSE A
// MUTATION WALKED STRAIGHT THROUGH THE SUITE. Deleting the `backRoom` skip in
// `buildTradeTab` - putting contraband back on the ordinary trade board, which
// is the exact thing this stage was asked to fix - left 360 of 360 tests green.
// Every other guard in this file asserts a fact about the WORLD (which docks
// hold what, whose counter is whose); not one of them could see what the screen
// actually drew.
//
// ⚑⚑⚑ SO IT COUNTS VERTICES, WHICH IS UGLY AND IS THE ONLY HONEST OBSERVABLE.
// `DrawList` retains geometry rather than strings, so there is nothing to
// grep for; what there IS, is the fact that drawing eight rows and drawing ten
// rows cannot produce the same triangles. The two draws differ in ONE bit on
// two rows and in nothing else, so a difference is the filter and an equality
// is the filter missing.
//
// ⚑⚑ AND IT IS ASSERTED IN BOTH DIRECTIONS, because "the Trade tab got
// shorter" and "the Black Market tab got the rows" are different failures: a
// filter that dropped the rows off both shelves would pass the first half and
// lose the player their contraband entirely.
SOL_TEST(marking_a_row_back_room_moves_it_off_the_trade_board_and_onto_the_fences)
{
    sol::assets::Font font;
    SOL_REQUIRE(font.loadFromMemory(sol::test::buildSyntheticCookedFont()));
    sol::ui::UiContext ui;
    ui.setFont(&font, 1);
    constexpr sol::core::Vec2 kScreen = {1280.0f, 720.0f};

    // Four goods, the last two illicit - the shipped shape in miniature.
    std::vector<sol::ui::TradeRow> rows = {
        {.name = "Foodstuffs", .price = 10.0f, .stock = 100.0f},
        {.name = "Raw Ore", .price = 15.0f, .stock = 100.0f},
        {.name = "Combat Stims", .price = 180.0f, .stock = 90.0f},
        {.name = "Stripped Components", .price = 230.0f, .stock = 90.0f},
    };

    const auto drawnVertices = [&](int tab, bool split) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i].backRoom = split && i >= 2;
        }
        sol::ui::StationPanel panel;
        panel.trade.stationName = "Probe";
        panel.trade.jurisdiction = "Solar Navy";
        panel.trade.credits = 100000.0;
        panel.trade.cargoCapacity = 50.0f;
        panel.trade.rows = rows;
        panel.fenceOperator = "The Ninth Shift";
        panel.screens = ~0u;
        game::StationScreenState state;
        state.tab = tab;
        sol::ui::InputState input;
        input.mousePosition = {-100.0f, -100.0f};
        ui.beginFrame(input, kScreen);
        (void)game::buildStationScreen(ui, panel, state);
        const std::size_t count = ui.drawList().vertices().size();
        ui.endFrame();
        return count;
    };

    const std::size_t tradeSplit = drawnVertices(game::StationScreenState::Trade, true);
    const std::size_t tradeWhole = drawnVertices(game::StationScreenState::Trade, false);
    const std::size_t fenceSplit = drawnVertices(game::StationScreenState::BlackMarket, true);
    const std::size_t fenceEmpty = drawnVertices(game::StationScreenState::BlackMarket, false);
    std::printf("  Trade: %zu vertices with the split, %zu without\n", tradeSplit, tradeWhole);
    std::printf("  Black Market: %zu with the split, %zu without\n", fenceSplit, fenceEmpty);

    // Two rows left the trade board.
    SOL_CHECK(tradeSplit < tradeWhole);
    // ...and arrived on the fence's.
    SOL_CHECK(fenceSplit > fenceEmpty);
}

// ⚑⚑⚑⚑ A TRADE CANNOT LEAVE A PILOT IN DEBT, AND UNTIL THIS TEST NO GAME TEST
// CALLED `playerBuy` AT ALL. That absence is the finding: the verb that moves
// the player's money had no guard in this directory, so an arithmetic error in
// it survived from Phase 8 to Phase 37 and was found by a person clicking Buy.
//
// ⚑⚑⚑⚑ IT LIVES IN THE FENCE'S FILE BECAUSE THAT IS WHERE IT WAS FOUND, AND
// THE FIRST THING IT DID WAS REFUTE THE STORY IT WAS WRITTEN TO TELL. The
// observed case was five crates of Combat Stims taking 1000 cr to **-69 cr**,
// and the obvious explanation - "contraband is the first cargo dear enough to
// reach the boundary" - is wrong. With the hold emptied between attempts this
// sweep overdraws on **Refined Metal at 40 cr** too, and on Machinery, Alloy,
// Hull Plate and Hull Section: eight of thirty-three attempts, every one of them
// reachable at the first dock of a new game with the 100 button.
//
// ⚑⚑⚑ SO IT GUARDS EVERY GOOD THE DOCK SELLS, NOT THE ILLICIT PAIR. A fence
// happens to stock eleven commodities including the two dearest in the game,
// which makes it the widest single counter to sweep - that, and not the
// contraband, is why the test is here.
//
// ⚑⚑ THE ASSERTION IS ON THE INVARIANT, NOT ON THE NUMBER. "-69" is a fact
// about one seed's prices; "a purchase never spends money that is not there" is
// the rule, and it is checked at every amount the trade buttons offer.
SOL_TEST(no_purchase_at_any_counter_can_spend_more_than_the_pilot_has)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    game::GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    bool docked = false;
    for (std::uint32_t sys = 0; sys < world.galaxy().systems.size() && !docked; ++sys) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[sys];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (!hasShadowModule(world, content.defs(), sys, t)) {
                continue;
            }
            SOL_REQUIRE(world.enterSystem(sys));
            SOL_REQUIRE(world.warpToStationOffset(t, {150.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(2000.0));
            docked = true;
            break;
        }
    }
    SOL_REQUIRE(docked);

    // ⚑⚑⚑⚑ THE HOLD IS EMPTIED BEFORE EVERY ATTEMPT, AND FORGETTING IT MADE THIS
    // TEST VACUOUS ON ITS FIRST RUN. `playerBuy` clamps to cargo space BEFORE it
    // clamps to money, the hold is 50 units, and the commodity list starts with
    // Foodstuffs at 10 cr - so the cheap goods filled the hold, every later buy
    // moved ZERO units, and the whole sweep passed with the defect restored. A
    // purse test that never spends is not a purse test.
    const auto emptyHold = [&] {
        for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
            if (world.playerCargo(c) > 0.0f) {
                (void)world.playerSell(c, world.playerCargo(c));
            }
        }
    };

    // The amounts the trade buttons offer, against the balance a new pilot has.
    constexpr float kAmounts[] = {1.0f, 10.0f, 100.0f};
    std::uint32_t tried = 0;
    std::uint32_t spent = 0;
    double worst = 0.0;
    for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
        if (!world.dockedStationStocks(c)) {
            continue;
        }
        for (const float amount : kAmounts) {
            // A fresh 1000 cr purse and an empty hold for every attempt, which
            // is the state the reported case was in.
            emptyHold();
            world.addCredits(1000.0 - world.playerCredits());
            const double before = world.playerCredits();
            const float price = world.economy().price(world.dockedMarket(), c);
            const sol::sim::TradeResult result = world.playerBuy(c, amount);
            const double after = world.playerCredits();
            ++tried;
            if (after < 0.0) {
                std::printf("  %s x%.0f at %.2f: %.0f cr -> %.0f cr\n",
                            world.commodityIds()[c].c_str(),
                            static_cast<double>(amount),
                            static_cast<double>(price),
                            before,
                            after);
            }
            SOL_CHECK(after >= 0.0);
            // And the money that left equals the money the trade charged, so a
            // clamp that simply refused everything would not pass this.
            SOL_CHECK(std::abs((before - after) - static_cast<double>(result.credits)) < 0.01);
            if (result.units > 0.0f) {
                ++spent;
                // How close to the bone the tightest trade ran. This is the
                // anti-vacuity number: a sweep where nothing ever approaches
                // zero is a sweep that never reached the clamp.
                worst = std::max(worst, 1.0 - after / before);
            }
        }
    }
    std::printf("  %u attempt(s) at %s, %u moved units; the tightest spent %.1f%% of the purse\n",
                tried,
                world.dockedStationName(),
                spent,
                worst * 100.0);
    SOL_REQUIRE(tried > 0);
    // ⚑⚑ ANTI-VACUITY, AND IT IS EXACTLY WHAT THE FIRST VERSION OF THIS TEST
    // LACKED. At least one trade has to have spent nearly the whole purse, or
    // the boundary this exists to guard was never approached at all.
    SOL_REQUIRE(spent > 0);
    SOL_CHECK(worst > 0.95);

    // ⚑⚑ THE COUNTERFACTUAL, so this is not passing because the fence was too
    // cheap to reach the boundary. The old clamp was `credits / price()`; if
    // 1000 cr buys the whole 100-unit block of the dearest good here, the
    // boundary is never crossed and this test proves nothing.
    std::uint32_t dearest = 0;
    float best = 0.0f;
    for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
        const float price =
            world.dockedStationStocks(c) ? world.economy().price(world.dockedMarket(), c) : 0.0f;
        if (price > best) {
            best = price;
            dearest = c;
        }
    }
    world.addCredits(1000.0 - world.playerCredits());
    const float naiveUnits = static_cast<float>(1000.0 / static_cast<double>(best));
    const float honestUnits = world.economy().unitsWithin(world.dockedMarket(), dearest, 100.0f, 1000.0);
    std::printf("  dearest here is %s at %.2f: division says %.2f units, the market says %.2f\n",
                world.commodityIds()[dearest].c_str(),
                static_cast<double>(best),
                static_cast<double>(naiveUnits),
                static_cast<double>(honestUnits));
    SOL_CHECK(naiveUnits < 100.0f);      // the purse really is the binding limit
    SOL_CHECK(honestUnits < naiveUnits); // and the old sum really was too generous
}
