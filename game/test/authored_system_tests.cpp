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
    world.generateUniverse(defs);
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
