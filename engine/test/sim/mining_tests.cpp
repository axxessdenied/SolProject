#include <sol/sim/mining.hpp>
#include <sol/sim/universe.hpp>

#include <sol/core/serialize.hpp>
#include <sol/test/test.hpp>

#include <cstdint>
#include <vector>

using sol::sim::AsteroidFieldSpec;
using sol::sim::Galaxy;
using sol::sim::GalaxyParams;
using sol::sim::generateGalaxy;
using sol::sim::MiningParams;
using sol::sim::MiningSim;
using sol::sim::OreEntry;
using sol::sim::Region;
using sol::sim::RockSpec;
using sol::sim::SignalCargo;
using sol::sim::SignalLoot;
using sol::sim::SystemSpec;
using sol::sim::WreckRecord;

namespace {

// Three systems in a line, one of each region tier, same fixture shape the
// survey tests use: 0 core, 1 frontier, 2 fringe.
Galaxy lineGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 4242;
    for (std::uint32_t i = 0; i < 3; ++i) {
        SystemSpec system;
        system.name = std::string("S") + static_cast<char>('0' + static_cast<char>(i));
        system.seed = 1000 + i;
        system.region = static_cast<Region>(i);
        system.planets.push_back({.name = "I", .position = {1.0e10, 0.0, 0.0}, .radius = 1.0e6});
        system.planets.push_back({.name = "II", .position = {2.0e10, 0.0, 0.0}, .radius = 1.0e6});
        system.primaryPlanet = 1;
        galaxy.systems.push_back(std::move(system));
    }
    galaxy.links = {{0, 1}, {1, 2}};
    return galaxy;
}

// Two ores: commodity 0 everywhere, commodity 1 only in the fringe — so the
// region gradient is testable without leaning on a seed.
MiningParams lineParams()
{
    MiningParams params;
    params.fieldCount[0][0] = 1; // one field per system in every tier
    params.fieldCount[0][1] = 1;
    params.fieldCount[1][0] = 1;
    params.fieldCount[1][1] = 1;
    params.fieldCount[2][0] = 1;
    params.fieldCount[2][1] = 1;
    for (std::size_t tier = 0; tier < 3; ++tier) {
        params.rockCount[tier][0] = 6;
        params.rockCount[tier][1] = 6;
    }
    params.ores = {OreEntry{.commodity = 0, .weight = {1.0f, 1.0f, 0.0f}},
                   OreEntry{.commodity = 1, .weight = {0.0f, 0.0f, 1.0f}}};
    return params;
}

MiningSim lineMining(const Galaxy& galaxy)
{
    MiningSim mining;
    mining.initialize(galaxy, lineParams(), 3, 99);
    return mining;
}

} // namespace

SOL_TEST(mining_rocks_regrow_and_drop_their_record)
{
    const Galaxy galaxy = lineGalaxy();
    MiningSim mining = lineMining(galaxy);

    SOL_CHECK(mining.mineRock(2, 0, 1, 40.0f, 10.0f) == 10.0f);
    SOL_CHECK(mining.depletionRecordCount() == 1);

    // Partway back: still a debt, still a record.
    mining.tick(100.0); // 0.02/s * 100 = 2 units
    SOL_CHECK(std::abs(mining.unitsTaken(2, 0, 1) - 8.0f) < 1.0e-3f);
    SOL_CHECK(mining.depletionRecordCount() == 1);

    // All the way back: the rock is whole and costs nothing to remember,
    // which is what keeps a hundred-hour save from accumulating them.
    mining.tick(1'000.0);
    SOL_CHECK(mining.unitsTaken(2, 0, 1) == 0.0f);
    SOL_CHECK(mining.depletionRecordCount() == 0);
}

SOL_TEST(mining_system_draw_spreads_across_rocks_and_runs_out)
{
    const Galaxy galaxy = lineGalaxy();
    MiningSim mining = lineMining(galaxy);

    const float before = mining.systemStock(galaxy, 2, 1);
    SOL_REQUIRE(before > 0.0f);
    SOL_CHECK(mining.systemStock(galaxy, 2, 0) == 0.0f); // wrong ore for the fringe

    // Repeated small draws have to work the field evenly rather than eating
    // one rock: regrowth is per record, so an even field is what lets a
    // system sustain an outpost at all.
    // Spread over many rocks, each bite is a sum of small proportional
    // takes, so it lands within rounding of what was asked rather than on it.
    float drawn = 0.0f;
    for (int i = 0; i < 40; ++i) {
        drawn += mining.drawFromSystem(galaxy, 2, 1, 1.0f);
    }
    SOL_CHECK(std::abs(drawn - 40.0f) < 1.0e-2f);
    SOL_CHECK(std::abs(mining.systemStock(galaxy, 2, 1) - (before - drawn)) < 1.0e-1f);
    // Every rock holding this ore now carries a debt — that even spread is
    // what regrowth needs to hold an equilibrium against a steady draw.
    SOL_CHECK(mining.depletionRecordCount() >= 4); // spread, not one rock drained

    // A commodity that is not in this system's rock yields nothing at all —
    // an outpost with no rock under it produces nothing, which is the point.
    SOL_CHECK(mining.drawFromSystem(galaxy, 2, 0, 5.0f) == 0.0f);
    SOL_CHECK(mining.drawFromSystem(galaxy, 0, 1, 5.0f) == 0.0f);

    // Mined out: the draw reports short rather than inventing units.
    const float left = mining.systemStock(galaxy, 2, 1);
    const float taken = mining.drawFromSystem(galaxy, 2, 1, left + 500.0f);
    SOL_CHECK(std::abs(taken - left) < 1.0e-2f);
    SOL_CHECK(mining.systemStock(galaxy, 2, 1) < 1.0e-2f);
}

SOL_TEST(mining_repeated_draws_accumulate_into_one_record_per_rock)
{
    // Phase 8n replaced the one-sorted-insert-per-rock path with a single
    // merge, because an outpost's draw touches every matching rock in its
    // system and that cost O(rocks * records) of memmove every economy step.
    // The merge has to land on the same table: one record per rock, debts
    // summed, and no zero-value record for a rock that gave up nothing.
    const Galaxy galaxy = lineGalaxy();
    MiningSim mining = lineMining(galaxy);

    (void)mining.drawFromSystem(galaxy, 2, 1, 3.0f);
    const std::size_t afterFirst = mining.depletionRecordCount();
    SOL_REQUIRE(afterFirst >= 4); // the draw did spread

    // Twenty more draws over the same rocks must deepen the existing records
    // rather than add new ones.
    float drawn = 0.0f;
    for (int i = 0; i < 20; ++i) {
        drawn += mining.drawFromSystem(galaxy, 2, 1, 1.0f);
    }
    SOL_CHECK(mining.depletionRecordCount() == afterFirst);
    SOL_CHECK(std::abs(drawn - 20.0f) < 1.0e-2f);

    // Debts accumulated rather than being overwritten by the last merge.
    float totalTaken = 0.0f;
    for (std::uint32_t field = 0; field < mining.fieldCount(2); ++field) {
        for (std::uint32_t rock = 0; rock < 64; ++rock) {
            totalTaken += mining.unitsTaken(2, field, rock);
        }
    }
    SOL_CHECK(std::abs(totalTaken - 23.0f) < 1.0e-1f);

    // A commodity with no rock behind it leaves the table exactly as it was —
    // no empty records for rocks that were never cut.
    (void)mining.drawFromSystem(galaxy, 2, 0, 5.0f);
    SOL_CHECK(mining.depletionRecordCount() == afterFirst);
}

SOL_TEST(mining_fields_and_rocks_are_deterministic_per_seed)
{
    const Galaxy galaxy = lineGalaxy();
    const MiningSim mining = lineMining(galaxy);

    std::vector<AsteroidFieldSpec> first;
    std::vector<AsteroidFieldSpec> second;
    mining.fieldsFor(galaxy, 2, first);
    mining.fieldsFor(galaxy, 2, second);
    SOL_REQUIRE(first.size() == 1);
    SOL_CHECK(mining.fieldCount(2) == 1);
    SOL_CHECK(first[0].seed == second[0].seed);
    SOL_CHECK(first[0].center.x == second[0].center.x);
    SOL_CHECK(first[0].rockCount == 6);

    // A field sits in the playfield band around the primary planet, like
    // every other visitable thing in a system.
    const MiningParams params = lineParams();
    const double distance =
        sol::core::length(first[0].center - galaxy.systems[2].planets[1].position);
    SOL_CHECK(distance >= params.fieldMinDistance * 0.999);
    SOL_CHECK(distance <= params.fieldMaxDistance * 1.001);

    // Rocks regenerate identically, scatter inside the field radius, and
    // carry positive yield.
    std::vector<RockSpec> rocks;
    std::vector<RockSpec> rocksAgain;
    mining.rocksFor(galaxy, 2, 0, rocks);
    mining.rocksFor(galaxy, 2, 0, rocksAgain);
    SOL_REQUIRE(rocks.size() == 6);
    SOL_REQUIRE(rocksAgain.size() == 6);
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        SOL_CHECK(rocks[i].seed == rocksAgain[i].seed);
        SOL_CHECK(rocks[i].position.x == rocksAgain[i].position.x);
        SOL_CHECK(rocks[i].yieldUnits == rocksAgain[i].yieldUnits);
        SOL_CHECK(rocks[i].yieldUnits > 0.0f);
        SOL_CHECK(rocks[i].radius >= params.rockRadiusMin * 0.999);
        SOL_CHECK(rocks[i].radius <= params.rockRadiusMax * 1.001);
        SOL_CHECK(sol::core::length(rocks[i].position - first[0].center) <= first[0].radius * 1.001);
    }

    // A second MiningSim over the same galaxy sees the same rocks.
    const MiningSim other = lineMining(galaxy);
    std::vector<RockSpec> otherRocks;
    other.rocksFor(galaxy, 2, 0, otherRocks);
    SOL_REQUIRE(otherRocks.size() == rocks.size());
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        SOL_CHECK(otherRocks[i].seed == rocks[i].seed);
    }

    // Out-of-range systems and fields yield nothing rather than stale output.
    std::vector<RockSpec> none;
    mining.rocksFor(galaxy, 2, 7, none);
    SOL_CHECK(none.empty());
    mining.fieldsFor(galaxy, 99, first);
    SOL_CHECK(first.empty());
}

SOL_TEST(mining_ore_and_yield_follow_the_region_gradient)
{
    const Galaxy galaxy = lineGalaxy();
    const MiningSim mining = lineMining(galaxy);
    const MiningParams params = lineParams();

    std::vector<RockSpec> core;
    std::vector<RockSpec> fringe;
    mining.rocksFor(galaxy, 0, 0, core);
    mining.rocksFor(galaxy, 2, 0, fringe);
    SOL_REQUIRE(!core.empty());
    SOL_REQUIRE(!fringe.empty());

    // The weight table decides composition outright: commodity 1 exists only
    // in the fringe.
    for (const RockSpec& rock : core) {
        SOL_CHECK(rock.commodity == 0);
    }
    for (const RockSpec& rock : fringe) {
        SOL_CHECK(rock.commodity == 1);
    }

    // Fringe rock is richer per unit of size (the multiplier is 1.8 vs 1.0),
    // so compare yield against what the size alone would give.
    double coreRatio = 0.0;
    for (const RockSpec& rock : core) {
        const double sizeFraction = (rock.radius - params.rockRadiusMin)
                                    / (params.rockRadiusMax - params.rockRadiusMin);
        const double base = params.yieldMin + (params.yieldMax - params.yieldMin) * sizeFraction;
        coreRatio += rock.yieldUnits / base;
    }
    coreRatio /= static_cast<double>(core.size());
    double fringeRatio = 0.0;
    for (const RockSpec& rock : fringe) {
        const double sizeFraction = (rock.radius - params.rockRadiusMin)
                                    / (params.rockRadiusMax - params.rockRadiusMin);
        const double base = params.yieldMin + (params.yieldMax - params.yieldMin) * sizeFraction;
        fringeRatio += rock.yieldUnits / base;
    }
    fringeRatio /= static_cast<double>(fringe.size());
    SOL_CHECK(fringeRatio > coreRatio * 1.3);
}

SOL_TEST(mining_depletion_is_sparse_and_clamps_at_the_rock)
{
    const Galaxy galaxy = lineGalaxy();
    MiningSim mining = lineMining(galaxy);
    SOL_CHECK(mining.depletionRecordCount() == 0);

    // An untouched rock costs nothing to remember.
    SOL_CHECK(mining.unitsTaken(2, 0, 3) == 0.0f);
    SOL_CHECK(mining.unitsLeft(2, 0, 3, 40.0f) == 40.0f);

    SOL_CHECK(mining.mineRock(2, 0, 3, 40.0f, 15.0f) == 15.0f);
    SOL_CHECK(mining.depletionRecordCount() == 1);
    SOL_CHECK(mining.unitsTaken(2, 0, 3) == 15.0f);
    SOL_CHECK(mining.unitsLeft(2, 0, 3, 40.0f) == 25.0f);

    // Cutting past the end of the rock returns only what was left, and the
    // rock then yields nothing at all.
    SOL_CHECK(mining.mineRock(2, 0, 3, 40.0f, 100.0f) == 25.0f);
    SOL_CHECK(mining.unitsLeft(2, 0, 3, 40.0f) == 0.0f);
    SOL_CHECK(mining.mineRock(2, 0, 3, 40.0f, 5.0f) == 0.0f);
    SOL_CHECK(mining.depletionRecordCount() == 1);

    // A neighbouring rock in the same field is untouched: depletion is per
    // rock, not per field.
    SOL_CHECK(mining.unitsTaken(2, 0, 4) == 0.0f);

    // Records stay sorted however they arrive (the lookup is a binary search).
    SOL_CHECK(mining.mineRock(2, 0, 1, 10.0f, 4.0f) == 4.0f);
    SOL_CHECK(mining.mineRock(1, 0, 5, 10.0f, 2.0f) == 2.0f);
    SOL_CHECK(mining.depletionRecordCount() == 3);
    SOL_CHECK(mining.unitsTaken(2, 0, 1) == 4.0f);
    SOL_CHECK(mining.unitsTaken(1, 0, 5) == 2.0f);
    SOL_CHECK(mining.unitsTaken(2, 0, 3) == 40.0f);

    // Bad input never writes a record.
    SOL_CHECK(mining.mineRock(99, 0, 0, 10.0f, 1.0f) == 0.0f);
    SOL_CHECK(mining.mineRock(2, 9, 0, 10.0f, 1.0f) == 0.0f);
    SOL_CHECK(mining.mineRock(2, 0, 0, 10.0f, -1.0f) == 0.0f);
    SOL_CHECK(mining.depletionRecordCount() == 3);
}

SOL_TEST(mining_wrecks_hold_validated_loot_and_decay)
{
    const Galaxy galaxy = lineGalaxy();
    MiningSim mining = lineMining(galaxy);

    const std::uint32_t id = mining.addWreck(1, {5.0, 0.0, 0.0}, "sol.interceptor", "Raider", 7);
    SOL_REQUIRE(id != 0);
    SOL_REQUIRE(mining.wreck(id) != nullptr);
    SOL_CHECK(mining.wreck(id)->system == 1);
    SOL_CHECK(!mining.wreck(id)->contentsSet);

    // Loot answers to the same rule signal loot does: unknown commodities are
    // refused, and a wreck is composed exactly once.
    SignalLoot bad;
    bad.cargo.push_back(SignalCargo{.commodity = 9, .units = 4.0f});
    SOL_CHECK(!mining.setWreckContents(id, bad));
    SOL_CHECK(!mining.wreck(id)->contentsSet);

    SignalLoot good;
    good.cargo.push_back(SignalCargo{.commodity = 1, .units = 6.0f});
    good.credits = 120.0;
    good.moduleId = "sol.shield_mk1";
    SOL_CHECK(mining.setWreckContents(id, good));
    SOL_CHECK(mining.wreck(id)->contentsSet);
    SOL_CHECK(mining.wreck(id)->contents.credits == 120.0);
    // The default is written at death and a script may replace it — but only
    // until the beam has been into the hull.
    SignalLoot replacement = good;
    replacement.credits = 500.0;
    SOL_CHECK(mining.setWreckContents(id, replacement));
    SOL_CHECK(mining.wreck(id)->contents.credits == 500.0);

    // Cutting draws cargo out a bite at a time, and freezes the contents.
    std::uint32_t commodity = 99;
    SOL_CHECK(mining.cutWreckCargo(id, 2.0f, &commodity) == 2.0f);
    SOL_CHECK(commodity == 1);
    SOL_CHECK(mining.wreck(id)->opened);
    SOL_CHECK(!mining.setWreckContents(id, replacement));
    SOL_CHECK(mining.cutWreckCargo(id, 100.0f, &commodity) == 4.0f); // the rest
    SOL_CHECK(mining.wreck(id)->contents.cargo.empty());
    // An emptied hull still holds what does not float; the caller takes that
    // and removes it.
    SOL_CHECK(mining.cutWreckCargo(id, 100.0f, &commodity) == 0.0f);
    SOL_CHECK(mining.wreck(id)->contents.credits == 500.0);
    SOL_CHECK(mining.cutWreckCargo(4242, 5.0f, &commodity) == 0.0f); // no such wreck

    std::vector<std::uint32_t> here;
    mining.wrecksIn(1, here);
    SOL_CHECK(here.size() == 1);
    mining.wrecksIn(0, here);
    SOL_CHECK(here.empty());

    // Cutting it open takes it out of the world.
    SOL_CHECK(mining.removeWreck(id));
    SOL_CHECK(mining.wreck(id) == nullptr);
    SOL_CHECK(!mining.removeWreck(id));

    // Wrecks age out on the coarse clock so a long save is not a junkyard.
    const std::uint32_t second = mining.addWreck(1, {}, "sol.shuttle", "Hauler", 3);
    SOL_REQUIRE(second != 0);
    mining.tick(mining.params().wreckDecaySeconds * 0.5);
    SOL_CHECK(mining.wreck(second) != nullptr);
    mining.tick(mining.params().wreckDecaySeconds * 0.6);
    SOL_CHECK(mining.wreck(second) == nullptr);

    // Ids never collide with a decayed wreck's.
    const std::uint32_t third = mining.addWreck(1, {}, "sol.shuttle", "Hauler", 3);
    SOL_CHECK(third != second);
}

SOL_TEST(mining_wreck_store_evicts_the_oldest)
{
    const Galaxy galaxy = lineGalaxy();
    MiningParams params = lineParams();
    params.maxWrecks = 3;
    MiningSim mining;
    mining.initialize(galaxy, params, 3, 99);

    const std::uint32_t first = mining.addWreck(1, {}, "sol.shuttle", "A", 1);
    mining.addWreck(1, {}, "sol.shuttle", "B", 2);
    mining.addWreck(1, {}, "sol.shuttle", "C", 3);
    SOL_CHECK(mining.wrecks().size() == 3);
    SOL_CHECK(mining.wreck(first) != nullptr);

    mining.addWreck(1, {}, "sol.shuttle", "D", 4);
    SOL_CHECK(mining.wrecks().size() == 3);
    SOL_CHECK(mining.wreck(first) == nullptr); // oldest evicted, not newest
    SOL_CHECK(mining.wrecks().back().name == "D");
}

SOL_TEST(mining_refine_jobs_finish_on_the_coarse_clock)
{
    const Galaxy galaxy = lineGalaxy();
    MiningSim mining = lineMining(galaxy);
    const MiningParams params = lineParams();

    const float units = 20.0f;
    SOL_CHECK(mining.refineFee(units) == units * params.refineFeePerUnit);
    const double duration = mining.refineDuration(units);
    SOL_CHECK(duration
              == params.refineSecondsBase + params.refineSecondsPerUnit * units);

    SOL_REQUIRE(mining.startRefineJob(4, 1, units, 2));
    SOL_CHECK(mining.refineJobs().size() == 1);
    SOL_CHECK(mining.readyAt(4, 2) == 0.0f); // still running
    SOL_CHECK(mining.soonestAt(4) == duration);
    SOL_CHECK(mining.soonestAt(5) < 0.0);

    mining.tick(duration * 0.5);
    SOL_CHECK(mining.readyAt(4, 2) == 0.0f);
    SOL_CHECK(mining.collectAt(4, 2, 100.0f) == 0.0f);

    mining.tick(duration);
    const float output = mining.refineOutput(units);
    SOL_CHECK(mining.readyAt(4, 2) == output);
    SOL_CHECK(mining.soonestAt(4) < 0.0); // nothing pending, only waiting

    // A hold with room for half leaves the rest waiting at the station.
    const float half = output * 0.5f;
    SOL_CHECK(mining.collectAt(4, 2, half) == half);
    SOL_CHECK(mining.refineJobs().size() == 1);
    SOL_CHECK(mining.readyAt(4, 2) == output - half);
    SOL_CHECK(mining.collectAt(4, 2, 100.0f) == output - half);
    SOL_CHECK(mining.refineJobs().empty());

    // Output waits at the market it was ordered at, not wherever the player
    // happens to dock.
    SOL_REQUIRE(mining.startRefineJob(4, 1, units, 2));
    mining.tick(duration * 2.0);
    SOL_CHECK(mining.collectAt(7, 2, 100.0f) == 0.0f);
    SOL_CHECK(mining.collectAt(4, 0, 100.0f) == 0.0f); // wrong commodity
    SOL_CHECK(mining.collectAt(4, 2, 100.0f) == output);

    // Bad orders are refused outright, and the queue has a ceiling.
    SOL_CHECK(!mining.startRefineJob(4, 1, 0.0f, 2));
    SOL_CHECK(!mining.startRefineJob(4, 9, units, 2));
    SOL_CHECK(!mining.startRefineJob(4, 1, units, 9));
    for (std::uint32_t i = 0; i < params.maxRefineJobs; ++i) {
        SOL_CHECK(mining.startRefineJob(4, 1, units, 2));
    }
    SOL_CHECK(!mining.startRefineJob(4, 1, units, 2));
}

SOL_TEST(mining_save_load_restores_depletion_wrecks_and_jobs)
{
    const Galaxy galaxy = lineGalaxy();
    MiningSim mining = lineMining(galaxy);

    SOL_CHECK(mining.mineRock(2, 0, 3, 40.0f, 15.0f) == 15.0f);
    SOL_CHECK(mining.mineRock(1, 0, 0, 30.0f, 30.0f) == 30.0f);
    const std::uint32_t id = mining.addWreck(1, {5.0, -2.0, 9.0}, "sol.interceptor", "Raider", 7);
    SignalLoot loot;
    loot.cargo.push_back(SignalCargo{.commodity = 1, .units = 6.0f});
    loot.credits = 120.0;
    loot.moduleId = "sol.shield_mk1";
    SOL_REQUIRE(mining.setWreckContents(id, loot));
    SOL_REQUIRE(mining.startRefineJob(4, 1, 20.0f, 2));
    mining.tick(30.0);

    sol::core::BinaryWriter writer;
    mining.save(writer);

    MiningSim restored = lineMining(galaxy);
    sol::core::BinaryReader reader(writer.data());
    SOL_REQUIRE(restored.load(reader));

    // Against the live sim rather than the amounts originally cut: the tick
    // above regrew some of it (Phase 8g), and what this test is about is that
    // a reload lands exactly where the save was taken.
    SOL_CHECK(restored.unitsTaken(2, 0, 3) == mining.unitsTaken(2, 0, 3));
    SOL_CHECK(restored.unitsTaken(1, 0, 0) == mining.unitsTaken(1, 0, 0));
    SOL_CHECK(restored.unitsTaken(2, 0, 3) < 15.0f); // regrowth actually ran
    SOL_CHECK(restored.depletionRecordCount() == mining.depletionRecordCount());
    SOL_REQUIRE(restored.wreck(id) != nullptr);
    SOL_CHECK(restored.wreck(id)->position.z == 9.0);
    SOL_CHECK(restored.wreck(id)->defId == "sol.interceptor");
    SOL_CHECK(restored.wreck(id)->name == "Raider");
    SOL_CHECK(restored.wreck(id)->contentsSet);
    SOL_CHECK(restored.wreck(id)->contents.credits == 120.0);
    SOL_REQUIRE(restored.wreck(id)->contents.cargo.size() == 1);
    SOL_CHECK(restored.wreck(id)->contents.cargo[0].units == 6.0f);
    SOL_CHECK(restored.wreck(id)->contents.moduleId == "sol.shield_mk1");
    SOL_REQUIRE(restored.refineJobs().size() == 1);
    SOL_CHECK(restored.refineJobs()[0].secondsRemaining
              == mining.refineJobs()[0].secondsRemaining);
    SOL_CHECK(restored.refineJobs()[0].outputUnits == mining.refineJobs()[0].outputUnits);

    // A new wreck after a load never reuses a live id.
    const std::uint32_t next = restored.addWreck(1, {}, "sol.shuttle", "Hauler", 3);
    SOL_CHECK(next != id);

    // Both sides then run tick-for-tick: the restored sim is the same sim.
    mining.tick(10.0);
    restored.tick(10.0);
    SOL_CHECK(restored.refineJobs()[0].secondsRemaining
              == mining.refineJobs()[0].secondsRemaining);

    // A save from a different galaxy is rejected, not half-applied.
    GalaxyParams biggerParams;
    biggerParams.seed = 5;
    biggerParams.systemCount = 8;
    const Galaxy bigger = generateGalaxy(biggerParams);
    MiningSim mismatched;
    mismatched.initialize(bigger, lineParams(), 3, 99);
    sol::core::BinaryReader replay(writer.data());
    SOL_CHECK(!mismatched.load(replay));
}

SOL_TEST(mining_generated_galaxy_fields_follow_the_region_gradient)
{
    GalaxyParams galaxyParams;
    galaxyParams.seed = 1701;
    galaxyParams.systemCount = 60;
    const Galaxy galaxy = generateGalaxy(galaxyParams);

    MiningParams params;
    params.ores = {OreEntry{.commodity = 0, .weight = {1.0f, 1.0f, 1.0f}}};
    MiningSim mining;
    mining.initialize(galaxy, params, 3, 1701);

    std::uint32_t fields[3] = {0, 0, 0};
    std::uint32_t systems[3] = {0, 0, 0};
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        const std::size_t tier = static_cast<std::size_t>(galaxy.systems[i].region);
        fields[tier] += mining.fieldCount(i);
        ++systems[tier];
    }
    SOL_REQUIRE(systems[0] > 0);
    SOL_REQUIRE(systems[2] > 0);
    const double corePerSystem = static_cast<double>(fields[0]) / systems[0];
    const double fringePerSystem = static_cast<double>(fields[2]) / systems[2];
    SOL_CHECK(fringePerSystem > corePerSystem);

    // With no ore table at all, nothing anywhere is mineable — a galaxy whose
    // defs carry no ore gets no empty fields to fly to.
    MiningSim oreless;
    oreless.initialize(galaxy, MiningParams{}, 3, 1701);
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        total += oreless.fieldCount(i);
    }
    SOL_CHECK(total == 0);
}
