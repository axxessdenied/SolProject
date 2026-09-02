// One registry per instantiated system (engine plan Phase 38 stage A).
//
// ⚑⚑⚑⚑ THE FRAME IS A PROPERTY OF THE REGISTRY, NOT A FIELD ON THE ENTITY, AND
// THAT AMENDS `decisions/015`. Its first bullet said "an entity gains a system
// index"; the user's ruling made it structural instead, so a cross-system
// question is UNASKABLE rather than merely wrong — the other system's entities
// are not in these pools. The reason is this project's own evidence: Phase 37
// shipped two stages whose entire point was invisible to every guard they wrote
// (361 of 361 green, then 366 of 366), and both were missing filters.
//
// ⚑⚑⚑ STAGE A IS A REFACTOR AND ITS EXIT IS THAT NOTHING MOVED. The plural is
// built and exercised — a jump opens the destination's bubble, migrates the
// player into it and releases the one behind — but nothing yet keeps a second
// bubble alive. That is stage C, and it is what makes the count below
// interesting. What this file pins is the invariant while it is still one, plus
// the two things that would silently break the moment it is not.

#include "asset_paths.hpp"
#include "content.hpp"
#include "space_world.hpp"

#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/ecs/ecs.hpp>
#include <sol/test/test.hpp>

namespace {

// The shipped galaxy through the real content path, mod layer included — the
// same harness `notice_tests` uses, and for the same reason: without the mods
// this is an 81-system galaxy nobody flies.
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

    // A system with a gate and a station, so arriving in it is an ordinary
    // arrival rather than an edge case.
    [[nodiscard]] std::uint32_t furnishedSystem(std::uint32_t skip) const
    {
        std::uint32_t seen = 0;
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            const sol::sim::SystemSpec& spec = world.galaxy().systems[s];
            if (spec.stations.empty() || spec.gates.empty() || s == world.currentSystemIndex()) {
                continue;
            }
            if (seen++ == skip) {
                return s;
            }
        }
        return 0xffff'ffffu;
    }
};

} // namespace

// ⚑⚑⚑⚑ THE STAGE'S OWN INVARIANT, AND THE LINE STAGE C CHANGES. Exactly one
// system is instantiated at a time and it is the one the player is standing in.
// Everything else in stage A is a refactor whose exit is that nothing moved;
// this is the one new fact, and it is stated so that the stage which stops it
// from being true has to come here and say so.
SOL_TEST(the_world_instantiates_exactly_one_system_and_it_is_the_players)
{
    Galaxy g;
    SOL_CHECK(g.world.instantiatedSystemCount() == 1);
    SOL_CHECK(g.world.instantiatedSystemAt(0) == g.world.currentSystemIndex());

    // Over a run of jumps, not just at rest: a bubble leaked per crossing would
    // show up here and nowhere else, because every other reader in the game
    // asks the player's bubble and would go on being right.
    for (std::uint32_t hop = 0; hop < 6; ++hop) {
        const std::uint32_t destination = g.furnishedSystem(hop);
        SOL_REQUIRE(destination != 0xffff'ffffu);
        SOL_REQUIRE(g.world.enterSystem(destination));
        SOL_CHECK(g.world.currentSystemIndex() == destination);
        SOL_CHECK(g.world.instantiatedSystemCount() == 1);
        SOL_CHECK(g.world.instantiatedSystemAt(0) == destination);
    }
}

// ⚑⚑⚑⚑ RE-ENTERING THE SYSTEM YOU ARE IN REBUILDS ITS SKY RATHER THAN DOUBLING
// IT, AND THIS IS THE BUG THE PLURAL REGISTRY ACTUALLY CAUSED. The teardown it
// replaced destroyed everything but the player unconditionally, so arriving
// where you already were was a clean rebuild for free. A "migrate the player,
// drop the OLD bubble" reading does not cover the case where the old bubble and
// the destination are the same one — the sky is then built a second time on top
// of the first. It surfaced as `posting_a_picket_adds_no_hulls_to_the_sky`
// counting a doubled garrison; this is the same fact said directly.
SOL_TEST(re_entering_the_system_you_are_in_rebuilds_its_sky_rather_than_doubling_it)
{
    Galaxy g;
    const std::uint32_t here = g.furnishedSystem(0);
    SOL_REQUIRE(here != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(here));

    const std::uint32_t first = g.world.entityCount();
    SOL_REQUIRE(first > 1); // anti-vacuity: there is a sky to double
    SOL_REQUIRE(g.world.enterSystem(here));
    SOL_CHECK(g.world.entityCount() == first);
    SOL_REQUIRE(g.world.enterSystem(here));
    SOL_CHECK(g.world.entityCount() == first);
    SOL_CHECK(g.world.instantiatedSystemCount() == 1);
}

// ⚑⚑⚑⚑ WHAT MOVES WITH YOU IS WHAT THE WORLD KEEPS. A jump is now an entity
// crossing between two registries, and the components that survive the crossing
// are exactly the ones the snapshot schema names — the same list the save
// writes. The hazard this pins is a component the PLAYER carries that the
// schema does not know about: it would be dropped on the first jump, silently,
// and read as a ship that had been reset rather than as one that had been lost.
//
// ⚑ The fire group is the perturbation because it is the one thing a test can
// write into a migrated component through the shipped public API, and because
// a ship REBUILT from its def would come back on group 1. `setFireGroup` also
// writes the `OwnedShip` fitting, which is not an entity and would survive
// either way — so the assertion is on `playerGuns()`, which is a view of the
// live component.
SOL_TEST(a_jump_carries_the_players_ship_rather_than_rebuilding_it)
{
    Galaxy g;
    const sol::assets::ShipDef* def = g.content.defs().findShip(g.world.activeShip().defId.c_str());
    SOL_REQUIRE(def != nullptr);
    SOL_REQUIRE(!g.world.playerGuns().empty());
    const std::uint32_t mount = g.world.playerGuns()[0].mount;
    SOL_REQUIRE(mount < def->mounts.size());
    SOL_REQUIRE(g.world.setFireGroup(def->mounts[mount].id.c_str(), 3, nullptr));

    const std::size_t guns = g.world.playerGuns().size();
    const std::size_t mounts = g.world.playerMounts().size();
    const double credits = g.world.playerCredits();
    SOL_REQUIRE(mounts > 0);
    SOL_REQUIRE(g.world.playerGuns()[0].group == 3);

    const std::uint32_t destination = g.furnishedSystem(0);
    SOL_REQUIRE(destination != 0xffff'ffffu);
    SOL_REQUIRE(g.world.enterSystem(destination));

    // The same ship, carried, not a fresh one built from the same def.
    SOL_REQUIRE(g.world.playerGuns().size() == guns);
    SOL_CHECK(g.world.playerGuns()[0].group == 3);
    SOL_CHECK(g.world.playerMounts().size() == mounts);
    SOL_CHECK(g.world.playerCredits() == credits);
    // And two more jumps do not wear it down: the crossing is idempotent in
    // the sense that matters, which a migrate that moved only SOME components
    // would fail on the second hop rather than the first.
    SOL_REQUIRE(g.world.enterSystem(g.furnishedSystem(1)));
    SOL_REQUIRE(g.world.enterSystem(g.furnishedSystem(2)));
    SOL_REQUIRE(g.world.playerGuns().size() == guns);
    SOL_CHECK(g.world.playerGuns()[0].group == 3);
    SOL_CHECK(g.world.playerMounts().size() == mounts);
}

// ⚑⚑⚑⚑ A FOREIGN REGISTRY'S SLOT ZERO IS NOT THE PLAYER, AND AN INDEX
// COMPARISON CANNOT TELL. "Is this the player" was written sixteen times in
// `space_world.cpp` as `entityIndex == playerEntityIndex()`. Indices are issued
// per registry and every registry starts at zero, so the moment a second bubble
// exists that comparison is a coincidence — `Projectile::shooterIndex` is the
// concrete case, because a bolt outlives the jump that leaves it behind and the
// death path decides a kill was the player's this way.
//
// The component that answers it correctly has existed since Phase 7 and was
// never asked: `PlayerShip` appeared at exactly three places in the file —
// registered in the schema, emplaced once, counted on load.
SOL_TEST(a_foreign_registrys_slot_zero_is_not_the_player)
{
    Galaxy g;
    SOL_REQUIRE(!g.world.playerGuns().empty()); // the player is findable at all

    // A bubble that is not the player's, with an entity in the slot the player
    // occupies in theirs. This is what stage C's cooling bubble is made of,
    // arranged by hand because stage A never keeps one.
    sol::ecs::Registry elsewhere;
    game::SpaceWorld::ensureWorldPools(elsewhere);
    const sol::ecs::Entity resident = elsewhere.create();
    SOL_REQUIRE(resident.index == 0); // the same number the player answers to
    SOL_CHECK(!game::SpaceWorld::isPlayerEntity(elsewhere, resident.index));

    // And the player's own bubble still says yes to exactly one entity.
    std::size_t players = 0;
    for (std::uint32_t i = 0; i < g.world.entityCount() + 8; ++i) {
        players += game::SpaceWorld::isPlayerEntity(elsewhere, i) ? 1 : 0;
    }
    SOL_CHECK(players == 0);
}

// ⚑⚑⚑ A BUBBLE CAN BE READ FOR A POOL NOTHING EVER PUT ANYTHING IN.
// `Registry::storage<T>() const` ASSERTS the pool exists, which never mattered
// while one registry served a whole run — everything had been emplaced at least
// once by the time anything read it. A bubble is fresh, and a system with no NPC
// in it never emplaces a `ShipPilot`, so the first const read is the assert.
SOL_TEST(a_fresh_bubble_can_be_read_for_a_pool_nothing_has_touched)
{
    sol::ecs::Registry fresh;
    game::SpaceWorld::ensureWorldPools(fresh);
    const sol::ecs::Registry& readOnly = fresh;
    SOL_CHECK(readOnly.storage<game::ShipPilot>().empty());
    SOL_CHECK(readOnly.storage<game::Projectile>().empty());
    SOL_CHECK(readOnly.storage<game::MineableRock>().empty());
    SOL_CHECK(readOnly.storage<game::TraderPuppet>().empty());
    SOL_CHECK(readOnly.storage<game::MinerPuppet>().empty());
    SOL_CHECK(readOnly.storage<game::PlayerShip>().empty());
}
