// The transponder: the one piece of legal state that is a SWITCH rather than a
// consequence (engine plan Phase 36 stage A, docs/decisions/017).
//
// ⚑⚑⚑⚑ THE ASSERTION THAT MATTERS MOST HERE IS THE PRICE, NOT THE FIELD. A
// bool that saves is four lines and no test would be interesting. What makes
// this stage a mechanic rather than a lamp is the user's ruling that running
// dark COSTS you docking clearance before you have ever been caught — because
// without a price paid up front, dark is strictly dominant, a player flips it
// once on session one and never touches it again, and Phase 36 stage B would be
// tuning a condition nobody ever leaves. So the tests below check the refusal
// and the revocation, and treat the round trip as the cheap half.
//
// ⚑⚑ THE REFUSAL LIVES IN TWO PLACES AND THE LAST TEST HERE EXERCISES THE
// AUTHORED ONE. The policy is in the `dock_request` Lua hook so a mod can write
// a station that does not care; the same rule is repeated in C++'s scriptless
// default, because a price that exists only inside `init.lua` vanishes the
// moment somebody's script errors, and this one is the entire idle cost of the
// switch. `a_station_will_not_clear_a_ship_that_will_not_identify_itself`
// builds a real `GameContent` over the shipped scripts, so it proves the HOOK.
// ⚑ The C++ default is therefore the half these tests do NOT cover, and that is
// named here rather than left to be discovered: reaching it needs a
// `GameContent` with the hook absent, which nothing in this suite sets up today.
//
// ⚑ Phase 33 stage D's own file records why a galaxy-level assertion is only as
// good as the galaxy: these use a fixed seed and check the RULE at a system the
// fixture proves exists, rather than sampling and hoping.

#include "content.hpp"
#include "space_world.hpp"

#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::platform::createDirectories;

namespace {

// Enough to generate a galaxy AND round-trip a save. The two commodities are
// load-bearing rather than padding, for the reason `save_format_tests.cpp`
// records in capitals: `saveTo` writes the economy unconditionally while
// `loadFrom` reads it back only when the commodity list is non-empty, so a
// fixture with none produces a save whose every later field reads at the wrong
// offset — and this file's whole point is a field at the very end of that save.
constexpr const char* kDefs = R"(
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
model = "ship"
max_speed = 220.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[faction]]
id = "sol.hegemony"
name = "Ironstar Hegemony"
color = [0.75, 0.22, 0.28]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5
)";

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    explicit Fixture(std::uint64_t seed = 1701)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        world.spawn(seed);
        // applyDefs before generateUniverse, which is the order the game boots
        // in and is load-bearing rather than tidy — `generateUniverse` calls
        // `initializeFactions`, which returns immediately with no def database
        // and leaves the faction table empty.
        world.applyDefs(defs);
        SOL_CHECK(world.generateUniverse(defs));
    }
};

std::string scratchPath(const char* leaf)
{
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/transponder";
    SOL_CHECK(createDirectories(dir.c_str()));
    return dir + "/" + leaf;
}

} // namespace

// A new pilot is broadcasting. Stated rather than assumed because the default
// is the whole reason the DARK chip is drawn on the negative: if a new game
// opened dark, every station in the galaxy would refuse the player and the
// first thing they would meet is a wall with no explanation.
SOL_TEST(a_new_pilot_is_broadcasting_and_can_be_heard)
{
    Fixture fixture;
    SOL_CHECK(fixture.world.transponderOn());
    SOL_CHECK(!fixture.world.runningDark());
    SOL_CHECK(!fixture.world.broadcastHeard().empty());
}

// ⚑⚑ THE IDENTITY IS DERIVED, SO THE TEST THAT MATTERS IS THAT IT IS STABLE
// WITHIN A RUN AND DIFFERENT BETWEEN RUNS. A registration that re-rolled would
// make every comms line a new ship; one that was constant across playthroughs
// would be a hardcoded string wearing a function's clothes.
SOL_TEST(the_registration_is_stable_within_a_run_and_differs_between_them)
{
    Fixture a;
    const std::string first = a.world.broadcastIdentity();
    SOL_CHECK(!first.empty());
    SOL_CHECK(first == a.world.broadcastIdentity()); // twice in a row, unmoved

    Fixture b(90210);
    SOL_CHECK(!b.world.broadcastIdentity().empty());
    SOL_CHECK(b.world.broadcastIdentity() != first);

    // ⚑ It names the hull it is flying, so a freighter does not broadcast as a
    // shuttle. The fixture flies the starter, so that is what it must say.
    SOL_CHECK(first.find("Shuttle") != std::string::npos);
}

// ⚑⚑⚑ GOING DARK AND BEING HEARD ARE DIFFERENT QUESTIONS AND THE SECOND IS THE
// ONE EVERY CONSUMER ASKS. `broadcastHeard` exists so no caller has to remember
// to check the switch and the identity separately — which is exactly the shape
// of bug that leaves a lamp lit backwards.
SOL_TEST(a_dark_ship_broadcasts_nothing_but_still_has_a_registration)
{
    Fixture fixture;
    const std::string lit = fixture.world.broadcastIdentity();

    SOL_CHECK(fixture.world.setTransponder(false));
    SOL_CHECK(fixture.world.runningDark());
    SOL_CHECK(fixture.world.broadcastHeard().empty());

    // The registration did not go anywhere — it is a fact about the hull, not
    // about the switch. Stage E's spoofer is what makes this line interesting.
    SOL_CHECK(fixture.world.broadcastIdentity() == lit);

    // Setting it to what it already is is a no-op, so a caller can tell a real
    // toggle from a repeat without tracking the previous state itself.
    SOL_CHECK(!fixture.world.setTransponder(false));
    SOL_CHECK(fixture.world.setTransponder(true));
    SOL_CHECK(fixture.world.broadcastHeard() == lit);
}

// ⚑⚑⚑⚑ THE FIRST TIME A CLEARANCE HAS EVER BEEN REVOKED FOR A REASON OTHER
// THAN RUNNING OUT OF SECONDS. Phase 8r built `DockClearance` as a timed,
// REVOCABLE grant and nothing in twenty-eight phases used the second half. The
// grant was made to somebody who was identifying themselves and they have
// stopped, so it goes — and it goes at the moment the switch is thrown rather
// than at the next approach check, which is Phase 35 stage D's lesson applied
// before the bug instead of after it: state is dropped where the thing that
// invalidates it HAPPENS, not where somebody later notices.
SOL_TEST(going_dark_drops_a_clearance_already_in_hand)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.world.grantDocking(0, 0, "Cleared for berth 1."));
    SOL_REQUIRE(fixture.world.hasClearance());

    SOL_CHECK(fixture.world.setTransponder(false));
    SOL_CHECK(!fixture.world.hasClearance());

    // ⚑ And coming back up does NOT hand it back. A clearance is something a
    // station grants, not something the ship holds a copy of — re-lighting the
    // transponder has to mean asking again, or the switch would be free after
    // all and the whole price would be a formality.
    SOL_CHECK(fixture.world.setTransponder(true));
    SOL_CHECK(!fixture.world.hasClearance());
}

// The switch survives a save and a load. This is the guard on `kSaveVersion`
// 34: nothing in the tree asserted the version number before this stage, so a
// field appended to the format had no test that could see it disappear.
SOL_TEST(the_switch_survives_a_save_and_a_load)
{
    const std::string path = scratchPath("dark.sav");
    {
        Fixture fixture;
        SOL_REQUIRE(fixture.world.setTransponder(false));
        SOL_REQUIRE(fixture.world.saveTo(path.c_str(), "dark"));
    }
    {
        Fixture fixture;
        SOL_REQUIRE(fixture.world.transponderOn()); // fresh world, lit
        SOL_REQUIRE(fixture.world.loadFrom(path.c_str()));
        SOL_CHECK(fixture.world.runningDark());
    }

    // ⚑ And the other way round, because a load that ALWAYS produced `false`
    // would pass the half above. This is the anti-vacuity check the file's own
    // rule asks for: a conserved total is not a checksum, and neither is one
    // direction of a round trip.
    const std::string lit = scratchPath("lit.sav");
    {
        Fixture fixture;
        SOL_REQUIRE(fixture.world.saveTo(lit.c_str(), "lit"));
    }
    {
        Fixture fixture;
        SOL_REQUIRE(fixture.world.setTransponder(false));
        SOL_REQUIRE(fixture.world.loadFrom(lit.c_str()));
        SOL_CHECK(fixture.world.transponderOn());
    }
}

// ⚑⚑ A BOUNTY IS PER FACTION AND `017` REFUSED THE GLOBAL ALTERNATIVE BY NAME.
// One number for all factions "would flatten the system's best feature", so the
// assertion that matters is that moving one leaves the others alone — a single
// counter behind a per-faction accessor would pass any test that only ever set
// and read one index.
SOL_TEST(a_bounty_is_posted_by_one_faction_and_not_by_the_others)
{
    Fixture fixture;
    sol::sim::FactionSim& factions = fixture.world.factionSim();
    SOL_REQUIRE(fixture.world.factions().size() >= 2);

    SOL_CHECK(!factions.wanted());
    SOL_CHECK(factions.bounty(0) == 0.0f);

    factions.setBounty(0, 2'500.0f);
    SOL_CHECK(factions.wanted());
    SOL_CHECK(factions.bounty(0) == 2'500.0f);
    SOL_CHECK(factions.bounty(1) == 0.0f); // the whole point of "per faction"

    // It accumulates and settles, and settling past zero does not leave a
    // faction owing you money.
    factions.addBounty(0, 500.0f);
    SOL_CHECK(factions.bounty(0) == 3'000.0f);
    factions.addBounty(0, -10'000.0f);
    SOL_CHECK(factions.bounty(0) == 0.0f);
    SOL_CHECK(!factions.wanted());

    // ⚑ A bounty is NOT standing and the two must not be folded: this one is
    // credits and that one is a -100..100 band. Sharing `clampScore` would have
    // silently capped every price in the game at a hundred, which is a bug that
    // looks exactly like a balance decision.
    factions.setBounty(1, 12'000.0f);
    SOL_CHECK(factions.bounty(1) == 12'000.0f);
}

// ⚑⚑⚑⚑ THE RULING ITSELF, THROUGH THE REAL `GameContent` AND THE SHIPPED
// `init.lua`. Everything above this point tests a bool and a vector; THIS is
// the test that says running dark is a mechanic rather than a lamp, because it
// is the only one that exercises the price. A station will not clear a contact
// that will not identify itself — and the same pilot, at the same station, one
// switch later, is cleared.
//
// ⚑⚑ IT IS THE SAME STATION AND THE SAME SECOND ON PURPOSE. A test that went
// dark at one dock and lit at another would pass just as happily against a rule
// keyed on the station, the faction, the roll, or the phase of the galaxy —
// which is Phase 33 stage D's recorded lesson about assertions that sample
// instead of stating the rule.
SOL_TEST(a_station_will_not_clear_a_ship_that_will_not_identify_itself)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed); // registers the component storages
    game::GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    // Somewhere inside kDockRequestRange of a station, not docked.
    SOL_REQUIRE(world.warpToStationOffset(0, {200.0, 0.0, 0.0}));
    SOL_REQUIRE(!world.isDocked());

    // Dark: asked, and refused.
    SOL_REQUIRE(world.setTransponder(false));
    SOL_REQUIRE(world.requestDocking());
    content.tick(1.0 / 30.0);
    SOL_CHECK(!world.hasClearance());

    // ⚑ And refused FOR THE RIGHT REASON. Without this the test would pass
    // against a station that simply refuses everybody — which is exactly what a
    // hostile owner does, and is the case the refusal is ordered above.
    bool saidWhy = false;
    for (const game::SpaceWorld::CommsMessage& line : world.comms()) {
        if (line.text.find("Unidentified") != std::string::npos) {
            saidWhy = true;
            // ⚑⚑ THE LENGTH IS PART OF THE ASSERTION, AND A LIVE DRIVE IS WHAT
            // PUT IT HERE. The first wording ran to 58 characters and the comms
            // panel is "wide enough for a ~50-character line ... measured, not
            // guaranteed" (`game_ui.cpp`), so the refusal reached a player as
            // "Squawk your transponder or s". `init.lua` warns about exactly
            // this two lines above the hook and it still had to be seen on
            // screen to be believed — no test in the tree measures a string
            // against a panel, and this one only checks the budget it knows.
            SOL_CHECK(line.text.size() <= 50);
        }
    }
    SOL_CHECK(saidWhy);

    // Lit: the same pilot, the same station, cleared.
    SOL_REQUIRE(world.setTransponder(true));
    SOL_REQUIRE(world.requestDocking());
    content.tick(1.0 / 30.0);
    SOL_CHECK(world.hasClearance());
}

SOL_TEST(a_posted_bounty_survives_a_save_and_a_load)
{
    const std::string path = scratchPath("bounty.sav");
    {
        Fixture fixture;
        fixture.world.factionSim().setBounty(1, 7'400.0f);
        SOL_REQUIRE(fixture.world.saveTo(path.c_str(), "wanted"));
    }
    {
        Fixture fixture;
        SOL_REQUIRE(!fixture.world.factionSim().wanted());
        SOL_REQUIRE(fixture.world.loadFrom(path.c_str()));
        SOL_CHECK(fixture.world.factionSim().bounty(1) == 7'400.0f);
        SOL_CHECK(fixture.world.factionSim().bounty(0) == 0.0f);
    }
}
