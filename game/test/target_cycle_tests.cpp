#include "input_actions.hpp"
#include "space_world.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/input_bindings.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;

namespace {

constexpr const char* kDefs = R"(
[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0

# A second hull purely so the two spawns land at DIFFERENT distances:
# spawnShipFromDef places a ship 150 + 100 * scale metres ahead.
[[ship]]
id = "sol.hauler"
name = "Hauler"
model = "ship"
scale = 3.0
max_speed = 120.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5
)";

[[nodiscard]] DefDatabase& testDefs()
{
    static DefDatabase defs;
    static bool loaded = false;
    if (!loaded) {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        loaded = true;
    }
    return defs;
}

[[nodiscard]] std::unique_ptr<game::SpaceWorld> makeWorld()
{
    auto world = std::make_unique<game::SpaceWorld>();
    world->spawn(1701);
    SOL_CHECK(world->generateUniverse(testDefs()));
    return world;
}

} // namespace

// The note itself: stepping forward then back returns the selection you had,
// so a player who overshoots the thing they were aiming for can come back to
// it rather than walking the whole list again.
SOL_TEST(a_nav_step_back_undoes_a_nav_step_forward)
{
    auto world = makeWorld();
    const std::size_t start = world->currentTargetIndex();

    world->cycleNavTarget(1);
    const std::size_t stepped = world->currentTargetIndex();
    SOL_REQUIRE(stepped != start); // the list has more than one visible thing

    world->cycleNavTarget(-1);
    SOL_CHECK(world->currentTargetIndex() == start);
}

// A backward walk must skip undiscovered targets on exactly the terms the
// forward walk does - the fog rule lives in the cycle so that no downstream
// consumer (autopilot, hail, dock request, the scan) needs one of its own,
// and a reverse that ignored it would hand them a hidden slot.
SOL_TEST(a_nav_step_back_walks_past_the_fog)
{
    auto world = makeWorld();
    const std::span<const game::NavTarget> targets = world->navTargets();

    // Something must actually be hidden, or this proves nothing.
    std::size_t hidden = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        hidden += world->navTargetVisible(i) ? 0u : 1u;
    }
    SOL_REQUIRE(hidden > 0);

    // A full lap backwards only ever lands on visible slots, and comes home.
    const std::size_t start = world->currentTargetIndex();
    std::size_t visited = 0;
    for (std::size_t i = 0; i < targets.size() + hidden; ++i) {
        world->cycleNavTarget(-1);
        SOL_REQUIRE(world->currentTargetIndex() < targets.size());
        SOL_CHECK(world->navTargetVisible(world->currentTargetIndex()));
        ++visited;
        if (world->currentTargetIndex() == start) {
            break;
        }
    }
    SOL_CHECK(visited > 0);
    SOL_CHECK(world->currentTargetIndex() == start);
}

// Forward from a nav target lands on the head of the threat order, so
// backward lands on the tail. Asserted because it is the one place the two
// directions are deliberately NOT mirror images of the same step.
SOL_TEST(a_contact_step_back_from_a_nav_target_lands_on_the_tail)
{
    auto world = makeWorld();

    // ⚑ Spawn the contacts rather than hoping for them. This fixture calls
    // generateUniverse without initializeFactions, so no ambient wing ever
    // flies here - and a test that quietly returned on an empty order would
    // assert nothing at all while reading as a pass.
    const sol::assets::ShipDef* hull = testDefs().findShip("sol.shuttle");
    SOL_REQUIRE(hull != nullptr);
    for (int i = 0; i < 3; ++i) {
        (void)world->spawnPilotFromDef(*hull, testDefs(), game::PilotRole::Trader);
    }

    std::vector<std::size_t> order;
    world->contactOrder(order);
    SOL_REQUIRE(order.size() >= 3);

    const std::size_t navSlots = world->navTargets().size();

    world->cycleContact(1);
    SOL_CHECK(world->currentTargetIndex() == navSlots + order.front());

    // Back to a nav target, then step backwards into the contacts.
    world->cycleNavTarget(1);
    SOL_REQUIRE(world->currentTargetIndex() < navSlots);
    world->cycleContact(-1);
    SOL_CHECK(world->currentTargetIndex() == navSlots + order.back());
}

// The two new actions need real keys that nothing else already claims.
// installDefaultBindings uses bind(), which does NOT resolve conflicts, so a
// duplicate default would leave one of them dead on arrival with no error
// anywhere - the Controls screen would list it and pressing it would do what
// the other action does.
SOL_TEST(the_two_reverse_actions_have_unique_default_bindings)
{
    sol::platform::BindingTable table;
    game::installDefaultBindings(table);
    SOL_REQUIRE(table.actionCount() == game::kActionCount);

    for (const game::Action action : {game::Action::CycleNavTargetBack, game::Action::CycleContactBack}) {
        const std::uint32_t index = static_cast<std::uint32_t>(action);
        const sol::platform::InputChord chord = table.chordFor(index);
        SOL_CHECK(chord.bound());
        // find() answers with whoever holds the chord, so this fails if any
        // earlier row claims the same key.
        SOL_CHECK(table.find(chord) == index);
    }

    // And the ids the settings file will carry, which are stable forever.
    SOL_CHECK(std::strcmp(game::actionId(game::Action::CycleNavTargetBack), "cycle_nav_target_back") == 0);
    SOL_CHECK(std::strcmp(game::actionId(game::Action::CycleContactBack), "cycle_contact_back") == 0);
}

// ⚑⚑ THE CYCLE RANKS BY THREAT BEFORE DISTANCE, AND UNTIL PHASE 31 STAGE C2
// NOTHING MEASURED IT. The tiering was an anonymous lambda inside
// `contactOrder`; C2 promoted it to `SpaceWorld::threatTier` so a turret's
// decision to open fire and this ranking are one answer, and the counterfactual
// that stubbed the lambda out to a constant turned NOTHING red - a gap found by
// the move rather than caused by it.
//
// The discriminating half is the DISTANCE: the hostile is spawned on the bigger
// hull, which `spawnShipFromDef` places further out, so a ranking that sorted on
// range alone would put the neutral first. The neutral is spawned WITHOUT a
// pilot, which is what makes it tier 2 - nobody is flying it, so it threatens
// nothing.
SOL_TEST(the_contact_cycle_puts_a_hostile_ahead_of_a_nearer_neutral)
{
    auto world = makeWorld();
    const sol::assets::ShipDef* near_ = testDefs().findShip("sol.shuttle");
    const sol::assets::ShipDef* far_ = testDefs().findShip("sol.hauler");
    SOL_REQUIRE(near_ != nullptr && far_ != nullptr);

    // Nearer, and inert: no pilot, so nothing is flying it.
    (void)world->spawnShipFromDef(*near_, testDefs());
    // Further out, and unaffiliated - which Lua has treated as unconditionally
    // player-hostile since Phase 8b, and which `threatTier` still honours.
    (void)world->spawnPilotFromDef(*far_, testDefs(), game::PilotRole::Fighter);

    std::vector<std::size_t> order;
    std::vector<int> tiers;
    world->contactOrder(order, tiers);
    SOL_REQUIRE(order.size() == 2);
    SOL_CHECK(tiers[0] == game::SpaceWorld::kHostileThreatTier);
    SOL_CHECK(tiers[1] == 2);
    SOL_CHECK(order[0] == 1); // the hostile, spawned second and standing further off

    // And the "jump to whatever is shooting at me" lever agrees with the
    // ranking rather than keeping its own idea of hostile.
    SOL_CHECK(world->selectNearestHostile());
    SOL_CHECK(world->currentTargetIndex() == world->navTargets().size() + 1);
}
