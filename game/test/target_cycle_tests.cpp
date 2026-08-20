#include "input_actions.hpp"
#include "space_world.hpp"

#include <sol/assets/data_defs.hpp>
#include <sol/platform/input_bindings.hpp>
#include <sol/test/test.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using sol::assets::DefDatabase;

namespace {

constexpr const char* kDefs = R"(
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
    world->generateUniverse(testDefs());
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

    for (const game::Action action :
         {game::Action::CycleNavTargetBack, game::Action::CycleContactBack}) {
        const std::uint32_t index = static_cast<std::uint32_t>(action);
        const sol::platform::InputChord chord = table.chordFor(index);
        SOL_CHECK(chord.bound());
        // find() answers with whoever holds the chord, so this fails if any
        // earlier row claims the same key.
        SOL_CHECK(table.find(chord) == index);
    }

    // And the ids the settings file will carry, which are stable forever.
    SOL_CHECK(std::strcmp(game::actionId(game::Action::CycleNavTargetBack),
                          "cycle_nav_target_back")
              == 0);
    SOL_CHECK(std::strcmp(game::actionId(game::Action::CycleContactBack), "cycle_contact_back")
              == 0);
}
