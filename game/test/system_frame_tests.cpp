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
//
// ⚑⚑⚑⚑ STAGE B IS THE TICK NESTING, AND ITS GUARDS ARE THE SECOND HALF OF THIS
// FILE. Eight of the profiler's zones became per-system, the four death-path
// attributions stopped being keyed on where the PLAYER is standing, and both
// puppet reconciles turned `== m_currentSystem` into a walk over instantiated
// bubbles. Stage B still never opens a second bubble on its own — the count
// above is still 1 for a whole played session, and stage C is the policy that
// changes it — so these tests open one by hand through `instantiateSystem`,
// which is the mechanism half stage C will call and nothing more.
//
// ⚑⚑⚑ WHY A CONTROL WORLD RATHER THAN AN ASSERTION ABOUT ONE. The failure mode
// of a nested tick is not a crash, it is a shared thing that two systems both
// write: a scratch buffer refilled by whoever went last, a random stream drawn
// from k times instead of once, a cooldown that throttles the whole galaxy.
// None of those show up in a world that only ever ticks one system, and all of
// them show up as a second world diverging from the first.

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

    // Runs `frames` ticks at a fixed step with no input, which is what an
    // untouched world does while the player sits at the spawn point.
    void run(int frames, double dt = 1.0 / 60.0)
    {
        for (int i = 0; i < frames; ++i) {
            world.tick(dt);
        }
    }

    [[nodiscard]] game::SpaceWorld::BubbleReport report(std::size_t slot) const
    {
        game::SpaceWorld::BubbleReport out;
        SOL_CHECK(world.bubbleReportAt(slot, out));
        return out;
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

// ⚑⚑⚑⚑ A SECOND SYSTEM IS TICKED AND THE PLAYER'S CANNOT TELL, WHICH IS BOTH
// HALVES OF WHAT STAGE B CLAIMS. The nesting is only worth having if the second
// pass does real work, and it is only safe if the first pass comes out
// identical — and the second of those is the one nothing else in this suite can
// see, because every other reader in the game asks the player's bubble and goes
// on being right while a shared buffer is trampled underneath it.
//
// ⚑⚑⚑ THE CONTROL IS A WHOLE SECOND WORLD, NOT A REMEMBERED NUMBER. What the
// nesting can break is anything two systems both write: `m_avoidance` and the
// collision body list (refilled per bubble, so whoever ran last wins), a random
// stream drawn k times instead of once, the law's dispatch throttle. Every one
// of those reads as a plausible world on its own and as a divergence here.
SOL_TEST(a_second_system_is_ticked_and_the_players_own_sky_is_unchanged)
{
    Galaxy control;
    Galaxy nested;
    const std::uint32_t elsewhere = nested.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu);
    SOL_REQUIRE(elsewhere != nested.world.currentSystemIndex());
    SOL_REQUIRE(nested.world.instantiateSystem(elsewhere));
    SOL_CHECK(nested.world.instantiatedSystemCount() == 2);
    // The player is still the front bubble, which is what every player-scoped
    // reader in the game depends on.
    SOL_CHECK(nested.world.instantiatedSystemAt(0) == nested.world.currentSystemIndex());
    SOL_CHECK(nested.world.instantiatedSystemAt(1) == elsewhere);
    SOL_CHECK(nested.report(0).player);
    SOL_CHECK(!nested.report(1).player);

    control.run(180);
    nested.run(180);

    // Tick for tick: the same ship, in the same place, going the same way.
    const sol::sim::ShipState a = control.world.shipState();
    const sol::sim::ShipState b = nested.world.shipState();
    SOL_CHECK(a.position.x == b.position.x);
    SOL_CHECK(a.position.y == b.position.y);
    SOL_CHECK(a.position.z == b.position.z);
    SOL_CHECK(a.velocity.x == b.velocity.x);
    SOL_CHECK(a.velocity.y == b.velocity.y);
    SOL_CHECK(a.velocity.z == b.velocity.z);

    // And the same sky around it, down to the last hull and the last bolt.
    const game::SpaceWorld::BubbleReport left = control.report(0);
    const game::SpaceWorld::BubbleReport right = nested.report(0);
    SOL_CHECK(left.entities == right.entities);
    SOL_CHECK(left.ships == right.ships);
    SOL_CHECK(left.projectiles == right.projectiles);
    SOL_CHECK(left.worstHull == right.worstHull);
    SOL_CHECK(left.shipCentroid.x == right.shipCentroid.x);
    SOL_CHECK(left.shipCentroid.y == right.shipCentroid.y);
    SOL_CHECK(left.shipCentroid.z == right.shipCentroid.z);
    SOL_CHECK(control.world.instantiatedSystemCount() == 1);
}

// ⚑⚑⚑⚑ AND THE SECOND BUBBLE IS BEING FLOWN, NOT PARKED — the anti-vacuity of
// the test above, and it has to be said separately because a bubble that is
// merely HELD would pass every assertion in it. A system nothing ticks keeps
// its hulls exactly where `spawnAmbientPilots` put them: same count, same
// hulls, same positions, forever. So the claim is that its traffic MOVED, which
// only the per-system flight pass can do.
//
// ⚑⚑ IT ALSO PINS `spawnWing` PUTTING A HULL AND ITS PILOT IN THE SAME
// REGISTRY. That line was the one thing in the whole spawn path stage B did not
// hand a bubble, and it read as correct: both registries have a `ShipPilot`
// pool, so it compiled, and the player's own sky was still right. What it built
// was a ship in one registry whose pilot component sat in another — a hull
// nothing would ever fly, and an entry in the player's pilot pool pointing at
// somebody else's entity index.
SOL_TEST(a_bubble_the_player_is_not_in_flies_its_own_traffic)
{
    Galaxy g;
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu);
    const std::size_t playerShipsBefore = g.report(0).ships;
    SOL_REQUIRE(g.world.instantiateSystem(elsewhere));

    const game::SpaceWorld::BubbleReport fresh = g.report(1);
    SOL_REQUIRE(fresh.system == elsewhere);
    // The pilots landed in ITS registry, not in the player's.
    SOL_REQUIRE(fresh.ships > 0);
    SOL_CHECK(g.report(0).ships == playerShipsBefore);
    SOL_REQUIRE(fresh.entities > fresh.ships); // stations and gates, in its own frame

    g.run(240);
    const game::SpaceWorld::BubbleReport flown = g.report(1);
    SOL_CHECK(flown.system == elsewhere);
    // Something moved. Four seconds of station-keeping is enough: an idle
    // pilot still holds position with the assist on, which is thrust.
    const double drift = length(flown.shipCentroid - fresh.shipCentroid);
    SOL_CHECK(drift > 0.0);
    // And the player's own sky is still the one being reported to the player.
    SOL_CHECK(g.report(0).player);
}

// ⚑⚑⚑⚑ A KILL WHERE THE PLAYER IS NOT LEAVES ITS WRECK WHERE IT DIED. This is
// the stage's headline correction and it is worth spelling out what the bug
// was: `handleShipDestroyed` charged the mission kill, the contest pressure,
// the trader loss AND the wreck to `m_currentSystem` — the system the PLAYER is
// standing in. For thirty phases that was the same system the dying ship was
// in, because there was only one. The wreck is the one that shows: a
// `WreckRecord` carries a `DVec3` in its own system's frame, so a hauler shot
// two jumps away would have left a derelict floating in open space beside the
// player's star, at coordinates that mean nothing there.
//
// ⚑⚑ THE LEVER IS `killCoarseTrader`, WHICH IS ALSO FIXED BY BEING USED HERE.
// It looked for the trader's body in the player's registry only, because that
// was the only registry; its own contract is "kill it the way the running game
// would, through its body if it has one", and a body in another instantiated
// system is still a body.
SOL_TEST(a_kill_in_a_bubble_the_player_is_not_in_is_charged_to_that_system)
{
    Galaxy g;
    const std::uint32_t here = g.world.currentSystemIndex();

    // The system is chosen by where a hauler already IS rather than the other
    // way round: a coarse leg runs for minutes and the fleet is spread over
    // eighty systems, so picking a system first and waiting for traffic is
    // waiting for a coincidence.
    std::uint32_t victim = 0xffff'ffffu;
    std::uint32_t elsewhere = 0xffff'ffffu;
    const std::uint32_t fleet = static_cast<std::uint32_t>(g.world.economy().traders().size());
    for (std::uint32_t t = 0; t < fleet; ++t) {
        const sol::sim::TraderRoute route = g.world.economy().route(t);
        if (route.system == here || route.system >= g.world.galaxy().systems.size() ||
            route.leg == sol::sim::TraderLeg::None || route.leg == sol::sim::TraderLeg::Jump) {
            continue;
        }
        victim = t;
        elsewhere = route.system;
        break;
    }
    SOL_REQUIRE(victim != 0xffff'ffffu); // anti-vacuity: there is a hauler to kill
    SOL_REQUIRE(g.world.instantiateSystem(elsewhere));

    // One tick to give it a body. The reconcile that does so runs inside the
    // tick, per bubble, which is itself the `== m_currentSystem` to `in the
    // instantiated set` change this stage made: before it, a hauler flying a
    // leg anywhere but the player's system got no body from any pass.
    const std::size_t shipsBefore = g.report(1).ships;
    g.run(1);
    SOL_REQUIRE(g.report(1).ships > shipsBefore);

    std::vector<std::uint32_t> before;
    std::vector<std::uint32_t> beforeHere;
    g.world.mining().wrecksIn(elsewhere, before);
    g.world.mining().wrecksIn(here, beforeHere);

    SOL_REQUIRE(g.world.killCoarseTrader(victim));

    std::vector<std::uint32_t> after;
    std::vector<std::uint32_t> afterHere;
    g.world.mining().wrecksIn(elsewhere, after);
    g.world.mining().wrecksIn(here, afterHere);
    // The wreck is in the system the hauler died in...
    SOL_CHECK(after.size() == before.size() + 1);
    // ...and NOT in the one the player happens to be standing in, which is
    // exactly what this read before the four attributions were re-keyed.
    SOL_CHECK(afterHere.size() == beforeHere.size());

    // ⚑⚑ AND THE HULL IS THERE TO CUT INTO, IN ITS OWN SKY. The reconcile that
    // materialises a wreck record as an entity is the fine half of mining, and
    // it asked `m_currentSystem` for the list until this stage - so a wreck
    // left in a bubble the player is not in got no hull at all, while the
    // player's own wrecks would have been drawn into every other bubble at
    // coordinates belonging to somewhere else.
    const std::size_t wrecksHere = g.report(0).wrecks;
    const std::size_t wrecksThere = g.report(1).wrecks;
    g.run(1);
    SOL_CHECK(g.report(1).wrecks == wrecksThere + 1);
    SOL_CHECK(g.report(0).wrecks == wrecksHere);
}

// ⚑⚑⚑ INSTANTIATING A SYSTEM TWICE DOES NOT BUILD ITS SKY TWICE. Stage A
// shipped this bug through the other door — `enterSystem` on the system you
// were already in doubled the garrison, because the teardown it replaced
// cleared unconditionally and made re-arrival free. This is the same room with
// a second entrance, and stage C will be opening and closing bubbles against a
// policy rather than by hand, so it is worth one line now.
SOL_TEST(instantiating_a_system_twice_does_not_double_its_sky)
{
    Galaxy g;
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu);
    SOL_REQUIRE(g.world.instantiateSystem(elsewhere));
    const game::SpaceWorld::BubbleReport once = g.report(1);
    SOL_REQUIRE(once.entities > 1);

    SOL_REQUIRE(g.world.instantiateSystem(elsewhere));
    SOL_CHECK(g.world.instantiatedSystemCount() == 2);
    SOL_CHECK(g.report(1).entities == once.entities);
    SOL_CHECK(g.report(1).ships == once.ships);

    // The player's own system, asked for while they are standing in it, is the
    // same question and gets the same answer: it is already instantiated.
    const std::size_t playerEntities = g.report(0).entities;
    SOL_REQUIRE(g.world.instantiateSystem(g.world.currentSystemIndex()));
    SOL_CHECK(g.world.instantiatedSystemCount() == 2);
    SOL_CHECK(g.report(0).entities == playerEntities);
}

// ⚑⚑ AND A JUMP STILL LEAVES EXACTLY ONE, WHICH IS STAGE C'S LINE AND NOT
// STAGE B'S. `leaveSystemFor` releases every bubble and opens a fresh one for
// the destination; that is what keeps `the_world_instantiates_exactly_one_
// system_and_it_is_the_players` true through a played session even though the
// machinery for more now exists and is exercised above. Said here so that the
// stage which starts retaining a bubble has to come and change it deliberately.
SOL_TEST(a_jump_still_releases_every_bubble_including_ones_opened_by_hand)
{
    Galaxy g;
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    const std::uint32_t destination = g.furnishedSystem(1);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && destination != 0xffff'ffffu);
    SOL_REQUIRE(g.world.instantiateSystem(elsewhere));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);

    SOL_REQUIRE(g.world.enterSystem(destination));
    SOL_CHECK(g.world.instantiatedSystemCount() == 1);
    SOL_CHECK(g.world.instantiatedSystemAt(0) == destination);
}

// ⚑⚑⚑⚑ A BUBBLE TICKS AGAINST ITS OWN STAR, AND THIS IS THE ONE FRAME BUG WITH
// NO SYMPTOM. `rebuildAvoidance` and the collision build both push the star and
// every planet into the list a system's ships dodge and collide with, and both
// read them off the bubble since stage B — they were plain members before, so
// every bubble would have been ticked against whichever system the PLAYER was
// standing in. Nothing crashes: two systems both place their contents around a
// barycentre origin, so a foreign star lands somewhere entirely plausible and
// just has the wrong size, and ships quietly dodge and ram the wrong thing.
SOL_TEST(a_bubble_ticks_against_its_own_star_and_not_the_players)
{
    Galaxy g;
    // A system whose star is a different size from the one the player is under,
    // because "it carries a star" is not a claim and "it carries THIS star" is.
    const std::uint32_t here = g.world.currentSystemIndex();
    std::uint32_t elsewhere = 0xffff'ffffu;
    for (std::uint32_t candidate = 0; candidate < g.world.galaxy().systems.size(); ++candidate) {
        if (candidate != here &&
            g.world.galaxy().systems[candidate].starRadius != g.world.galaxy().systems[here].starRadius) {
            elsewhere = candidate;
            break;
        }
    }
    SOL_REQUIRE(elsewhere != 0xffff'ffffu);
    SOL_REQUIRE(g.world.instantiateSystem(elsewhere));

    const game::SpaceWorld::BubbleReport mine = g.report(0);
    const game::SpaceWorld::BubbleReport theirs = g.report(1);
    SOL_CHECK(mine.starRadius == g.world.galaxy().systems[here].starRadius);
    SOL_CHECK(theirs.starRadius == g.world.galaxy().systems[elsewhere].starRadius);
    SOL_CHECK(theirs.starRadius != mine.starRadius);
    SOL_CHECK(mine.planets == g.world.galaxy().systems[here].planets.size());
    SOL_CHECK(theirs.planets == g.world.galaxy().systems[elsewhere].planets.size());

    // And they stay their own across a tick, which is where a rebuild from the
    // player's side data would put them back.
    g.run(60);
    SOL_CHECK(g.report(1).starRadius == g.world.galaxy().systems[elsewhere].starRadius);
    SOL_CHECK(g.report(0).starRadius == g.world.galaxy().systems[here].starRadius);
}

// ⚑⚑⚑⚑ HOW MANY BUBBLES ARE INSTANTIATED IS NOT A FACT ABOUT ANY OF THEM, AND
// THIS IS PHASE 37 STAGE B'S LESSON ARRIVING ONE LAYER DOWN. There, a sixteenth
// faction displaced every trader loss after it because they shared a random
// stream; here, the same shape is a per-system stream drawn k times instead of
// once, a scratch buffer whoever went last owns, or a cooldown that throttles
// the whole galaxy. Every one of those reads as a perfectly plausible world on
// its own — the only way to see it is to tick the SAME system beside a
// different number of others and demand the same answer.
SOL_TEST(a_system_ticks_the_same_however_many_others_are_instantiated)
{
    Galaxy alone;
    Galaxy crowded;
    const std::uint32_t watched = alone.furnishedSystem(0);
    const std::uint32_t extra = alone.furnishedSystem(1);
    const std::uint32_t third = alone.furnishedSystem(2);
    SOL_REQUIRE(watched != 0xffff'ffffu && extra != 0xffff'ffffu && third != 0xffff'ffffu);
    SOL_REQUIRE(watched != extra && watched != third && extra != third);

    SOL_REQUIRE(alone.world.instantiateSystem(watched));
    // The watched system goes in FIRST in both, so this is not a test about
    // slot order: what differs is only how many others are being ticked.
    SOL_REQUIRE(crowded.world.instantiateSystem(watched));
    SOL_REQUIRE(crowded.world.instantiateSystem(extra));
    SOL_REQUIRE(crowded.world.instantiateSystem(third));
    SOL_CHECK(alone.world.instantiatedSystemCount() == 2);
    SOL_CHECK(crowded.world.instantiatedSystemCount() == 4);

    alone.run(180);
    crowded.run(180);

    const game::SpaceWorld::BubbleReport left = alone.report(1);
    const game::SpaceWorld::BubbleReport right = crowded.report(1);
    SOL_REQUIRE(left.system == watched);
    SOL_REQUIRE(right.system == watched);
    SOL_REQUIRE(left.ships > 0); // anti-vacuity: there is traffic to disagree about
    SOL_CHECK(left.entities == right.entities);
    SOL_CHECK(left.ships == right.ships);
    SOL_CHECK(left.projectiles == right.projectiles);
    SOL_CHECK(left.worstHull == right.worstHull);
    SOL_CHECK(left.shipCentroid.x == right.shipCentroid.x);
    SOL_CHECK(left.shipCentroid.y == right.shipCentroid.y);
    SOL_CHECK(left.shipCentroid.z == right.shipCentroid.z);

    // And the player's own sky agrees with itself too, which is the same claim
    // said from the other end.
    SOL_CHECK(alone.report(0).shipCentroid.x == crowded.report(0).shipCentroid.x);
    SOL_CHECK(alone.report(0).entities == crowded.report(0).entities);
}
