// Countermeasures, as components (engine plan Phase 36 stage E).
//
// ⚑⚑⚑⚑ THIS IS THE HALF `017` BOUGHT THE EXPENSIVE SHAPE FOR, AND THE PHASE
// SPEC SAID SO: "if E is cut, the phase delivers detection and consequence -
// precisely the cheap alternative 017 rejected, arrived at by running out of
// stages instead of by choosing it." That record turned the cheap shape down
// because "with no stop to survive, a signature dampener is a bigger number and
// nothing else". Stages C and D built the stop and the verdict; this stage is
// the kit that argues with them, and the test that matters is the one where a
// dampener changes the OUTCOME of a stop rather than the odds of one.
//
// ⚑⚑⚑⚑ THE OBVIOUS DESIGN WAS PRICED FIRST AND REFUTED BEFORE A LINE WAS
// WRITTEN, WHICH IS THE ONLY REASON THE STAT MOVES WHAT IT MOVES. "A dampener
// shortens the range they can read you at" is what everybody reaches for, and
// on this geometry it is the worst curve in the phase:
//
//     inspectionScanRange     1.6 km   (kTargetScanRangeFraction of 80 km)
//     InspectionParams::standoff       500 m   - where the patrol PARKS
//     closing speed                    340 m/s - interceptor vs 220 m/s shuttle
//
//     lapse needs spot distance > R + (60 - 12) * 340
//     R = 1.6 km  ->  17.9 km
//     R = 0.8 km  ->  17.1 km      a 50% dampener buys 4%. Invisible.
//     R < 500 m   ->  the patrol parks OUTSIDE its own scanner. Never reads.
//
// Flat, and then an I-win button at sig 0.31. So signature moves the NOTICE
// RATE and the SCAN CLOCK instead (the user's ruling, 2026-09-01), and the
// second of those is the one that is a tactic: `holdSeconds` does not move, so
// stretching the read is what turns a `Complied` into a `Lapsed` - and stage D
// prices a lapse at nothing while it prices running at 400 credits.
//
// ⚑ A constant in `InspectionParams` silently bounds what a stat in `FitStat`
// is allowed to be, and nothing in either place says so. That is the finding
// worth carrying out of this stage even if the numbers all change.

#include "asset_paths.hpp"
#include "content.hpp"
#include "ship_ui.hpp"
#include "space_world.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

namespace {

using Notice = game::SpaceWorld::NoticeReason;
using Outcome = game::SpaceWorld::InspectionOutcome;
using Post = game::SpaceWorld::PatrolPost;

constexpr const char* kDampenerMk1 = "sol.signature_dampener_mk1";
constexpr const char* kDampenerMk2 = "sol.signature_dampener_mk2";

// The shipped galaxy through the real content path INCLUDING the mod layer -
// without it this is an 81-system galaxy where the running game has 85, which
// is exactly the size of discrepancy nobody questions (Phase 36 stage B).
struct Galaxy
{
    game::SpaceWorld world;
    game::GameContent content;

    Galaxy()
    {
        world.spawn(game::kDefaultUniverseSeed);
        const std::string mods = std::string(SOL_DEF_DATA_DIR) + "/../mods";
        SOL_CHECK(content.initialize(SOL_DEF_DATA_DIR, game::discoverModLayers(mods), &world));
        SOL_CHECK(world.generateUniverse(content.defs()));
    }

    [[nodiscard]] std::uint32_t lawfulSystem(float minSecurity = 0.6f) const
    {
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            const sol::assets::FactionDef* law = world.jurisdictionOf(s);
            if (law == nullptr || (law->contraband.empty() && law->restricted.empty())) {
                continue;
            }
            if (world.systemSecurityBaseline(s) < minSecurity) {
                continue;
            }
            const sol::sim::SystemSpec& spec = world.galaxy().systems[s];
            if (!spec.stations.empty() && !spec.gates.empty()) {
                return s;
            }
        }
        return 0xffff'ffffu;
    }

    // ⚑⚑ THE REAL ROAD AND NOT A BACK DOOR. A dampener reaches the ship the
    // way the player's does - dock, pay, `buyFitting` picks the mount - so this
    // fixture proves the catalogue, the mount rule, the resolve and the wiring
    // in one call. There IS no cheaper way in: `applyActiveLoadout` is private
    // and `resolvedShipDef` resolves without applying.
    [[nodiscard]] bool dockAndFit(const char* componentId)
    {
        const std::uint32_t system = lawfulSystem();
        if (system == 0xffff'ffffu || !world.enterSystem(system)) {
            return false;
        }
        if (!dockHere()) {
            return false;
        }
        world.addCredits(100'000.0);
        // ⚑ The Mk2 carries `min_rep = 25` and a new pilot stands at 0 with
        // everybody, so a fixture that only paid would be refused at the
        // counter - which is the gate working, and is asserted on its own in
        // `the_strongest_kit_is_fenced_in_clan_space_and_refused_in_the_core`.
        // Here the kit is the subject and the counter is not, so the standing
        // is bought outright.
        world.factionSim().setStanding(world.systemOwnerFaction(system), 50.0f);
        std::string error;
        if (!world.buyFitting(componentId, nullptr, &error)) {
            std::printf("  buyFitting('%s') refused: %s\n", componentId, error.c_str());
            return false;
        }
        return world.undock();
    }

    [[nodiscard]] bool dockHere()
    {
        for (std::size_t i = 0; i < world.navTargets().size(); ++i) {
            if (world.navTargetKind(i) != game::SpaceWorld::NavKind::Station) {
                continue;
            }
            if (!world.warpToStationOffset(world.navTargetStation(i), {100.0, 0.0, 0.0})) {
                return false;
            }
            return world.tryDockNearestStation(1000.0);
        }
        return false;
    }

    // Stand `metres` off a patrol's HULL, in a system whose law can say
    // something. Same helper stage C's suite uses, and the distance is off the
    // hull rather than off its post because that is what every number here
    // turns on.
    [[nodiscard]] Post standOff(double metres)
    {
        const std::uint32_t system = lawfulSystem();
        SOL_CHECK(system != 0xffff'ffffu);
        SOL_CHECK(world.enterSystem(system));
        std::vector<Post> posts;
        world.patrolPosts(posts);
        SOL_CHECK(!posts.empty());
        SOL_CHECK(world.warpTo(posts[0].position, metres));
        world.patrolPosts(posts);
        return posts[0];
    }

    // Ticks until the hold ends or `seconds` pass; returns how long it took.
    double runToEnd(double seconds)
    {
        constexpr double kStep = 1.0 / 60.0;
        int frames = 0;
        const int limit = static_cast<int>(seconds * 60.0);
        while (world.heldForInspection() && frames < limit) {
            content.tick(kStep);
            world.tick(kStep);
            ++frames;
        }
        return frames / 60.0;
    }
};

// ⚑⚑⚑ HOW MANY TIMES A PATROL WOULD HAVE TAKEN AN INTEREST, WITH THE COOLDOWN
// TAKEN OUT OF THE ANSWER. `considerNotice` is a rate and a 90 s cooldown; over
// any window the cooldown dominates the count and two different rates come back
// nearly equal, which would let this measure a difference it cannot see. So the
// cooldown is cleared after every stop and what is left is the rate alone.
//
// ⚑⚑ AND THE COMPARISON IS EXACT RATHER THAN STATISTICAL, WHICH IS WHY IT CAN
// BE ASSERTED AS A NUMBER. `m_noticeRng` is a member that advances one draw per
// call, and with the cooldown cleared EVERY call draws - so two worlds built
// from the same seed see the identical stream and differ only in the threshold
// each draw is compared against. Nothing here is a flake risk.
std::uint32_t noticesOver(game::SpaceWorld& world, int minutes)
{
    const std::uint32_t before = world.lastNotice().count;
    constexpr double kStep = 1.0 / 60.0;
    for (int i = 0; i < minutes * 60 * 60; ++i) {
        if (world.considerNotice(kStep) != Notice::None) {
            world.clearNoticeCooldown();
        }
    }
    return world.lastNotice().count - before;
}

} // namespace

// ⚑⚑⚑⚑ THE ROW NOBODY HAD WRITTEN, AND THE PHASE SPEC NAMED IT AS THE THING
// THAT WOULD STOP THE EXIT BEING FLYABLE: "there is exactly one `Subsystem`
// mount in the whole game and the player's ship does not have it", so "fit a
// dampener and get through" could not be flown in the hull the game opens in.
// This is that sentence closed, through the shop rather than through a fixture.
SOL_TEST(the_hull_the_game_opens_in_can_carry_the_kit_the_phase_is_about)
{
    Galaxy g;
    SOL_REQUIRE(g.world.activeShip().defId == "sol.shuttle");
    SOL_CHECK(g.world.signature() == 1.0f); // nobody has quietened this ship

    // The mount exists, it is where `buyFitting` puts a dampener with no mount
    // named, and it is the ONLY place one goes.
    SOL_CHECK(g.world.firstFreeMountFor(kDampenerMk1) == "covert_bay");
    SOL_REQUIRE(g.dockAndFit(kDampenerMk1));

    const sol::assets::ComponentDef* mk1 = g.content.defs().findComponent(kDampenerMk1);
    SOL_REQUIRE(mk1 != nullptr);
    SOL_CHECK(g.world.activeShip().fittingAt("covert_bay") != nullptr);
    SOL_CHECK(g.world.signature() ==
              mk1->modifiers.mul[static_cast<std::size_t>(sol::assets::FitStat::Signature)]);
    SOL_CHECK(g.world.signature() < 1.0f);

    // ⚑⚑ AND THE PLAYER CAN SEE IT, WHICH ON THIS SCREEN IS NOT A DETAIL. Every
    // other row in the utility group is a quantity that is better HIGH; this is
    // the only one that is better low, so it is also the only one a player can
    // misread by knowing the others. `shipInfoReport` is the screen's own text
    // mirror - "how the screen gets verified without reading pixels" - and it
    // is asserted against the FIT rather than against a literal, because a row
    // that always printed x1.00 would look right on an unfitted ship forever.
    const std::string readout = game::shipInfoReport(g.world, g.content.defs());
    char expected[64] = {};
    std::snprintf(expected, sizeof(expected), "Signature: x%.2f", static_cast<double>(g.world.signature()));
    if (readout.find(expected) == std::string::npos) {
        std::printf("  the ship screen does not say '%s':\n%s\n", expected, readout.c_str());
    }
    SOL_CHECK(readout.find(expected) != std::string::npos);
    // The detail says what it MEANS, in the two units the stat moves. A bare
    // multiplier against a column of ranges reads as half of something good.
    SOL_CHECK(readout.find("noticed at x") != std::string::npos);
    SOL_CHECK(readout.find("read in") != std::string::npos);
}

// ⚑⚑⚑⚑ THE RULING WAS A CONVERSION AND NOT A SIXTH MOUNT, AND THIS IS THE
// ASSERTION THAT SAYS SO. `017`: "dampeners, spoofers, shielded holds and
// early-warning sensors are mounts that are not carrying guns. That trade is
// the design" - a mount handed out for free is that sentence with the trade
// taken out of it. The shuttle still carries five: what changed is that one of
// the two hold bays is now somewhere a cargo pod cannot go.
SOL_TEST(the_covert_bay_is_a_trade_and_not_a_gift)
{
    Galaxy g;
    const sol::assets::ShipDef* shuttle = g.content.defs().findShip("sol.shuttle");
    SOL_REQUIRE(shuttle != nullptr);

    std::uint32_t utility = 0, subsystem = 0;
    for (const sol::assets::ShipMount& mount : shuttle->mounts) {
        utility += mount.kind == sol::assets::MountKind::Utility ? 1u : 0u;
        subsystem += mount.kind == sol::assets::MountKind::Subsystem ? 1u : 0u;
    }
    // Five mounts, as before the stage; ONE utility where there were two.
    SOL_CHECK(shuttle->mounts.size() == 5);
    SOL_CHECK(utility == 1);
    SOL_CHECK(subsystem == 1);

    // And the trade bites in the direction the design intends: with a dampener
    // aboard there is exactly one utility mount left, so the smuggler carries
    // one pod where they used to carry two.
    SOL_REQUIRE(g.dockAndFit(kDampenerMk1));
    SOL_CHECK(g.world.firstFreeMountFor("sol.cargo_pod_mk1") == "bay_port");
    SOL_REQUIRE(g.dockAndFit("sol.cargo_pod_mk1"));
    SOL_CHECK(g.world.firstFreeMountFor("sol.cargo_pod_mk1").empty());

    // A pod is not covert kit and the mount says so - which is the whole reason
    // the kind exists rather than the family being more `utility` fittings.
    SOL_REQUIRE(!g.world.isDocked());
    SOL_CHECK(g.dockHere());
    std::string error;
    SOL_CHECK(!g.world.buyFitting("sol.cargo_pod_mk1", "covert_bay", &error));
}

// ⚑⚑⚑ THE FIRST OF THE TWO THINGS SIGNATURE MOVES: HOW OFTEN SOMEBODY LOOKS.
// The rate, not the envelope - see this file's header for why the envelope was
// refused, and note that shrinking it would also move the line at which
// `tickInspection` calls you a RUNNER, which is an offence. A countermeasure
// that made it easier to commit one would be a trap.
SOL_TEST(a_dampener_makes_a_patrol_take_an_interest_less_often)
{
    Galaxy loud;
    const Post patrol = loud.standOff(2'000.0);
    SOL_REQUIRE(patrol.factionIndex != 0xffff'ffffu);
    SOL_REQUIRE(loud.world.setTransponder(false)); // the loud case: running dark
    const std::uint32_t loudStops = noticesOver(loud.world, 30);

    Galaxy quiet;
    SOL_REQUIRE(quiet.dockAndFit(kDampenerMk1));
    (void)quiet.standOff(2'000.0);
    SOL_REQUIRE(quiet.world.setTransponder(false));
    const std::uint32_t quietStops = noticesOver(quiet.world, 30);

    Galaxy quietest;
    SOL_REQUIRE(quietest.dockAndFit(kDampenerMk2));
    (void)quietest.standOff(2'000.0);
    SOL_REQUIRE(quietest.world.setTransponder(false));
    const std::uint32_t quietestStops = noticesOver(quietest.world, 30);

    std::printf("  30 min parked dark on a patrol: %u stops at x%.2f, %u at x%.2f, %u at x%.2f\n",
                loudStops,
                static_cast<double>(loud.world.signature()),
                quietStops,
                static_cast<double>(quiet.world.signature()),
                quietestStops,
                static_cast<double>(quietest.world.signature()));

    // Anti-vacuity: the undampened case has to actually be getting stopped, or
    // "fewer" is a comparison between two zeroes.
    SOL_REQUIRE(loudStops > 20);
    SOL_CHECK(quietStops < loudStops);
    SOL_CHECK(quietestStops < quietStops);
    // And the size of the drop is the stat, not a nudge: the Mk2 is 0.35, so
    // the count has to be nearer a third than a half.
    SOL_CHECK(quietestStops * 2 < loudStops);
}

// ⚑⚑⚑⚑ THE SECOND THING IT MOVES, AND THE ONE THAT IS A TACTIC RATHER THAN A
// STAT. `holdSeconds` is a fixed 60 s budget and the read is what has to fit
// inside it, so a quieter hull is one the patrol runs out of time on. This is
// `017`'s "a stop to survive" being survivable by kit.
SOL_TEST(a_dampener_stretches_the_read_against_a_hold_clock_that_does_not_move)
{
    Galaxy loud;
    const Post loudPatrol = loud.standOff(1'000.0);
    SOL_REQUIRE(loud.world.beginInspection(loudPatrol.pilotIndex, Notice::Dark));
    const double loudSeconds = loud.runToEnd(120.0);
    SOL_REQUIRE(!loud.world.heldForInspection());
    SOL_CHECK(loud.world.lastInspection().outcome == Outcome::Complied);

    Galaxy quiet;
    SOL_REQUIRE(quiet.dockAndFit(kDampenerMk2));
    const Post quietPatrol = quiet.standOff(1'000.0);
    SOL_REQUIRE(quiet.world.beginInspection(quietPatrol.pilotIndex, Notice::Dark));
    const double quietSeconds = quiet.runToEnd(120.0);
    SOL_REQUIRE(!quiet.world.heldForInspection());

    std::printf("  read from 1 km: %.1f s at x%.2f, %.1f s at x%.2f (hold is %.0f s)\n",
                loudSeconds,
                static_cast<double>(loud.world.signature()),
                quietSeconds,
                static_cast<double>(quiet.world.signature()),
                quiet.world.inspectionParams().holdSeconds);

    // The undampened read is the shipped 12 s plus whatever the patrol spent
    // closing the last few hundred metres; the dampened one is that read
    // divided by 0.35. The ratio is the assertion, not the seconds.
    SOL_CHECK(loudSeconds < quiet.world.inspectionParams().scanSeconds + 6.0);
    SOL_CHECK(quietSeconds > loudSeconds * 2.0);
    // ⚑ AND THE GRANT IS UNTOUCHED. If a later change "balanced" this by
    // stretching the hold to match, the countermeasure would quietly stop
    // working while every other assertion here still passed.
    SOL_CHECK(quiet.world.inspectionParams().holdSeconds == loud.world.inspectionParams().holdSeconds);
}

// ⚑⚑⚑⚑ AND THIS IS THE ONE THAT MATTERS: THE OUTCOME FLIPS. Same distance,
// same patrol, same law - and the undampened ship is read while the dampened
// one is not. Stage D's ruling 2 is what turns that into a payoff: "running is
// the crime; lapsing is not", so a lapse costs nothing at all, where running
// the same stop costs 400 credits and 8 standing.
SOL_TEST(a_dampened_ship_lapses_a_stop_that_reads_an_undampened_one)
{
    // Far enough out that the patrol spends most of its grant closing. Below
    // this the read still lands whatever the fit; above it, neither ship is
    // ever read and the test would be measuring the distance, not the kit.
    constexpr double kSpotDistance = 14'000.0;

    Galaxy loud;
    const Post loudPatrol = loud.standOff(kSpotDistance);
    SOL_REQUIRE(loud.world.beginInspection(loudPatrol.pilotIndex, Notice::Dark));
    loud.runToEnd(120.0);
    SOL_REQUIRE(!loud.world.heldForInspection());

    Galaxy quiet;
    SOL_REQUIRE(quiet.dockAndFit(kDampenerMk2));
    const Post quietPatrol = quiet.standOff(kSpotDistance);
    SOL_REQUIRE(quiet.world.beginInspection(quietPatrol.pilotIndex, Notice::Dark));
    quiet.runToEnd(120.0);
    SOL_REQUIRE(!quiet.world.heldForInspection());

    std::printf("  spotted at %.0f km: %s at x%.2f, %s at x%.2f\n",
                kSpotDistance / 1000.0,
                game::SpaceWorld::inspectionOutcomeName(loud.world.lastInspection().outcome),
                static_cast<double>(loud.world.signature()),
                game::SpaceWorld::inspectionOutcomeName(quiet.world.lastInspection().outcome),
                static_cast<double>(quiet.world.signature()));

    SOL_CHECK(loud.world.lastInspection().outcome == Outcome::Complied);
    SOL_CHECK(quiet.world.lastInspection().outcome == Outcome::Lapsed);
    // ⚑ Lapsed and not `Ran`, which is the whole difference: the ship never
    // left the envelope, so nobody committed an offence. Stage D queues no
    // verdict at all for a lapse.
    SOL_CHECK(quiet.world.lastInspection().verdict == game::SpaceWorld::InspectionVerdict::None);
}

// ⚑⚑⚑ NO FIT MAY SWITCH THE PHASE OFF, AND THE DEF LAYER CANNOT ENFORCE THAT.
// `resolveLoadout` multiplies whatever a component asks for - two dampeners
// stack, and `assets.unit` pins that they do - so a mod with `signature_mul =
// 0.0`, or a negative `signature_add`, would arrive at the reader as "never
// noticed, and a scan clock divided by nothing". The floor is where the value
// is READ, so there is no way round it from a data file.
SOL_TEST(no_fit_can_quieten_a_ship_past_the_floor)
{
    constexpr const char* kSilentDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

[[commodity]]
id = "sol.ore"
name = "Raw Ore"
base_price = 12.0
ore_weight_core = 1.0
ore_weight_frontier = 1.0
ore_weight_fringe = 1.0

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
mass = 10000.0
power_output = 8.0
signature = 1.0

  [[ship.mount]]
  id = "covert_bay"
  kind = "subsystem"
  size = "small"

[[component]]
id = "sol.silence"
name = "Silence"
mount = "subsystem"
size = "small"
price = 10.0
mass = 0.0
power_draw = 1.0
signature_mul = 0.0
)";

    sol::assets::DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kSilentDefs, std::strlen(kSilentDefs), "silent.toml", &error));

    game::SpaceWorld world;
    world.spawn(1701);
    // ⚑ `applyDefs` BEFORE `generateUniverse`, and it is not optional: it is
    // what hands the world its def database, and without it `buyFitting`
    // refuses with "must be docked to refit" - the same sentence a genuinely
    // undocked ship gets, which is the most misleading refusal in the file.
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));
    SOL_CHECK(world.signature() == 1.0f);

    // Dock, buy the thing that would silence the ship, and read what the world
    // actually flies with.
    bool docked = false;
    for (std::size_t i = 0; i < world.navTargets().size() && !docked; ++i) {
        if (world.navTargetKind(i) != game::SpaceWorld::NavKind::Station) {
            continue;
        }
        SOL_REQUIRE(world.warpToStationOffset(world.navTargetStation(i), {100.0, 0.0, 0.0}));
        docked = world.tryDockNearestStation(1000.0);
    }
    SOL_REQUIRE(docked);
    world.addCredits(1000.0);
    if (!world.buyFitting("sol.silence", nullptr, &error)) {
        std::printf("  buyFitting refused: %s\n", error.c_str());
    }
    SOL_REQUIRE(error.empty());

    // The def layer did what it was asked - the resolved hull really is zero -
    // and the world refused to fly it.
    SOL_CHECK(world.resolvedShipDef(world.activeShip()).signature == 0.0f);
    SOL_CHECK(world.signature() == game::SpaceWorld::kMinSignature);

    // ⚑⚑⚑ AND THE FLOOR HOLDS ON BOTH ROADS IN, WHICH IS A SEPARATE LINE OF
    // CODE IN A SEPARATE FUNCTION. A refit runs `applyShipDef`; a LOAD
    // recomputes the stat from the resolved def in `loadFrom` and never calls
    // that one. Clamping in one and not the other is a save file that silences
    // a ship the shop refused to silence - and mutation testing is what found
    // that the round-trip above, which fits a dampener at 0.6, cannot see it.
    SOL_REQUIRE(world.undock());
    const std::string path = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/silent_round_trip.sav";
    SOL_REQUIRE(sol::platform::createDirectories(SOL_GAME_TEST_SCRATCH_DIR));
    (void)sol::platform::deleteFile(path.c_str());
    SOL_REQUIRE(world.saveTo(path.c_str(), "silent"));

    game::SpaceWorld loaded;
    loaded.spawn(1701);
    loaded.applyDefs(defs);
    SOL_REQUIRE(loaded.generateUniverse(defs));
    SOL_REQUIRE(loaded.loadFrom(path.c_str()));
    SOL_CHECK(loaded.resolvedShipDef(loaded.activeShip()).signature == 0.0f);
    SOL_CHECK(loaded.signature() == game::SpaceWorld::kMinSignature);
}

// ⚑⚑⚑⚑ THE EXIT CRITERION NEEDS A COUNTER TO BUY IT OVER, AND `buyFitting`
// IS NOT EVIDENCE THAT ONE EXISTS. The live drive is what raised this: the
// console lever bought a dampener at Lyrioa Alpha, and the station screen there
// has FIVE tabs and no Outfitting among them. `stationSells` asks about the
// faction, the standing and the stock; whether the place has an outfitter at
// all is `dockedStationScreens`, a bitmask off the station's MODULES that only
// the tab strip reads - so a dev lever can buy where a player cannot, and every
// other test in this file goes in through the lever.
//
// ⚑⚑ "Fit a dampener on the shuttle and get through" is not flyable if the
// answer is nowhere. So this walks the shipped galaxy for a station that has
// both halves at once, and it is the only assertion here that would fail on a
// content change rather than a code one.
SOL_TEST(a_player_can_actually_buy_a_dampener_over_a_counter)
{
    Galaxy g;
    const sol::assets::ComponentDef* mk1 = g.content.defs().findComponent(kDampenerMk1);
    SOL_REQUIRE(mk1 != nullptr);
    constexpr std::uint32_t kOutfitting =
        1u << static_cast<std::uint32_t>(sol::assets::StationScreen::Outfitting);

    std::uint32_t counters = 0;
    std::uint32_t docksSeen = 0;
    std::string firstCounter;
    for (std::uint32_t s = 0; s < g.world.galaxy().systems.size() && counters < 3; ++s) {
        const sol::sim::SystemSpec& spec = g.world.galaxy().systems[s];
        for (std::uint32_t st = 0; st < spec.stations.size() && counters < 3; ++st) {
            Galaxy probe;
            if (!probe.world.enterSystem(s)) {
                continue;
            }
            if (!probe.world.warpToStationOffset(st, {100.0, 0.0, 0.0}) ||
                !probe.world.tryDockNearestStation(1000.0)) {
                continue;
            }
            ++docksSeen;
            if ((probe.world.dockedStationScreens() & kOutfitting) == 0u) {
                continue; // no outfitter here: the tab is not drawn
            }
            if (!probe.world.stationSells(mk1->gate)) {
                continue;
            }
            if (firstCounter.empty()) {
                firstCounter = probe.world.dockedStationName();
            }
            ++counters;
        }
    }
    std::printf("  %u dock(s) visited, %u sell a dampener over a counter (first: %s)\n",
                docksSeen,
                counters,
                firstCounter.c_str());
    SOL_REQUIRE(docksSeen > 0);
    SOL_CHECK(counters > 0);
}

// ⚑⚑⚑ IT COSTS NO SAVE BUMP, AND THAT IS A CLAIM ABOUT A SECOND CODE PATH
// RATHER THAN ABOUT THE FORMAT. `kSaveVersion` does not move because signature
// is DERIVED - the save already stores which fitting is in which mount, and the
// load recomputes the stat from the resolved def. But "recomputes" is a
// separate line from the one a refit runs, in a different function, and a load
// that forgot it would hand the player a ship whose outfitting screen said
// x0.60 while every patrol in the galaxy read it as x1.00. Nothing else in this
// suite touches that line.
SOL_TEST(a_dampener_survives_a_save_without_the_format_moving)
{
    const std::string path = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/covert_round_trip.sav";
    SOL_REQUIRE(sol::platform::createDirectories(SOL_GAME_TEST_SCRATCH_DIR));
    (void)sol::platform::deleteFile(path.c_str());

    Galaxy writer;
    SOL_REQUIRE(writer.dockAndFit(kDampenerMk1));
    const float fitted = writer.world.signature();
    SOL_REQUIRE(fitted < 1.0f);
    SOL_REQUIRE(writer.world.saveTo(path.c_str(), "covert"));

    Galaxy reader;
    SOL_REQUIRE(reader.world.signature() == 1.0f); // a fresh ship, before the load
    SOL_REQUIRE(reader.world.loadFrom(path.c_str()));
    SOL_CHECK(reader.world.activeShip().fittingAt("covert_bay") != nullptr);
    SOL_CHECK(reader.world.signature() == fitted);
}

// ⚑⚑⚑ THE FENCE ALREADY EXISTS AND NOTHING IN THIS STAGE BUILT IT. The Mk2
// carries `min_rep = 25`, which a major will not waive - but `stationSells`
// short-circuits on `faction.pirate` before it ever reads the number ("pirate
// stations fence anything their defs allow, standing be damned"). So the
// strongest countermeasure in the game is sold to a stranger in clan space and
// refused to one in the core, and that is a consequence of a line Phase 8b
// wrote rather than a key this stage authored. ⚑ It is also why the def uses
// `min_rep` and not a `factions` allowlist: an allowlist would have made this
// phase author Phase 37's content.
SOL_TEST(the_strongest_kit_is_fenced_in_clan_space_and_refused_in_the_core)
{
    Galaxy g;
    const sol::assets::ComponentDef* mk2 = g.content.defs().findComponent(kDampenerMk2);
    SOL_REQUIRE(mk2 != nullptr);
    SOL_REQUIRE(mk2->gate.minRep > 0.0f);
    SOL_REQUIRE(mk2->gate.factions.empty());

    int lawful = 0, clan = 0;
    for (std::uint32_t s = 0; s < g.world.galaxy().systems.size() && (lawful == 0 || clan == 0); ++s) {
        const std::uint32_t owner = g.world.systemOwnerFaction(s);
        if (owner >= g.world.factions().size() || g.world.galaxy().systems[s].stations.empty()) {
            continue;
        }
        const bool pirate = g.world.factions()[owner].pirate;
        if ((pirate && clan > 0) || (!pirate && lawful > 0)) {
            continue;
        }
        Galaxy probe;
        if (!probe.world.enterSystem(s) || !probe.dockHere()) {
            continue;
        }
        // Standing starts at 0 with everybody, which is under the Mk2's gate.
        const bool sells = probe.world.stationSells(mk2->gate);
        if (pirate) {
            ++clan;
            SOL_CHECK(sells);
        } else {
            ++lawful;
            SOL_CHECK(!sells);
        }
    }
    // Anti-vacuity, and it is a real question about the shipped galaxy rather
    // than a formality: 29 of 85 systems are clan-held, and a fence needs one
    // of them to have somewhere to dock.
    std::printf("  probed %d lawful station(s) and %d clan station(s)\n", lawful, clan);
    SOL_REQUIRE(lawful == 1);
    SOL_REQUIRE(clan == 1);

    // The Mk1 is the one a new pilot can actually buy, which is what makes the
    // phase's exit flyable from the ship the game opens in.
    const sol::assets::ComponentDef* mk1 = g.content.defs().findComponent(kDampenerMk1);
    SOL_REQUIRE(mk1 != nullptr);
    SOL_CHECK(mk1->gate.minRep <= 0.0f);
}
