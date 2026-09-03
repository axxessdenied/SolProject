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
// ⚑⚑⚑⚑ STAGE C IS THE POLICY, AND IT IS THE LAST SECTION OF THIS FILE. When a
// system stays open, for how long, and how many at once — plus the one bug
// keeping a bubble actually creates, which is that the slot the player vacates
// is the next one that registry hands out. Its headline is that the spec's own
// wording could not be built as written: see the block above
// `a_ship_left_damaged_in_a_system_is_still_there_and_still_damaged`.
//
// ⚑⚑⚑⚑ STAGE D IS THE OUTPUT PATHS, AND WHAT IT SHIPS IS AN ABSENCE. Every
// position in this game is metres in one system's barycentre frame, and the
// two cosmetic sinks - the mixer's single listener and the one particle buffer
// - had no idea such a thing existed, because until stage C there was only
// ever one frame to be in. Its tests are therefore shaped differently from the
// rest of this file: what they assert is that nothing happened, so each one
// carries its own proof that there was something to suppress.
//
// ⚑⚑⚑ AND THE COST OF GETTING IT WRONG IS NOT A NOISE IN THE WRONG PLACE, IT
// IS A SILENCE IN THE RIGHT ONE. `sounds.toml` caps `sol.explosion` at four
// concurrent voices and `Mixer::claimVoice` steals the oldest at the cap;
// `CombatEffects::kMaxParticles` is 2,000 for the whole game and `burst` drops
// the overflow. Both numbers were tuned against the only fight there could be.
// A fight in a cooling bubble spends them, and the player's own explosion is
// the one that goes missing.

// ⚑⚑⚑ WHY A CONTROL WORLD RATHER THAN AN ASSERTION ABOUT ONE. The failure mode
// of a nested tick is not a crash, it is a shared thing that two systems both
// write: a scratch buffer refilled by whoever went last, a random stream drawn
// from k times instead of once, a cooldown that throttles the whole galaxy.
// None of those show up in a world that only ever ticks one system, and all of
// them show up as a second world diverging from the first.

#include "asset_paths.hpp"
#include "combat_effects.hpp"
#include "content.hpp"
#include "game_audio.hpp"
#include "space_world.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/ecs/ecs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/sim/flight.hpp>
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

// ⚑⚑⚑⚑ STAGE A'S INVARIANT, NARROWED BY STAGE C TO THE THING THAT IS STILL
// TRUE — AND IT PASSED UNCHANGED, WHICH IS THE POINT WORTH RECORDING. Stage A
// said "exactly one system is instantiated at a time", and stated it so that
// the stage which stopped it being true would have to come here and say so.
// Stage C stopped it being true, and this test did not fail: six ordinary
// crossings retain nothing, because retention has an ENTRY CONDITION and a
// quiet system does not meet it.
//
// So what it pins now is the more useful half — **an ordinary jump costs
// nothing**. The cooling bubble is not a tax on every gate you fly through; it
// is a thing that happens when you leave a fight, and a player who never
// fights never pays for it. A retention policy that quietly held every system
// you passed through would sail through every other test in this file and
// would show up here and nowhere else.
SOL_TEST(a_quiet_crossing_retains_nothing_and_leaves_one_system_instantiated)
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

// ⚑⚑⚑⚑ A JUMP RELEASES A BUBBLE ON A DEAD CLOCK, AND STAGE C IS THE STAGE THAT
// CAME AND CHANGED THIS. Stage B's version asserted that a jump leaves exactly
// one bubble however many were open, and said so precisely to make the stage
// that starts retaining one arrive here deliberately. What survives of it is
// the half that is still true and is now a statement about the POLICY rather
// than about the mechanism: a system whose window has run out is released by
// the crossing, and a `kMaxInstantiatedSystems`-wide world does not leak.
//
// ⚑ `instantiateSystem` puts a bubble on a full window, so the clock is wound
// down here rather than waited out - `kCoolingSeconds` of sim time is 7,200
// ticks and this is a test, not a flight.
SOL_TEST(a_jump_releases_the_bubbles_whose_window_has_run_out)
{
    Galaxy g;
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    const std::uint32_t destination = g.furnishedSystem(1);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && destination != 0xffff'ffffu);
    SOL_REQUIRE(g.world.instantiateSystem(elsewhere));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);
    SOL_REQUIRE(g.report(1).holdSeconds == game::kCoolingSeconds);

    // Run the window out. The bubble is released by the tick's own sweep, and
    // the player has not gone anywhere - which is the half of the policy that
    // is nothing to do with jumping.
    g.run(static_cast<int>(game::kCoolingSeconds * 60.0) + 2);
    SOL_CHECK(g.world.instantiatedSystemCount() == 1);
    SOL_CHECK(g.world.instantiatedSystemAt(0) == g.world.currentSystemIndex());

    // And a jump out of a quiet system retains nothing, so the count comes back
    // to one across a crossing too.
    SOL_REQUIRE(g.world.enterSystem(destination));
    SOL_CHECK(g.world.instantiatedSystemCount() == 1);
    SOL_CHECK(g.world.instantiatedSystemAt(0) == destination);
    SOL_CHECK(g.report(0).holdSeconds == 0.0);
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

// ---------------------------------------------------------------------------
// Stage C: the retention policy and its cap.
//
// ⚑⚑⚑⚑ THE FINDING THAT SHAPED THE WHOLE STAGE, BECAUSE IT INVERTS THE SPEC'S
// OWN WORDING. The spec asks for a system that "stays instantiated while a
// fight in it is live — any `ShipPilot` in `Attack`, any `threatTimer`, any
// projectile". Read as a per-tick predicate that is unbuildable, and not for a
// subtle reason: THE PLAYER LEAVING IS WHAT ENDS THE FIGHT. `tickSystem`'s
// Attack case drops a pilot to Idle the instant `pilot.targetIndex` stops
// resolving, and the migrate retires the player out of the registry they were
// being shot in — so every raider that was shooting at you stands down one tick
// after you jump, and nobody re-engages, because the think pass is
// player-scoped (stage B's recorded LOD statement). `threatTimer` is six
// seconds and a bolt lives less. A "while live" reading therefore closes the
// bubble about five seconds into the minute the phase's own exit spends next
// door — every test green, and the feature simply never happening.
//
// So the fight is the ENTRY condition, asked once at the moment of departure,
// and `kCoolingSeconds` is the retention. The ceiling is not a backstop against
// a rare stalemate; it is the mechanism.
// ---------------------------------------------------------------------------

// ⚑⚑⚑⚑ THE PHASE'S EXIT, AND THE ENTITY HANDLE IS THE ASSERTION. Fight a ship
// down to a known hull fraction, jump out, spend a minute somewhere else, jump
// back, and find the SAME ship on the SAME damage.
//
// ⚑⚑⚑ "SAME DAMAGE" IS THE WEAKER HALF AND IT IS WORTH SAYING WHY. A sky built
// by `fillSystemSky` is a pure function of the spec and the seed, so a
// regenerated system comes back with the same ships at the same full health in
// the same places — an assertion about counts or hulls alone would pass over a
// world that had thrown the fight away and made a new one. What a regenerated
// sky CANNOT reproduce is a `sol::ecs::Entity` that was never destroyed: a
// fresh registry hands out its slots from zero. `shipHullFraction(mark)`
// answering at all, two crossings later, is the identity claim; the number it
// answers with is the damage claim; and the drift below is the claim that the
// bubble was ticked rather than parked.
//
// ⚑⚑⚑ AND THE SPEC'S OWN EXIT SENTENCE OVERSTATES IT, WHICH IS WORTH SAYING
// ONCE. "Fly out of a losing engagement, cross back, and the raider that was on
// 30% hull is still on 30% hull" describes a system that was PAUSED. The one
// this stage builds was RUNNING: the ship marked below goes out at 0.383 and
// comes back at 0.183, because the fight it was in carried on without the
// player in it - which is the feature, not a defect in it. So the equality is
// asserted across the CROSSING, where nothing may change, and an inequality
// across the minute, where a great deal may.
SOL_TEST(a_ship_left_damaged_in_a_system_is_still_there_and_still_damaged)
{
    Galaxy g;
    const std::uint32_t here = g.world.currentSystemIndex();
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && elsewhere != here);

    // A hull in the water with a pilot, so what is left behind is traffic
    // rather than scenery.
    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity mark = g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.shipHullFraction(mark) == 1.0);

    // Shoot it, through the player's own guns and the real damage path — which
    // is also what puts bolts in the air and a threat on the clock, the entry
    // condition the retention is about to be asked for.
    sol::sim::FlightInput trigger;
    trigger.trigger = true;
    for (int i = 0; i < 180; ++i) {
        g.world.setShipInput(trigger);
        g.world.tick(1.0 / 60.0);
    }
    g.world.setShipInput(sol::sim::FlightInput{});
    const double wounded = g.world.shipHullFraction(mark);
    SOL_REQUIRE(wounded < 1.0); // anti-vacuity: there is damage to preserve
    SOL_REQUIRE(wounded > 0.0); // and something survived to carry it

    // Out. The system behind stays open because a fight was live in it.
    SOL_REQUIRE(g.world.enterSystem(elsewhere));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);
    SOL_REQUIRE(g.world.instantiatedSystemAt(1) == here);
    const game::SpaceWorld::BubbleReport left = g.report(1);
    SOL_CHECK(left.holdSeconds == game::kCoolingSeconds);
    SOL_CHECK(!left.player);

    // A minute next door, which is the exit's own wording.
    g.run(60 * 60);
    const game::SpaceWorld::BubbleReport cooling = g.report(1);
    SOL_REQUIRE(cooling.system == here);
    // It was TICKED, not parked: its traffic is somewhere else than it was.
    SOL_CHECK(length(cooling.shipCentroid - left.shipCentroid) > 0.0);
    // And the clock ran down by about the minute that passed.
    SOL_CHECK(cooling.holdSeconds < left.holdSeconds - 59.0);
    SOL_CHECK(cooling.holdSeconds > 0.0);

    // Back. The bubble is taken up rather than rebuilt.
    SOL_REQUIRE(g.world.enterSystem(here));
    SOL_CHECK(g.world.instantiatedSystemCount() == 1);
    const game::SpaceWorld::BubbleReport home = g.report(0);
    SOL_CHECK(home.player);
    SOL_CHECK(home.holdSeconds == 0.0); // off the clock: the player is in it

    // ⚑⚑⚑⚑ THE EXIT, AND IT IS NOT THE EQUALITY THE SPEC'S SENTENCE IMPLIES.
    // "The raider that was on 30% hull is still on 30% hull" describes a bubble
    // that was FROZEN, and a cooling bubble is one that was RUNNING — this ship
    // went in at 0.383 and came out at 0.183, because the system kept fighting
    // over it while the player was a jump away. The exact claim is therefore
    // made across the CROSSING, where nothing may change at all, and the
    // inequality is made across the minute, where a great deal may.
    SOL_CHECK(home.worstHull == cooling.worstHull);
    SOL_CHECK(home.ships == cooling.ships);
    // The same entity: a handle into a registry that was rebuilt would not
    // resolve, and one into a regenerated sky would answer a full hull.
    const double after = g.world.shipHullFraction(mark);
    SOL_CHECK(after > 0.0);      // still there
    SOL_CHECK(after < 1.0);      // and not handed back a fresh spawn
    SOL_CHECK(after <= wounded); // nothing healed it while nobody was watching
}

// ⚑⚑⚑ AND THE SAME CROSSING WITHOUT A FIGHT GIVES YOU A NEW SYSTEM, WHICH IS
// THE ANTI-VACUITY OF THE TEST ABOVE AND THE ONLY WAY TO SEE THAT IT MEASURED
// ANYTHING. Every assertion in the exit test would read identically against a
// world that simply never released any bubble at all. What separates those is
// the negative: leave a QUIET system and the ship you left in it is gone,
// because the bubble was released and the next arrival generates a sky.
SOL_TEST(a_ship_left_in_a_quiet_system_is_not_there_when_you_come_back)
{
    Galaxy g;
    const std::uint32_t here = g.world.currentSystemIndex();
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && elsewhere != here);

    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity mark = g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.shipHullFraction(mark) == 1.0);

    // Nothing shoots at anything. Long enough for any threat the arrival itself
    // raised to age past `kThreatMemorySeconds`.
    g.run(10 * 60);
    SOL_REQUIRE(g.world.enterSystem(elsewhere));
    SOL_CHECK(g.world.instantiatedSystemCount() == 1); // released, not retained

    SOL_REQUIRE(g.world.enterSystem(here));
    SOL_CHECK(g.world.instantiatedSystemCount() == 1);
    // The handle names a registry that no longer exists. `shipHullFraction`
    // answers 0 for an entity that is not there.
    SOL_CHECK(g.world.shipHullFraction(mark) == 0.0);
}

// ⚑⚑⚑⚑ THE BUBBLE YOU LEFT HAS STOPPED FIGHTING YOU, AND THE ASSERTION IS
// TAKEN BEFORE A SINGLE TICK RUNS. That timing is the whole test. `Registry::
// create` pops a LIFO free list, so the slot the player vacates on a jump is
// the very next index that registry hands out — and a raider left holding
// `targetIndex` does not merely lose its target, it inherits a new one.
//
// ⚑⚑⚑⚑ AND THE MUTATION TEST SAYS THIS HAZARD IS CURRENTLY UNREACHABLE, WHICH
// IS RECORDED HERE RATHER THAN PAPERED OVER. Breaking `forgetDepartedPlayer`
// outright survives every assertion in this suite EXCEPT the one below, and the
// reason is an ordering nobody wrote down: the only things that spawn into a
// retained bubble are the two puppet reconciles, and they run in `sim.puppets`
// AFTER the per-bubble loop — so on the first tick after a jump the pilot pass
// reads a slot that is still empty, `tickSystem`'s Attack case drops the pilot
// to Idle, and the inheritor arrives to find nobody hunting it. The dangling
// index is real; the window in which it can be dereferenced is currently zero
// ticks wide.
//
// So `forgetDepartedPlayer` is structure rather than a live fix, exactly like
// stage B's per-system chunk stream — and it is worth having for the same
// reason: the alternative is a correctness property held up by the relative
// order of two passes in `tick`, with nothing anywhere saying so. What this
// test can still pin exactly is the instant of departure, where the difference
// between "forgotten" and "about to be forgotten" is visible.
SOL_TEST(the_system_you_jump_out_of_stops_fighting_you_the_moment_you_leave)
{
    Galaxy g;
    const std::uint32_t here = g.world.currentSystemIndex();
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && elsewhere != here);

    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity hunter =
        g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.pilotAttackPlayer(hunter));
    SOL_REQUIRE(g.world.pilotStateOf(hunter) == game::PilotState::Attack);
    SOL_REQUIRE(g.report(0).fighting > 0); // anti-vacuity: somebody IS fighting

    // Out, mid-attack. The fight is live, so the bubble is kept - which is the
    // only condition under which a stale target could ever be read at all.
    SOL_REQUIRE(g.world.enterSystem(elsewhere));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);
    SOL_REQUIRE(g.world.instantiatedSystemAt(1) == here);

    // ⚑ NO TICK HAS RUN. Nobody in that bubble is in `Attack` any more, and
    // that is a fact about `leaveSystemFor` rather than about the pilot pass
    // that would have reached the same answer one tick later.
    SOL_CHECK(g.report(1).fighting == 0);

    // And it stays that way, rather than being an instant that passes: nothing
    // re-engages in a bubble the player is not in, because the think pass is
    // player-scoped. That is stage B's LOD statement, read from outside.
    g.run(600);
    const game::SpaceWorld::BubbleReport left = g.report(1);
    SOL_REQUIRE(left.system == here);
    SOL_REQUIRE(left.ships > 0); // anti-vacuity: there is somebody to mis-target
    SOL_CHECK(left.fighting == 0);
    SOL_CHECK(left.projectiles == 0);
    SOL_CHECK(left.worstHull == 1.0f);
}

// ⚑⚑⚑⚑ THE CAP IS ASSERTED RATHER THAN CLAIMED, WHICH THE SPEC ASKS FOR IN
// THOSE WORDS — `sim::resolveCollisions` is O(n^2) with no broadphase, so k
// bubbles cost k*n^2 and this is the only thing that bounds k. Both doors are
// tried: `instantiateSystem`, which the console and these tests open a system
// through, and a retention, which is how the running game opens one.
//
// ⚑⚑ AND THE EVICTION IS COLDEST-FIRST. The bubble the player has just backed
// out of carries a full window, so it is never the one dropped — the fight you
// most recently ran from is the one you are most likely to turn around and fly
// back into. What goes is the one nearest release, which loses the least.
SOL_TEST(the_instantiated_system_count_never_passes_its_cap)
{
    Galaxy g;
    // Fill to the cap by hand and then keep asking. The refusal is the point:
    // a door that quietly ignored the cap would leave the pass unbounded.
    std::uint32_t opened = 0;
    for (std::uint32_t k = 0; k < 12; ++k) {
        const std::uint32_t candidate = g.furnishedSystem(k);
        if (candidate == 0xffff'ffffu) {
            break;
        }
        if (g.world.instantiateSystem(candidate)) {
            ++opened;
        }
        SOL_CHECK(g.world.instantiatedSystemCount() <= game::SpaceWorld::kMaxInstantiatedSystems);
    }
    SOL_REQUIRE(opened > 0); // anti-vacuity: the door works at all
    SOL_CHECK(g.world.instantiatedSystemCount() == game::SpaceWorld::kMaxInstantiatedSystems);

    // Now age them unevenly, so "coldest" names one bubble rather than a tie,
    // and check that a retention evicts THAT one and not the player's and not
    // the one just left.
    g.run(60);
    double coldest = game::kCoolingSeconds * 2.0;
    std::uint32_t coldestSystem = 0xffff'ffffu;
    for (std::size_t slot = 1; slot < g.world.instantiatedSystemCount(); ++slot) {
        const game::SpaceWorld::BubbleReport r = g.report(slot);
        if (r.holdSeconds < coldest) {
            coldest = r.holdSeconds;
            coldestSystem = r.system;
        }
    }
    SOL_REQUIRE(coldestSystem != 0xffff'ffffu);

    // A jump out of a live fight wants a slot that is not there.
    const std::uint32_t here = g.world.currentSystemIndex();
    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity hunter =
        g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.pilotAttackPlayer(hunter));

    std::uint32_t destination = 0xffff'ffffu;
    for (std::uint32_t k = 0; k < 12 && destination == 0xffff'ffffu; ++k) {
        const std::uint32_t candidate = g.furnishedSystem(k);
        if (candidate == 0xffff'ffffu) {
            break;
        }
        bool open = false;
        for (std::size_t slot = 0; slot < g.world.instantiatedSystemCount(); ++slot) {
            open = open || g.world.instantiatedSystemAt(slot) == candidate;
        }
        if (!open) {
            destination = candidate;
        }
    }
    SOL_REQUIRE(destination != 0xffff'ffffu); // anti-vacuity: somewhere new to go

    SOL_REQUIRE(g.world.enterSystem(destination));
    SOL_CHECK(g.world.instantiatedSystemCount() == game::SpaceWorld::kMaxInstantiatedSystems);
    // The player's is the front one, the system just left is retained, and the
    // coldest is the one that went.
    SOL_CHECK(g.world.instantiatedSystemAt(0) == destination);
    bool keptTheOneJustLeft = false;
    bool keptTheColdest = false;
    for (std::size_t slot = 0; slot < g.world.instantiatedSystemCount(); ++slot) {
        keptTheOneJustLeft = keptTheOneJustLeft || g.world.instantiatedSystemAt(slot) == here;
        keptTheColdest = keptTheColdest || g.world.instantiatedSystemAt(slot) == coldestSystem;
    }
    SOL_CHECK(keptTheOneJustLeft);
    SOL_CHECK(!keptTheColdest);
}

// ⚑⚑⚑ JUMPING BACK INTO A RETAINED BUBBLE DOES NOT BUILD ITS SKY A SECOND
// TIME. This is stage A's doubled-garrison bug through its THIRD door, and the
// first two are why it is worth a test of its own: `enterSystem` on the system
// you are standing in (stage A, caught by a picket count) and
// `instantiateSystem` called twice (stage B, said directly). Stage C adds an
// arrival that finds the destination already open, which is the one door where
// the sky in the bubble is not the sky the spec would generate — so a filled
// one would not merely be doubled, it would be a rebuilt system with a stale
// population inside it.
// ⚑⚑⚑⚑ WHAT THE SOFT CAP COSTS, MEASURED, BECAUSE THE RULING THAT MADE IT SOFT
// CAME WITH THAT OBLIGATION ATTACHED (Phase 39 stage C, the user's ruling 11).
// The cap went soft for captains: a system somebody paid for is never evicted,
// so the number of instantiated systems is bounded by how many hulls a player
// can afford rather than by a constant. That trades a known ceiling for an
// unknown one, and the arc's own standing rule is that a claim about COST is
// settled by measuring rather than by reading.
//
// Phase 38 stage C measured the same curve up to its cap and wrote the numbers
// into `kMaxInstantiatedSystems`' comment: one bubble 0.067 ms, four 0.47 ms,
// six 0.64 ms, debug, against a 16.7 ms frame. This continues that line PAST
// the cap, through the door the captain tick uses, so the shape past six is a
// measurement rather than an extrapolation. `sim::resolveCollisions` is O(n^2)
// per bubble with no broadphase, so the honest question is whether the curve
// bends.
//
// ⚑ IT PRINTS AND DOES NOT ASSERT A TIME. A wall clock in a test that runs on
// three platforms and in two configurations is a flake generator; what it
// asserts is that the bubbles it is timing actually exist, which is the only
// way the numbers can be wrong about what they measured.
SOL_TEST(what_a_bubble_costs_per_frame_past_the_cap)
{
    Galaxy g;
    constexpr int kFrames = 600;
    constexpr std::size_t kMost = 12;

    std::printf("  bubbles   ms/frame   per bubble\n");
    for (std::size_t target = 1; target <= kMost; target += (target < 6 ? 1 : 2)) {
        Galaxy sample;
        // Through the captain tick's own door, because that is the only one
        // that can go past the cap - and past the cap is the half Phase 38
        // could not measure.
        for (std::uint32_t k = 0; sample.world.instantiatedSystemCount() < target && k < 40; ++k) {
            const std::uint32_t candidate = sample.furnishedSystem(k);
            if (candidate == 0xffff'ffffu) {
                break;
            }
            (void)sample.world.instantiateSystem(candidate, true);
        }
        if (sample.world.instantiatedSystemCount() != target) {
            continue; // the galaxy ran out of furnished systems; say nothing
        }
        sample.run(30); // let the sky settle before the clock starts
        const auto start = std::chrono::steady_clock::now();
        sample.run(kFrames);
        const auto end = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(end - start).count() / static_cast<double>(kFrames);
        std::printf("  %7zu   %8.3f   %10.3f\n", target, ms, ms / static_cast<double>(target));
        // The anti-vacuity, and the only thing here that can fail: a run that
        // quietly dropped its bubbles would print a flat line and read as good
        // news.
        SOL_CHECK(sample.world.instantiatedSystemCount() == target);
    }
}

SOL_TEST(returning_to_a_retained_system_does_not_refill_its_sky)
{
    Galaxy g;
    const std::uint32_t here = g.world.currentSystemIndex();
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && elsewhere != here);

    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity hunter =
        g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.pilotAttackPlayer(hunter));

    SOL_REQUIRE(g.world.enterSystem(elsewhere));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);
    g.run(60);
    const game::SpaceWorld::BubbleReport away = g.report(1);
    SOL_REQUIRE(away.system == here);
    SOL_REQUIRE(away.ships > 0);

    SOL_REQUIRE(g.world.enterSystem(here));
    const game::SpaceWorld::BubbleReport home = g.report(0);
    SOL_CHECK(home.system == here);
    SOL_CHECK(home.player);
    // One player arrived and one sky was already there: the entity count is the
    // bubble's plus the player, and the hulls are the same hulls.
    SOL_CHECK(home.entities == away.entities + 1);
    SOL_CHECK(home.ships == away.ships);
}

// ⚑⚑ THE CLOCK SURVIVES A SAVE (v38). The bubbles have been written since v37,
// so the cooling ones are in the file whether or not their clocks are — and a
// file carrying five retained systems and no clocks loads a world that drops
// four of them on its first tick. Saving mid-retreat and finding the wounded
// raider gone is the phase's exit failing at a save point, which is exactly
// where a player would not think to look for it.
SOL_TEST(a_save_carries_the_retention_clock_and_the_player_stays_at_the_front)
{
    Galaxy g;
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu);
    const std::uint32_t retained = g.world.currentSystemIndex();

    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity hunter =
        g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.pilotAttackPlayer(hunter));
    SOL_REQUIRE(g.world.enterSystem(elsewhere));
    g.run(60);
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);
    const double held = g.report(1).holdSeconds;
    SOL_REQUIRE(held > 0.0 && held < game::kCoolingSeconds);

    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/saves";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/phase38_retention.sav";
    SOL_REQUIRE(g.world.saveTo(path.c_str(), "cooling"));

    // Run the window out, so the world being loaded INTO has let the bubble go.
    // Without this the load could leave the pre-existing one standing and read
    // as a pass, which is the shape a save test fails at most often.
    g.run(static_cast<int>(game::kCoolingSeconds * 60.0) + 2);
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 1);

    SOL_REQUIRE(g.world.loadFrom(path.c_str()));
    SOL_CHECK(g.world.instantiatedSystemCount() == 2);
    // The player is at the front however the file ordered them, and off the
    // clock - a loaded world must never have the player standing in a system
    // that is counting down.
    SOL_CHECK(g.report(0).player);
    SOL_CHECK(g.report(0).holdSeconds == 0.0);
    SOL_CHECK(g.world.instantiatedSystemAt(0) == elsewhere);
    SOL_CHECK(g.world.instantiatedSystemAt(1) == retained);
    SOL_CHECK(g.report(1).holdSeconds == held);
    SOL_CHECK(!g.report(1).player);
}

// ⚑⚑⚑⚑ COMING BACK TO A COOLING SYSTEM LEAVES ITS SHIPS TARGETABLE, AND THIS
// IS THE BUG THE FLIGHT FOUND. `leaveSystemFor` ended with
// `playerShips().clear()`, which reads as obvious housekeeping and was: it
// clears the ARRIVAL bubble's ship list, and while every arrival was a brand
// new bubble that list was empty, so the line was a no-op that had been
// correct for two stages by accident.
//
// The moment an arrival can be a bubble that already has ships in it, the same
// line throws away the display names of hulls that are still alive and still
// flying - and `spawnedShips` is what the targeting cycle, the hail and the
// death path all look a hull up in. The symptom this test asserts is the mild
// one: you back out of a fight, come straight back, see the raider on your
// screen, and cannot target it, cannot hail it and are given no name for it.
//
// ⚑⚑⚑⚑ THE SEVERE ONE IS THAT SUCH A SHIP CANNOT DIE. `handleShipDestroyed`
// walks `spawnedShips` for the victim and does its whole job - the wreck, the
// kill credit, the contest pressure AND `registry.destroy` - inside that loop,
// so a hull that is not in the list is never destroyed. It stays in the sky on
// a dead hull, is hit again, and runs the entire death path again on every
// subsequent hit, pushing wreck records and kill credits without bound. That is
// what took the flight down: one crash, `0x80000003`, on the build before this
// line was removed, and none since across several runs of the same churn.
//
// ⚑ It is the third form of the same mistake this phase keeps finding - a line
// that is correct only because something else is being thrown away.
SOL_TEST(returning_to_a_cooling_system_leaves_its_ships_targetable)
{
    Galaxy g;
    const std::uint32_t here = g.world.currentSystemIndex();
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && elsewhere != here);

    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity hostile =
        g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.pilotAttackPlayer(hostile));

    // What the player could target before they left.
    std::size_t before = 0;
    for (int i = 0; i < 60; ++i) {
        g.world.cycleContact(1);
        if (g.world.targetShipEntityIndex() != 0xffff'ffffu) {
            ++before;
        }
    }
    SOL_REQUIRE(before > 0); // anti-vacuity: there was something to target

    SOL_REQUIRE(g.world.enterSystem(elsewhere));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);
    g.run(120);
    SOL_REQUIRE(g.world.enterSystem(here));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 1);
    SOL_REQUIRE(g.report(0).ships > 0); // the ships ARE there...

    // ...and the player can still take hold of them.
    std::size_t after = 0;
    for (int i = 0; i < 60; ++i) {
        g.world.cycleContact(1);
        if (g.world.targetShipEntityIndex() != 0xffff'ffffu) {
            ++after;
        }
    }
    SOL_CHECK(after > 0);
}

// ---------------------------------------------------------------------------
// Stage D: audio, render and the probe.
// ---------------------------------------------------------------------------

// The player's own sky, drawn. `buildParticleInstances` is what `main.cpp`
// hands the renderer, so this is literally what is on screen: thruster plumes
// plus every combat spark in flight. With no input on the stick the thrusters
// contribute nothing, which is what makes a plain count meaningful here.
namespace {

[[nodiscard]] std::size_t drawnParticles(const game::SpaceWorld& world)
{
    std::vector<game::ParticleInstance> instances;
    world.buildParticleInstances(0.0f, instances);
    return instances.size();
}

} // namespace

// ⚑⚑⚑⚑ THE STAGE'S OWN EXIT: A FIGHT YOU HAVE LEFT IS NOT DRAWN IN THE SKY YOU
// ARE IN. The cooling bubble keeps shooting after the player jumps out — that
// is the feature, and it is what `a_ship_left_damaged_in_a_system_is_still_
// there_and_still_damaged` measures from the other end. Every one of those
// shots ran through `noteDamage`, which spawned a spark, and through the death
// path, which spawned a fireball; both went into ONE particle buffer, in
// coordinates belonging to a star the player is no longer near.
//
// ⚑⚑⚑ AND THE COORDINATES DO NOT SAVE IT, WHICH IS WHY THIS IS NOT A THEORETICAL
// TIDY-UP. Both systems lay their contents around a barycentre origin, so two
// ships in different systems sit within a couple of hundred kilometres of each
// other AS NUMBERS. That is the phase's own `pilotEngageEnemy` argument — the
// spec calls it the worst frame bug in the phase — arriving one output path
// over: a fireball from next door does not land somewhere absurd and get
// culled, it lands plausibly close and is drawn.
//
// ⚑⚑ THE POSITIVE CONTROL IS THE FIRST HALF OF THE TEST AND IT IS NOT
// DECORATION. A guard that dropped every burst in the game would satisfy the
// second half perfectly.
SOL_TEST(a_fight_in_the_system_you_left_is_not_drawn_in_the_one_you_are_in)
{
    Galaxy g;
    const std::uint32_t here = g.world.currentSystemIndex();
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && elsewhere != here);

    const sol::assets::ShipDef* def = g.content.defs().findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    const sol::ecs::Entity mark = g.world.spawnPilotFromDef(*def, g.content.defs(), game::PilotRole::Fighter);
    SOL_REQUIRE(g.world.shipHullFraction(mark) == 1.0);

    // The player's own fight, in the player's own system. Sparks are what a hit
    // looks like, so this is the control: the mechanism draws, when it should.
    sol::sim::FlightInput trigger;
    trigger.trigger = true;
    std::size_t seenAtHome = 0;
    for (int i = 0; i < 180; ++i) {
        g.world.setShipInput(trigger);
        g.world.tick(1.0 / 60.0);
        seenAtHome = std::max(seenAtHome, drawnParticles(g.world));
    }
    g.world.setShipInput(sol::sim::FlightInput{});
    SOL_REQUIRE(seenAtHome > 0);                  // the sky the player is in is drawn
    SOL_REQUIRE(g.world.outOfFrameBursts() == 0); // and nothing was refused to get it
    SOL_REQUIRE(g.world.shipHullFraction(mark) < 1.0);

    // Out, leaving a live fight behind — the retention entry condition.
    SOL_REQUIRE(g.world.enterSystem(elsewhere));
    SOL_REQUIRE(g.world.instantiatedSystemCount() == 2);
    SOL_REQUIRE(g.world.instantiatedSystemAt(1) == here);
    // The jump clears what was in flight (`leaveSystemFor`), so the sky the
    // player arrives in starts empty and anything counted below is new.
    SOL_REQUIRE(drawnParticles(g.world) == 0);

    // A minute next door, sampled every frame rather than at the end: a spark
    // lives 0.35 s and a fireball 1.5 s, so a check taken only once could miss
    // every one of them and read as a pass.
    std::size_t worstFrame = 0;
    for (int i = 0; i < 110 * 60; ++i) {
        g.world.tick(1.0 / 60.0);
        worstFrame = std::max(worstFrame, drawnParticles(g.world));
    }
    // ⚑ ANTI-VACUITY, AND IT IS THE ASSERTION THAT MAKES THE ONE BELOW MEAN
    // ANYTHING: the bubble behind really was producing combat events for that
    // whole minute. Without this the test would pass just as well against a
    // world that had quietly released the system on the way out.
    SOL_CHECK(g.world.outOfFrameBursts() > 0);
    SOL_CHECK(worstFrame == 0);
}

// ⚑⚑⚑⚑ WHAT THIS SUITE DOES *NOT* COVER, RECORDED RATHER THAN LEFT TO BE
// REDISCOVERED. Eleven mutations were run against stage D and three survived,
// and neither reason is "the guard is missing".
//
// ⚑⚑⚑ THE FIGHT A COOLING BUBBLE CARRIES ON IS INERTIAL: IT LANDS HITS AND
// FINISHES NOBODY. Charging `noteDamage`'s spark to the player's system is
// caught by the test above; charging `handleShipDestroyed`'s fireball or
// `damageMounts`' is not, and neither is deleting `spawnExplosion`'s frame
// check outright. All three survive for ONE reason, and it is stage B's
// recorded LOD statement rather than a hole in the guards: nobody in a system
// the player has left re-targets or presses an attack, so the shooting that
// continues is the shooting already in flight. Over 110 sim-seconds of a
// retained fight in the shipped galaxy, hits land continuously and NOT ONE
// hull or mount is destroyed. Shooting the mark down to 0.30 before leaving
// does not fix it either - at that hull it breaks off, and then there is no
// fight left behind at all.
//
// ⚑⚑ AND EVERY AUDIO CALL SITE IS UNREACHABLE FROM A UNIT TEST, WHICH IS WHY
// `GameAudio` GREW A COUNTER AT ALL. `m_audio` is null in every test in this
// repository - the ear needs a device and a cooked bank - so a mutation that
// hands `playAt` the wrong system changes nothing any test can see. The sink's
// own refusal is pinned directly instead, one test down. The call sites are
// held by the same `bubble.system` the particle sites use, on the same lines.

// ⚑⚑⚑⚑ AND THE REASON THE CHECK IS AT THE SPAWN RATHER THAN AT THE DRAW.
// `CombatEffects` owns a random stream and `burst` takes six numbers out of it
// per particle — 84 for one impact spark, some 900 for a fireball. One stream,
// for the whole world. So a fight the player cannot see would change what the
// player's own sparks look like, which is precisely the determinism shape the
// phase's risk list names for `m_chunkRng` and `m_noticeRng`.
//
// ⚑⚑⚑ THE DIFFERENCE IS THAT THOSE TWO ARE NOT LIVE AND THIS ONE IS. Stage B
// recorded both as structure rather than a fix: `m_chunkRng` is drawn only when
// something cuts rock, and cutting is player-gated. `CombatEffects::m_rng` is
// drawn by every hit in the game, and stage C made hits happen where the player
// is not. It became a live shared stream the moment the cooling bubble shipped,
// in a class the spec's risk list never mentions.
//
// Filtering at draw time would have left it exactly as broken, because by then
// the numbers are already spent. This pins the ordering: refused BEFORE
// `burst`, so a foreign event costs the stream nothing.
SOL_TEST(a_burst_from_another_frame_never_reaches_the_particle_stream)
{
    game::CombatEffects clean;
    game::CombatEffects disturbed;
    clean.setFrame(3);
    disturbed.setFrame(3);

    // The same fight in the same frame, except that the second buffer is also
    // being offered everything a cooling bubble would offer it.
    disturbed.spawnImpact(7, sol::core::DVec3{100.0, 0.0, 0.0}, false);
    disturbed.spawnExplosion(7, sol::core::DVec3{0.0, 100.0, 0.0}, 2.0f);
    clean.spawnImpact(3, sol::core::DVec3{0.0, 0.0, 0.0}, true);
    disturbed.spawnImpact(3, sol::core::DVec3{0.0, 0.0, 0.0}, true);

    SOL_CHECK(disturbed.outOfFrameBursts() == 2);
    SOL_CHECK(clean.outOfFrameBursts() == 0);

    std::vector<game::ParticleInstance> left;
    std::vector<game::ParticleInstance> right;
    clean.appendInstances(0.0f, left);
    disturbed.appendInstances(0.0f, right);
    SOL_REQUIRE(!left.empty()); // anti-vacuity: there are particles to disagree about
    SOL_REQUIRE(left.size() == right.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        // Every field, not just the count: the stream decides direction, speed,
        // colour, size and lifetime, and a shifted stream would keep the count
        // exactly while changing all five.
        SOL_CHECK(left[i].position.x == right[i].position.x);
        SOL_CHECK(left[i].position.y == right[i].position.y);
        SOL_CHECK(left[i].position.z == right[i].position.z);
        SOL_CHECK(left[i].size == right[i].size);
        SOL_CHECK(left[i].color.x == right[i].color.x);
        SOL_CHECK(left[i].color.w == right[i].color.w);
    }
}

// ⚑⚑⚑ THE EAR HAS A FRAME NOW, AND THIS IS THE ONLY PLACE THE AUDIO HALF CAN BE
// ASSERTED. `GameAudio` needs a device and a cooked bank before it will play
// anything, so a test cannot watch a voice start; what it CAN watch is the
// refusal, which is why the counter exists at all. The check sits above
// `cueFor` deliberately — a cue from another system must not even look its
// sample up, because the cost being avoided is the instance cap, not the mix.
SOL_TEST(the_ear_refuses_a_positional_cue_from_a_system_it_is_not_in)
{
    game::GameAudio audio;
    SOL_REQUIRE(audio.listenerSystem() == 0);
    audio.setListenerSystem(4);
    SOL_CHECK(audio.listenerSystem() == 4);

    audio.playAt(sol::audio::kNoSound, sol::core::DVec3{}, 9);
    audio.playAt(sol::audio::kNoSound, sol::core::DVec3{}, 0);
    SOL_CHECK(audio.outOfFrameCues() == 2);

    // And a cue from the ear's own system is NOT refused — it goes on to the
    // cue table and dies there for want of a device, which is a different
    // thing and has to be, or the guard above would be indistinguishable from
    // an audio system that never plays anything.
    audio.playAt(sol::audio::kNoSound, sol::core::DVec3{}, 4);
    SOL_CHECK(audio.outOfFrameCues() == 2);
}

// ⚑⚑⚑⚑ THE EAR CROSSES WITH THE PLAYER, NOT A TICK BEHIND THEM, AND THAT IS THE
// WHOLE REASON THE LISTENER'S FRAME IS SET APART FROM THE LISTENER'S POSE. The
// pose comes from the render loop, which runs after `world.tick` returns. A
// jump happens INSIDE `tick`. Had the frame been taken from the same place as
// the pose, then for one tick after every crossing the ear would have been in
// the system the player just left — playing that system's shots and refusing
// the arrival's. Both of the stage's errors at once, at exactly the moment the
// phase is about.
// ⚑⚑⚑⚑ THE BUG THIS STAGE'S FLIGHT FOUND, AND NO TEST IN THIS REPOSITORY
// COULD HAVE: A NEW GAME THREW THE AUDIO DEVICE AWAY. `resetForNewGame` is
// `*this = SpaceWorld{}` followed by `spawn`, which is right about everything
// belonging to a RUN and wrong about the two pointers borrowed from `main`.
// `m_defs` is put back by `applyDefs`; the device was not put back by anything.
//
// ⚑⚑⚑ AND THE GAME BOOTS TO A MENU, SO THIS IS NOT AN EDGE CASE - IT IS EVERY
// SESSION. The first thing anyone does is start a run. From that moment
// `m_audio` was null at every cue site in the game, all of which are written
// `if (m_audio != nullptr)` so that a machine with no sound card still plays.
// The result is total silence for everything except the UI clicks, which go
// through `GameContent`'s own copy of the pointer.
//
// ⚑⚑ THE SEAM IS THE LISTENER'S FRAME, WHICH IS WHY THIS TEST CAN EXIST AT
// ALL. Before stage D there was nothing the world wrote into `GameAudio` that
// a test could read back without a device, so "does the world still hold the
// device" was unaskable from outside. It is askable now: scribble a system
// number no galaxy has onto the ear, reset, and see whether anything wrote
// over it.
SOL_TEST(a_new_game_keeps_the_audio_device_it_was_handed)
{
    Galaxy g;
    game::GameAudio audio;
    g.world.setAudio(&audio);
    SOL_REQUIRE(audio.listenerSystem() == g.world.currentSystemIndex());

    constexpr std::uint32_t kScribble = 0xBADF00Du; // no galaxy has this system
    audio.setListenerSystem(kScribble);
    g.world.resetForNewGame(game::kDefaultUniverseSeed);

    SOL_CHECK(audio.listenerSystem() != kScribble); // something wrote to it
    SOL_CHECK(audio.listenerSystem() == g.world.currentSystemIndex());
    g.world.setAudio(nullptr);
}

SOL_TEST(the_ear_changes_system_in_the_same_statement_the_player_does)
{
    Galaxy g;
    const std::uint32_t elsewhere = g.furnishedSystem(0);
    const std::uint32_t third = g.furnishedSystem(1);
    SOL_REQUIRE(elsewhere != 0xffff'ffffu && third != 0xffff'ffffu && elsewhere != third);
    SOL_REQUIRE(g.world.enterSystem(elsewhere));

    // ⚑ ATTACHED AFTER THE GALAXY EXISTS AND AFTER A CROSSING, BECAUSE THAT IS
    // THE REAL ORDER: `content.initialize` runs the generator, and `main.cpp`
    // hands the device over afterwards. So the ear cannot wait for the next
    // jump to learn where it is - it has to be told as it is attached, or every
    // positional cue until then is refused as foreign.
    game::GameAudio audio;
    SOL_REQUIRE(audio.listenerSystem() != elsewhere); // it starts in 0 and is not there
    g.world.setAudio(&audio);
    SOL_CHECK(audio.listenerSystem() == elsewhere);

    // And thereafter it crosses in the same statement the player does.
    SOL_REQUIRE(g.world.enterSystem(third));
    SOL_CHECK(audio.listenerSystem() == third);
    SOL_CHECK(audio.listenerSystem() == g.world.currentSystemIndex());

    g.world.setAudio(nullptr);
}
