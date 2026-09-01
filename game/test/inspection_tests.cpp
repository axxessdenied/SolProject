// The stop (engine plan Phase 36 stage C): an inbound hail, a hold, and a scan
// that is not the player's.
//
// ⚑⚑⚑⚑ THE HOLD IS MODELLED ON `DockClearance` RATHER THAN ON THE HAIL, WHICH
// THE SPEC ASKED FOR AND THE SAVE FORMAT SECONDED. A hail is a one-shot
// request/reply with no duration; a timed, revocable grant is structurally what
// "hold station while I scan you" IS. And `m_clearance` has never been
// serialised, so a hold beside it costs no save bump - where the spec's
// "per-patrol scan state" written as a FIELD on `ShipPilot` would have cost
// one, because `ShipPilot` is snapshot component 19 and an id in that schema is
// a promise about a layout.
//
// ⚑⚑⚑⚑ AND THE MEASUREMENT THAT MOVED THE DESIGN, WHICH IS WHAT
// `you_cannot_out_fly_a_patrols_nose_once_it_is_close` exists to pin down. The
// user's ruling was that the hold cuts the cruise drive, on the reasoning that
// running would then have to be FLOWN. Flying it was then measured: six
// manoeuvres against a live hold - sitting still, full burn, boost, hard
// strafe, a barrel and a hard pitch - and ALL SIX complied, because a 340 m/s
// interceptor tracks a 220 m/s shuttle inside a 20 degree cone indefinitely.
// With the cone as the only way out the stop was unescapable and `017`'s "every
// beat interruptible by flying away" was simply false.
//
// What restored it was giving the scan a RANGE, derived rather than chosen: the
// player's own scanner is already a long pulse plus a target scan at
// `kTargetScanRangeFraction` of it, and its own comment says why - "you find a
// contact from across the playfield, then fly to it to learn what it is". A
// patrol notices you at 80 km and must close to 1.6 km to read you. What that
// buys is a WARNING PROPORTIONAL TO DISTANCE, and it is the whole mechanic:
//
//     stopped at  1.0 km  ->  drive locked immediately, complied in 12.8 s
//     stopped at  2.1 km  ->  3.2 s of free drive, complied in 15.2 s
//     stopped at  5.1 km  ->  12.1 s of free drive, complied in 24.0 s
//     stopped at 20.0 km  ->  57.2 s free, and the hold LAPSED
//     stopped at 60.0 km  ->  never locked at all
//
// So the gate picket that meets you 738 m off your arrival gate has you, and
// the patrol that spots you across a station approach has to spend two minutes
// closing while your drive is still your own.

#include "asset_paths.hpp"
#include "content.hpp"
#include "space_world.hpp"

#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/sim/damage.hpp>
#include <sol/test/test.hpp>

namespace {

using Notice = game::SpaceWorld::NoticeReason;
using Outcome = game::SpaceWorld::InspectionOutcome;
using Post = game::SpaceWorld::PatrolPost;

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

    // Stand in a lawful system with a patrol to be stopped by, `metres` off the
    // patrol's HULL rather than off its post - the two differ by kilometres and
    // this stage's whole design turns on the distance.
    [[nodiscard]] Post standOff(double metres)
    {
        const std::uint32_t system = lawfulSystem();
        SOL_CHECK(system != 0xffff'ffffu);
        SOL_CHECK(world.enterSystem(system));
        std::vector<Post> posts;
        world.patrolPosts(posts);
        SOL_CHECK(!posts.empty());
        SOL_CHECK(world.warpTo(posts[0].position, metres));
        world.patrolPosts(posts); // refreshed: distanceToPlayer has just moved
        return posts[0];
    }

    void run(double seconds)
    {
        constexpr double kStep = 1.0 / 60.0;
        for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i) {
            content.tick(kStep);
            world.tick(kStep);
        }
    }

    // What a named patrol is DOING right now. Read off `patrolPosts` rather
    // than off `pilotStateOf`, which wants an entity where everything on this
    // side of the wall has an entity INDEX.
    [[nodiscard]] game::PilotState stateOf(std::uint32_t pilotIndex)
    {
        std::vector<Post> posts;
        world.patrolPosts(posts);
        for (const Post& post : posts) {
            if (post.pilotIndex == pilotIndex) {
                return post.state;
            }
        }
        return game::PilotState::Idle;
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

} // namespace

// The stop opens, names who is running it, and puts that patrol into a state
// nothing else in the game uses - which is what stops Lua's patrol branch
// putting it back on its beat mid-inspection.
SOL_TEST(a_stop_opens_a_hold_and_the_patrol_stops_flying_its_beat)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    SOL_REQUIRE(!g.world.heldForInspection());

    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    SOL_CHECK(g.world.heldForInspection());
    SOL_CHECK(g.world.inspection().patrolIndex == patrol.pilotIndex);
    SOL_CHECK(g.world.inspection().reason == Notice::Dark);
    SOL_CHECK(g.world.inspection().factionIndex == patrol.factionIndex);
    SOL_CHECK(g.world.inspection().secondsLeft == g.world.inspectionParams().holdSeconds);
    SOL_CHECK(g.world.inspection().scanProgress == 0.0f);
    SOL_CHECK(g.world.lastInspection().opened == 1);

    // In `Inspect`, which no other path in the game produces, so the patrol is
    // off its beat for as long as the hold stands.
    SOL_CHECK(g.stateOf(patrol.pilotIndex) == game::PilotState::Inspect);
}

// ⚑⚑ The demand says WHY, and every line this stage can speak is inside the
// comms panel's measured budget. Stage A shipped a 58-character line into a
// ~50-character panel and the player read "Squawk your transponder or s";
// nothing in this project measures a string against a panel, so the test does.
SOL_TEST(every_line_a_stop_speaks_fits_the_panel_and_the_reason_changes_it)
{
    const Notice reasons[] = {Notice::Dark, Notice::Wanted, Notice::RandomCheck};
    std::vector<std::string> demands;
    for (const Notice reason : reasons) {
        Galaxy g;
        const Post patrol = g.standOff(1'000.0);
        SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, reason));
        SOL_REQUIRE(!g.world.comms().empty());
        const std::string demand = g.world.comms().back().text;
        SOL_CHECK(demand.size() <= 50);
        demands.push_back(demand);

        // And every line on the way out of a hold, whichever way it ends.
        g.runToEnd(120.0);
        SOL_REQUIRE(!g.world.heldForInspection());
        for (const game::SpaceWorld::CommsMessage& line : g.world.comms()) {
            SOL_CHECK(line.text.size() <= 50);
        }
    }
    SOL_REQUIRE(demands.size() == 3);
    // Three reasons, three sentences. A stop that always opens the same way
    // teaches the player that the reason does not matter.
    SOL_CHECK(demands[0] != demands[1]);
    SOL_CHECK(demands[1] != demands[2]);
    SOL_CHECK(demands[0] != demands[2]);
}

// ⚑⚑ A faction that has decided to shoot you does not stop to check your papers
// first - and this is a correctness rule, not flavour. Lua's patrol branch calls
// `pilot_engage_enemy` on every think while not attacking, so without this the
// stop cancels itself one frame after it opens, which reads as a bug.
SOL_TEST(a_faction_already_shooting_at_you_does_not_check_your_papers)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    SOL_REQUIRE(patrol.factionIndex != 0xffff'ffffu);

    // The control first: neutral, and the stop opens.
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    g.world.endInspection(Outcome::Lost, nullptr);

    g.world.factionSim().setStanding(patrol.factionIndex, -100.0f);
    SOL_REQUIRE(g.world.factionSim().playerHostile(patrol.factionIndex));
    SOL_CHECK(!g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    SOL_CHECK(!g.world.heldForInspection());
}

// One stop at a time, which is what "per-patrol scan state" means with notice
// throttled to one stop per system per cooldown. A vector of holds would be a
// second copy of a guarantee `considerNotice` already makes.
SOL_TEST(only_one_stop_runs_at_a_time)
{
    Galaxy g;
    const std::uint32_t system = g.lawfulSystem();
    SOL_REQUIRE(system != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(system));
    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    SOL_REQUIRE(posts.size() >= 2); // anti-vacuity: there IS a second patrol
    SOL_REQUIRE(g.world.warpTo(posts[0].position, 1'000.0));

    SOL_REQUIRE(g.world.beginInspection(posts[0].pilotIndex, Notice::Dark));
    SOL_CHECK(!g.world.beginInspection(posts[1].pilotIndex, Notice::Wanted));
    SOL_CHECK(g.world.inspection().patrolIndex == posts[0].pilotIndex);
}

// ⚑⚑⚑ THE WHOLE LOOP, FLOWN. The patrol turns, closes, holds its nose on the
// player for twelve seconds, and hands itself back to `pilot_think`. Nothing
// short of ticking the world proves this: the cone rule is measured off the
// patrol's ORIENTATION, so the scan only advances while the steering succeeds.
SOL_TEST(holding_station_completes_the_scan_and_the_patrol_goes_back_to_its_beat)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    const sol::sim::DefenseState before = g.world.playerDefense().state;
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));

    const double took = g.runToEnd(90.0);
    SOL_REQUIRE(!g.world.heldForInspection());
    SOL_CHECK(g.world.lastInspection().outcome == Outcome::Complied);
    SOL_CHECK(g.world.lastInspection().reason == Notice::Dark);
    SOL_CHECK(g.world.lastInspection().complied == 1);
    SOL_CHECK(g.world.lastInspection().ran == 0);
    SOL_CHECK(g.world.lastInspection().progressAtEnd >= 1.0f);
    // The scan is 12 s and the turn costs about one more. A stop that took
    // thirty would mean the patrol was losing and re-finding its aim.
    SOL_CHECK(took > g.world.inspectionParams().scanSeconds);
    SOL_CHECK(took < g.world.inspectionParams().scanSeconds + 8.0);
    // ⚑⚑ AND NOBODY FIRED - READ OFF THE SHIELDS AND NOT ONLY THE HULL. An
    // inspection is the attack case's steering with the trigger left alone, and
    // this is what says so from outside. The hull alone did NOT say it: the
    // shuttle carries 100 points of shield, so a patrol that opened fire for
    // the whole twelve seconds never reached the hull and the mutation walked
    // straight past a hull-only assertion. The first thing a shot touches is
    // the first thing to check.
    SOL_CHECK(g.world.playerDefense().state.shieldFore == before.shieldFore);
    SOL_CHECK(g.world.playerDefense().state.shieldAft == before.shieldAft);
    SOL_CHECK(g.world.playerDefense().state.hull == before.hull);
    // Handed back: Idle is what `pilot_think` reads as "give me my next leg".
    SOL_CHECK(g.stateOf(patrol.pilotIndex) != game::PilotState::Inspect);
}

// ⚑⚑⚑⚑ THE USER'S RULING, AND ITS REACH. Cruise is 25,000x the assist cap, so a
// hold anybody can light the drive out of is a message rather than a hold. It
// reaches exactly as far as the scan does and no further - one rule, on the
// range and not on the cone, because a cone that flickers for a frame would
// hand out escapes nobody could aim for.
SOL_TEST(the_drive_lock_reaches_exactly_as_far_as_the_scan_does)
{
    {
        Galaxy g;
        const Post patrol = g.standOff(1'000.0);
        SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
        SOL_CHECK(g.world.inspection().distance < g.world.inspectionScanRange());
        SOL_CHECK(g.world.driveLockedByInspection());

        // And the drive really is refused, not merely reported as locked.
        sol::sim::FlightInput run;
        run.cruise = true;
        g.world.setShipInput(run);
        g.world.tick(1.0 / 60.0);
        SOL_CHECK(!g.world.shipInput().cruise);
    }
    {
        Galaxy g;
        // Well outside the scan's reach but well inside the notice envelope,
        // which is the case the whole warning window is built out of.
        const Post patrol = g.standOff(30'000.0);
        SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
        SOL_CHECK(g.world.inspection().distance > g.world.inspectionScanRange());
        SOL_CHECK(g.world.heldForInspection());
        SOL_CHECK(!g.world.driveLockedByInspection());

        sol::sim::FlightInput run;
        run.cruise = true;
        g.world.setShipInput(run);
        g.world.tick(1.0 / 60.0);
        SOL_CHECK(g.world.shipInput().cruise); // still yours
    }
}

// ⚑⚑⚑ THE MECHANIC THE MEASUREMENT BOUGHT: how much warning you get is how far
// off they spotted you. This is the assertion that would fail if somebody made
// the lock unconditional again, and it is why the stage is shaped this way.
SOL_TEST(a_stop_at_a_gate_gives_no_warning_and_one_seen_from_far_off_gives_a_minute)
{
    const auto freeSecondsAfterStopAt = [](double metres) {
        Galaxy g;
        const Post patrol = g.standOff(metres);
        SOL_CHECK(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
        constexpr double kStep = 1.0 / 60.0;
        double free = 0.0;
        int frames = 0;
        while (g.world.heldForInspection() && frames < 60 * 120) {
            free += g.world.driveLockedByInspection() ? 0.0 : kStep;
            g.content.tick(kStep);
            g.world.tick(kStep);
            ++frames;
        }
        return free;
    };

    // Met at a gate: the picket is already inside its own scanning range, so
    // there is no window at all. Measured arrival distance is 738 m.
    SOL_CHECK(freeSecondsAfterStopAt(1'000.0) < 1.0);
    // Spotted across a station approach: minutes of closing, all of it yours.
    SOL_CHECK(freeSecondsAfterStopAt(30'000.0) > 30.0);
}

// ⚑⚑⚑⚑ WHY THE RANGE HAD TO BE THE WAY OUT, ASSERTED RATHER THAN ASSUMED. Once
// a patrol is inside its scanning range, how you fly does not change the answer
// - it tracks a slower, less agile hull inside a 20 degree cone indefinitely. If
// this ever starts failing, the steering has changed enough that the cone could
// carry the escape on its own and this stage's shape should be re-read.
SOL_TEST(you_cannot_out_fly_a_patrols_nose_once_it_is_close)
{
    const sol::sim::FlightInput manoeuvres[] = {
        {},                                                                            // sit still
        {.linear = {0.0f, 0.0f, -1.0f}},                                               // full burn
        {.linear = {0.0f, 0.0f, -1.0f}, .boost = true},                                // and boost
        {.linear = {1.0f, 0.0f, 0.0f}, .boost = true},                                 // hard strafe
        {.linear = {1.0f, 0.0f, -1.0f}, .angular = {0.0f, 1.0f, 0.0f}, .boost = true}, // a barrel
        {.linear = {0.0f, 0.0f, -1.0f}, .angular = {1.0f, 0.0f, 0.0f}, .boost = true}, // hard pitch
    };
    for (const sol::sim::FlightInput& manoeuvre : manoeuvres) {
        Galaxy g;
        const Post patrol = g.standOff(1'000.0);
        SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
        g.world.setShipInput(manoeuvre);
        g.runToEnd(90.0);
        SOL_REQUIRE(!g.world.heldForInspection());
        // ⚑ NOT `== Complied`, and the difference is a finding. Five of the six
        // comply; the sixth ends `Lost`, because the patrol's own 2 Hz think
        // called `pilot_engage_enemy` and found a war enemy worth more than a
        // paperwork check. What the manoeuvres never do is `Ran` - flying is
        // not what gets you out of this, which is the whole claim.
        SOL_CHECK(g.world.lastInspection().outcome != Outcome::Ran);
    }
}

// The one escape that is pure distance: get outside the 80 km envelope and the
// patrol has lost you, whatever it was in the middle of.
SOL_TEST(leaving_the_envelope_ends_the_stop_as_running)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    g.run(4.0); // let the scan get going, so this is an interruption
    SOL_REQUIRE(g.world.heldForInspection());
    SOL_REQUIRE(g.world.inspection().scanProgress > 0.0f);

    SOL_REQUIRE(g.world.warpTo(patrol.position, 5.0e6));
    g.run(0.5);
    SOL_CHECK(!g.world.heldForInspection());
    SOL_CHECK(g.world.lastInspection().outcome == Outcome::Ran);
    SOL_CHECK(g.world.lastInspection().ran == 1);
    SOL_CHECK(g.world.lastInspection().complied == 0);
    // It ended part-way through, which is what makes it an interruption rather
    // than a stop that quietly never started.
    SOL_CHECK(g.world.lastInspection().progressAtEnd > 0.0f);
    SOL_CHECK(g.world.lastInspection().progressAtEnd < 1.0f);
}

// ⚑ A pilot who keeps their distance is not running, they are declining, and the
// patrol eventually gives up. This is the outcome the 60 s grant exists for: at
// 340 m/s an interceptor can only service a stop it opened inside ~20 km.
SOL_TEST(a_patrol_that_never_gets_close_enough_gives_up)
{
    Galaxy g;
    const Post patrol = g.standOff(60'000.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));

    const double took = g.runToEnd(120.0);
    SOL_REQUIRE(!g.world.heldForInspection());
    SOL_CHECK(g.world.lastInspection().outcome == Outcome::Lapsed);
    SOL_CHECK(g.world.lastInspection().progressAtEnd == 0.0f);
    SOL_CHECK(took >= g.world.inspectionParams().holdSeconds - 1.0);
}

// Jumping out from under a hold is running, and it is recorded BEFORE the patrol
// that opened it stops existing - state is dropped where the thing that
// invalidates it happens (Phase 35 stage D), not where somebody later notices.
SOL_TEST(a_jump_out_from_under_a_hold_is_running)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    const std::uint32_t was = g.world.currentSystemIndex();
    const std::uint32_t neighbour = g.world.galaxy().systems[was].gates.at(0).toSystem;

    SOL_REQUIRE(g.world.jumpToSystem(g.world.galaxy().systems[neighbour].name.c_str()));
    SOL_CHECK(g.world.currentSystemIndex() != was);
    SOL_CHECK(!g.world.heldForInspection());
    SOL_CHECK(g.world.lastInspection().outcome == Outcome::Ran);
}

// ⚑⚑ THE PATROL'S NOSE HAS TO BE ON YOU, WHICH IS `tickScanning`'s OWN RULE
// POINTED THE OTHER WAY - "scanning is a held aim, not a checkbox". On the frame
// a stop opens the patrol is still pointed down its patrol leg, so the scan
// cannot have started yet however close the two hulls are.
SOL_TEST(the_scan_waits_for_the_patrol_to_get_its_nose_on_you)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    SOL_REQUIRE(g.world.driveLockedByInspection()); // in range from frame one

    // A quarter of a second: comfortably inside the scan's 12 s, and far too
    // little for an interceptor to swing round at 1.9 rad/s.
    g.run(0.25);
    SOL_REQUIRE(g.world.heldForInspection());
    SOL_CHECK(g.world.inspection().scanProgress == 0.0f);

    // But it does get there, and then it runs. Anti-vacuity for the above.
    g.run(4.0);
    SOL_CHECK(g.world.inspection().scanProgress > 0.0f);
}

// ⚑⚑⚑ THE SCAN RESETS RATHER THAN PAUSES, WHICH IS `stopScan`'s BEHAVIOUR
// VERBATIM - the player's own target scan drops to zero the moment aim breaks,
// and the two scans in this game read identically from the cockpit whichever
// end of one you are on. Backing out of their scanning range costs the whole
// scan; it does not bank what you have already sat through.
//
// ⚑ And this is the one rule with an escape hatch in it, because the pilot is
// still inside the 80 km envelope here: the hold stands, the drive comes back,
// and the patrol has to close all over again.
SOL_TEST(backing_out_of_scanning_range_costs_the_whole_scan)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    g.run(6.0);
    SOL_REQUIRE(g.world.heldForInspection());
    SOL_REQUIRE(g.world.inspection().scanProgress > 0.2f); // well under way
    SOL_REQUIRE(g.world.driveLockedByInspection());

    // Out of their reach, still well inside the envelope that noticed us.
    SOL_REQUIRE(g.world.warpTo(patrol.position, 40'000.0));
    g.world.tick(1.0 / 60.0);
    SOL_CHECK(g.world.heldForInspection()); // the hold stands: this is not Ran
    SOL_CHECK(g.world.inspection().scanProgress == 0.0f);
    SOL_CHECK(!g.world.driveLockedByInspection()); // and the drive is yours again
}

// ⚑⚑ THE QUIET WINDOW IS MEASURED FROM THE END OF A STOP AND NOT FROM ITS
// START. A hold can stand for a minute, so charging the 90 s cooldown against
// the moment of notice would let the next patrol open one the instant this one
// let go - a second checkpoint immediately after the first, which is exactly the
// tax `017` warns about, arriving through a clock instead of through a rate.
SOL_TEST(the_quiet_window_starts_when_the_stop_ends)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    SOL_REQUIRE(g.world.setTransponder(false)); // the loudest a pilot can be
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    g.runToEnd(90.0);
    SOL_REQUIRE(!g.world.heldForInspection());

    // A full minute of rolls after the stop ended, at the dark rate, parked on
    // top of a patrol - and nothing, because the cooldown began at the end.
    for (int i = 0; i < 60 * 60; ++i) {
        SOL_CHECK(g.world.considerNotice(1.0 / 60.0) == Notice::None);
    }
    SOL_CHECK(g.world.lastInspection().opened == 1);
}

// A hold names an entity index in the run that is being replaced, so it goes
// where the clearance beside it goes. Loading into a game already held would be
// the load restoring a moment rather than a state.
SOL_TEST(a_loaded_game_does_not_wake_up_held)
{
    Galaxy g;
    const Post patrol = g.standOff(1'000.0);
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/saves";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/inspection_hold.sav";
    SOL_REQUIRE(g.world.saveTo(path.c_str(), "held"));
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    SOL_REQUIRE(g.world.heldForInspection());

    SOL_REQUIRE(g.world.loadFrom(path.c_str()));
    SOL_CHECK(!g.world.heldForInspection());
    SOL_CHECK(!g.world.driveLockedByInspection());
}
