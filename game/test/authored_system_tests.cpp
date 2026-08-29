// An authored system, from TOML to a place in the galaxy (Phase 29 stage A).
//
// ⚑⚑ THIS SUITE EXISTS FOR THE SEAM, WHICH NEITHER OF THE OTHER TWO CAN SEE.
// `universe.unit` proves the generator against a hand-built `GalaxyParams` and
// never learns what a def is - that is the layering, kept since Phase 5.
// `assets.unit` proves the parser and never generates a galaxy. What is left
// over is the translation in `SpaceWorld::generateUniverse`: faction ids into
// claimant indices, station ids into archetype indices, region words into the
// enum. It is the half of the phase most likely to be quietly wrong, because a
// mis-resolved index is a legal index.
//
// ⚑ It runs against the REAL `game/data` with one authored row merged on top,
// rather than a fixture, because "does this work with the game's own factions
// and station archetypes" is the actual question - and it is the question a
// fixture is guaranteed to answer wrongly.

#include "space_world.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::sim::Galaxy;
using sol::sim::SystemSpec;

namespace {

// One authored system naming things that really exist in `game/data`: the Navy
// claims territory, and a refinery is an archetype the shipped stations file
// declares. Kept here rather than in `game/data` on purpose - stage A must not
// change the galaxy the golden photographs, and the example file that ships is
// stage D's.
constexpr const char* kAuthored = R"(
[[system]]
id = "test.harrow"
name = "Harrow"
region = "fringe"
faction = "sol.navy"
secret = true
primary_planet = 1

[[system.planet]]
name = "Harrow Prime"

[[system.planet]]
name = "The Anvil"
radius = 4200000.0

[[system.station]]
name = "The Long Watch"
station = "sol.station_refinery"
)";

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] const SystemSpec* findAuthored(const Galaxy& galaxy, const char* id)
{
    for (const SystemSpec& system : galaxy.systems) {
        if (system.authoredId == id) {
            return &system;
        }
    }
    return nullptr;
}

// The claimant index the generator would give a major faction: its position
// among the majors in def order, which is the order `claimTerritory` hands them
// out in. Recomputed here from the defs rather than assumed, because assuming
// it is exactly the mistake this suite is looking for.
[[nodiscard]] std::uint32_t majorIndexOf(const DefDatabase& defs, const char* id)
{
    std::uint32_t major = 0;
    for (const sol::assets::FactionDef& faction : defs.factions()) {
        if (faction.kind == sol::assets::FactionKind::Pirate) {
            continue;
        }
        if (faction.id == id) {
            return major;
        }
        ++major;
    }
    return sol::sim::kNoFaction;
}

[[nodiscard]] std::uint32_t archetypeIndexOf(const DefDatabase& defs, const char* id)
{
    for (std::size_t i = 0; i < defs.stations().size(); ++i) {
        if (defs.stations()[i].id == id) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return 0xffff'ffffu;
}

} // namespace

SOL_TEST(an_authored_system_reaches_the_galaxy_with_every_field_it_was_written_with)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kAuthored, std::strlen(kAuthored), "test/systems.toml", &error));
    SOL_REQUIRE(defs.validateSystems(&error));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_CHECK(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    const SystemSpec* harrow = findAuthored(galaxy, "test.harrow");
    SOL_REQUIRE(harrow != nullptr);
    const std::uint32_t index = static_cast<std::uint32_t>(harrow - galaxy.systems.data());
    std::printf("  test.harrow landed at system %u of %zu, '%s'\n",
                index,
                galaxy.systems.size(),
                harrow->name.c_str());

    SOL_CHECK(harrow->name == "Harrow");
    SOL_CHECK(harrow->region == sol::sim::Region::Fringe);
    SOL_CHECK(harrow->secret);

    // The two resolutions this suite exists for.
    SOL_CHECK(harrow->factionIndex == majorIndexOf(defs, "sol.navy"));
    SOL_REQUIRE(harrow->stations.size() == 1);
    SOL_CHECK(harrow->stations[0].name == "The Long Watch");
    SOL_CHECK(harrow->stations[0].archetype == archetypeIndexOf(defs, "sol.station_refinery"));

    // Authored bodies, generated orbits: the author never wrote a coordinate.
    SOL_REQUIRE(harrow->planets.size() == 2);
    SOL_CHECK(harrow->planets[0].name == "Harrow Prime");
    SOL_CHECK(harrow->planets[1].name == "The Anvil");
    SOL_CHECK(harrow->planets[1].radius == 4'200'000.0);
    SOL_CHECK(harrow->primaryPlanet == 1);
    SOL_CHECK(sol::core::length(harrow->planets[1].position) > 0.0);

    // ⚑ The invariant six unguarded call sites depend on, checked on the
    // authored system specifically: this is the one the generator did not build.
    SOL_CHECK(harrow->primaryPlanet < harrow->planets.size());

    // It is a place you can reach: it kept the node's gates, so it is on the
    // graph rather than beside it.
    SOL_CHECK(!harrow->gates.empty());
    SOL_CHECK(!sol::sim::routeBetween(galaxy, 0, index).empty());

    // And the galaxy did not grow: a `random` system consumes an ordinary
    // system's slot. `anywhere` is what grows it, and that is stage B.
    SOL_CHECK(galaxy.systems.size() == 80);
}

// ⚑ A mod that names something the base game does not have is refused BEFORE it
// can reach the generator, with the id in the message - decision 3. Warning and
// falling back would put the campaign's starting system somewhere nobody chose.
SOL_TEST(an_authored_system_naming_a_missing_archetype_is_refused_by_name)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    constexpr const char* kBad = R"(
[[system]]
id = "test.pier"

[[system.station]]
name = "Pier Nine"
station = "sol.station_that_was_removed"
)";
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kBad, std::strlen(kBad), "test/systems.toml", &error));
    SOL_CHECK(!defs.validateSystems(&error));
    SOL_CHECK(error.find("sol.station_that_was_removed") != std::string::npos);
    SOL_CHECK(error.find("test.pier") != std::string::npos);
    SOL_CHECK(error.find("test/systems.toml") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Phase 29 stage B: the other three rules, across the same seam.
// ---------------------------------------------------------------------------

namespace {

// All three of stage B's rules against the REAL `game/data`, so the faction id
// and the archetype id are the ones the shipped galaxy actually has. The ring
// is 1-3 jumps because that is satisfiable in an 80-system galaxy at any seed
// worth shipping; the unsatisfiable case gets its own fixture below.
constexpr const char* kStageBAuthored = R"(
[[system]]
id = "test.outpost"
name = "Outpost"
placement = "anywhere"

[[system]]
id = "test.capital"
name = "Admiralty"
placement = "at_system"
at_system = "sol.navy"

[[system]]
id = "test.picket"
name = "Picket"
placement = "jumps_from"
jumps_from = { system = "test.capital", min = 1, max = 3 }
)";

// A ring no 80-system galaxy can satisfy: the graph is nowhere near 40 jumps
// across.
constexpr const char* kImpossibleRing = R"(
[[system]]
id = "test.anchor"

[[system]]
id = "test.nowhere"
placement = "jumps_from"
jumps_from = { system = "test.anchor", min = 40, max = 50 }
)";

} // namespace

// The seam for stage B: a faction id becomes a CAPITAL rather than an owner,
// which is a second and entirely separate resolution of the same id through
// the same rule. A mis-resolved capital is a legal index, so this is exactly
// the shape this suite exists to catch.
SOL_TEST(the_three_stage_b_rules_reach_the_galaxy_through_the_real_defs)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kStageBAuthored, std::strlen(kStageBAuthored), "test/systems.toml", &error));
    if (!defs.validateSystems(&error)) {
        std::printf("  %s\n", error.c_str());
    }
    SOL_REQUIRE(defs.validateSystems(&error));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    const SystemSpec* outpost = findAuthored(galaxy, "test.outpost");
    const SystemSpec* capital = findAuthored(galaxy, "test.capital");
    const SystemSpec* picket = findAuthored(galaxy, "test.picket");
    SOL_REQUIRE(outpost != nullptr);
    SOL_REQUIRE(capital != nullptr);
    SOL_REQUIRE(picket != nullptr);

    const std::uint32_t outpostIndex = static_cast<std::uint32_t>(outpost - galaxy.systems.data());
    const std::uint32_t capitalIndex = static_cast<std::uint32_t>(capital - galaxy.systems.data());
    const std::uint32_t picketIndex = static_cast<std::uint32_t>(picket - galaxy.systems.data());

    // `anywhere` GREW the galaxy, and it grew it at the end. 80 is what the
    // golden asserts for a galaxy with no authored systems in it; one
    // insertion makes it 81, and the newcomer is the last index.
    std::printf("  %zu systems; outpost %u, capital %u, picket %u\n",
                galaxy.systems.size(),
                outpostIndex,
                capitalIndex,
                picketIndex);
    SOL_CHECK(galaxy.systems.size() == 81);
    SOL_CHECK(outpostIndex == 80);
    SOL_CHECK(outpost->name == "Outpost");

    // ⚑⚑ THE ONE THIS SUITE IS FOR: "sol.navy" resolved to the CAPITAL of the
    // Navy, not merely to a system the Navy owns. It is a core system, and the
    // Navy holds it - which is what a capital is, and a wrong index would
    // almost certainly still be a legal system somewhere.
    SOL_CHECK(capital->name == "Admiralty");
    SOL_CHECK(capital->region == sol::sim::Region::Core);
    SOL_CHECK(capital->factionIndex == majorIndexOf(defs, "sol.navy"));

    // And the ring is measured against the graph the player will fly.
    const std::vector<std::uint32_t> route = sol::sim::routeBetween(galaxy, capitalIndex, picketIndex);
    SOL_REQUIRE(!route.empty());
    const std::uint32_t jumps = static_cast<std::uint32_t>(route.size()) - 1;
    std::printf("  picket is %u jump(s) from the Admiralty\n", jumps);
    SOL_CHECK(jumps >= 1);
    SOL_CHECK(jumps <= 3);
}

// ⚑⚑⚑ THE REFUSAL, END TO END, AND THIS IS THE HALF NEITHER OTHER SUITE CAN
// REACH. `sol::sim` reports an id, a rule and a reason and has never known
// what a file is - there is not one `SOL_LOG` in all of `engine/sim/src`. The
// game layer holds `SystemDef::source`, matches on the id and composes the
// error decision 3 asks for. What is testable from here is the VERDICT: boot
// data that cannot be placed makes `generateUniverse` say so instead of
// shipping a galaxy with a hole where the campaign starts.
SOL_TEST(an_unplaceable_authored_system_refuses_the_whole_universe)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kImpossibleRing, std::strlen(kImpossibleRing), "test/systems.toml", &error));
    // ⚑ It passes VALIDATION, and that is the point rather than an oversight:
    // the anchor exists and is declared first, so nothing about the file is
    // wrong. Whether the ring can be satisfied is a claim about a gate graph,
    // and the gate graph comes from the seed - so there is no load-time check
    // that could have settled it.
    SOL_CHECK(defs.validateSystems(&error));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_CHECK(!world.generateUniverse(defs));
}

// The stage A file still parses and still lands, with no `placement` line in
// it at all. Stage B added three rules and changed nothing an author had
// already written.
SOL_TEST(a_stage_a_file_still_places_after_stage_b)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kAuthored, std::strlen(kAuthored), "test/systems.toml", &error));
    SOL_REQUIRE(defs.validateSystems(&error));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_REQUIRE(world.generateUniverse(defs));
    // A replacement, so the galaxy is still the size the golden records.
    SOL_CHECK(world.galaxy().systems.size() == 80);
    SOL_CHECK(findAuthored(world.galaxy(), "test.harrow") != nullptr);
}
