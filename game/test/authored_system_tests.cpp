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
        // ⚑ `!= Major` since Phase 37 stage B, which is the production rule's own
        // spelling: this is a mirror, and a mirror that has drifted is worse
        // than none. A shadow faction is not a claimant either, and it is the
        // one whose position among the majors would be counted by accident.
        if (faction.kind != sol::assets::FactionKind::Major) {
            continue;
        }
        if (faction.id == id) {
            return major;
        }
        ++major;
    }
    return sol::sim::kNoFaction;
}

// ⚑⚑⚑ EVERY INDEX IN THIS SUITE IS DERIVED FROM DEF ORDER RATHER THAN WRITTEN
// DOWN, AND STAGE D IS WHY. Stages A-C could say "the appended system is index
// 80" because `game/data` shipped no authored content at all and a fixture was
// the only thing being appended. Since stage D it ships `sol.lantern`, so every
// literal index in this file was a statement about how much content the base
// game happens to carry, wearing the costume of a statement about the fixture
// under test. The helpers below restate the generator's own documented rule -
// everything the seed produced first, then every `anywhere` [[system]] in def
// order, then every constellation's members contiguously in def order - which
// is a better assertion than the numbers were, because it says what it means.
constexpr std::size_t kProceduralSystems = 80; // what the golden records

[[nodiscard]] std::size_t anywhereCount(const DefDatabase& defs)
{
    std::size_t count = 0;
    for (const sol::assets::SystemDef& system : defs.systems()) {
        count += system.placement == "anywhere" ? 1 : 0;
    }
    return count;
}

[[nodiscard]] std::size_t memberCount(const DefDatabase& defs)
{
    std::size_t count = 0;
    for (const sol::assets::ConstellationDef& group : defs.constellations()) {
        count += group.members.size();
    }
    return count;
}

// How many nodes these defs ADD to the galaxy rather than replace.
[[nodiscard]] std::size_t appendedNodeCount(const DefDatabase& defs)
{
    return anywhereCount(defs) + memberCount(defs);
}

// Where an `anywhere` [[system]] should land: after the procedural galaxy, in
// def order among the other `anywhere` rows.
[[nodiscard]] std::size_t expectedAnywhereIndex(const DefDatabase& defs, const char* id)
{
    std::size_t ordinal = 0;
    for (const sol::assets::SystemDef& system : defs.systems()) {
        if (system.placement != "anywhere") {
            continue;
        }
        if (system.id == id) {
            break;
        }
        ++ordinal;
    }
    return kProceduralSystems + ordinal;
}

// Where a constellation's members begin: after every `anywhere` system.
[[nodiscard]] std::size_t expectedConstellationBase(const DefDatabase& defs)
{
    return kProceduralSystems + anywhereCount(defs);
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

    // And the galaxy did not grow BY THIS ROW: a `random` system consumes an
    // ordinary system's slot. `anywhere` is what grows it, and that is stage B.
    // ⚑ The 81st is `sol.lantern`, which `game/data/systems.toml` has shipped
    // since stage D - so this count is a statement about `test.harrow` plus
    // whatever the base game authors, and it is written that way rather than
    // as a literal that would need re-deriving the next time either changes.
    SOL_CHECK(galaxy.systems.size() == kProceduralSystems + appendedNodeCount(defs));
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
    SOL_CHECK(galaxy.systems.size() == kProceduralSystems + appendedNodeCount(defs));
    SOL_CHECK(outpostIndex == expectedAnywhereIndex(defs, "test.outpost"));
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
    // A replacement, so this file adds nothing to the size the base game's own
    // authored content already made it.
    SOL_CHECK(world.galaxy().systems.size() == kProceduralSystems + appendedNodeCount(defs));
    SOL_CHECK(findAuthored(world.galaxy(), "test.harrow") != nullptr);
}

// ---------------------------------------------------------------------------
// Phase 29 stage C: a constellation, across the same seam.
// ---------------------------------------------------------------------------

namespace {

// A five-member group against the REAL `game/data`, so the faction id and the
// archetype id are the ones the shipped galaxy actually has - and a STAR rather
// than a chain, because members land in a tight cluster and proximity draws a
// near-complete mesh over any small group by accident. A shape proximity does
// not draw is the only shape that can prove a lane was seeded.
constexpr const char* kStageCAuthored = R"(
[[constellation]]
id = "test.deadfall"

[[constellation.system]]
id = "test.deadfall_hub"
name = "Deadfall"
region = "fringe"
faction = "sol.navy"

[[constellation.system.planet]]
name = "Deadfall Prime"
radius = 6400000.0

[[constellation.system.station]]
name = "The Long Watch"
station = "sol.station_refinery"

[[constellation.system]]
id = "test.deadfall_n"
name = "Deadfall North"

[[constellation.system]]
id = "test.deadfall_e"
name = "Deadfall East"

[[constellation.system]]
id = "test.deadfall_s"
name = "Deadfall South"

[[constellation.system]]
id = "test.deadfall_w"
name = "Deadfall West"
lawless = true

[[constellation.link]]
from = "test.deadfall_hub"
to = "test.deadfall_n"

[[constellation.link]]
from = "test.deadfall_hub"
to = "test.deadfall_e"

[[constellation.link]]
from = "test.deadfall_hub"
to = "test.deadfall_s"

[[constellation.link]]
from = "test.deadfall_hub"
to = "test.deadfall_w"

[[system]]
id = "test.picket"
name = "Picket"
placement = "jumps_from"
jumps_from = { system = "test.deadfall_hub", min = 1, max = 3 }
)";

[[nodiscard]] bool linked(const Galaxy& galaxy, std::uint32_t a, std::uint32_t b)
{
    for (const sol::sim::GateLink& link : galaxy.links) {
        if ((link.a == a && link.b == b) || (link.a == b && link.b == a)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint32_t indexOfAuthored(const Galaxy& galaxy, const char* id)
{
    const SystemSpec* spec = findAuthored(galaxy, id);
    return spec != nullptr ? static_cast<std::uint32_t>(spec - galaxy.systems.data()) : 0xffff'ffffu;
}

} // namespace

// The seam for stage C, and it carries a resolution neither other suite can
// see: a lane's ENDS are member IDS in the file and member INDICES by the time
// `sol::sim` receives them, the same way a station id becomes an archetype
// index. A mis-resolved member index is a legal member index, which is exactly
// the shape this suite exists to catch.
SOL_TEST(a_constellation_reaches_the_galaxy_as_a_group_with_its_lanes_intact)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kStageCAuthored, std::strlen(kStageCAuthored), "test/systems.toml", &error));
    if (!defs.validateSystems(&error)) {
        std::printf("  %s\n", error.c_str());
    }
    SOL_REQUIRE(defs.validateSystems(&error));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    // Five appended members plus one `jumps_from` REPLACEMENT, which consumes
    // an ordinary system's slot rather than adding one: 80 + 5.
    std::printf("  %zu systems\n", galaxy.systems.size());
    SOL_CHECK(galaxy.systems.size() == kProceduralSystems + appendedNodeCount(defs));

    const std::uint32_t hub = indexOfAuthored(galaxy, "test.deadfall_hub");
    // Members are contiguous, at the end, in def order - after every `anywhere`
    // system, this file's and the base game's alike.
    SOL_REQUIRE(hub == expectedConstellationBase(defs));

    // The hub kept every field its author wrote, resolved through the shipped
    // defs: the Navy is a claimant index, the refinery is an archetype index.
    const SystemSpec* hubSpec = &galaxy.systems[hub];
    SOL_CHECK(hubSpec->name == "Deadfall");
    SOL_CHECK(hubSpec->region == sol::sim::Region::Fringe);
    SOL_CHECK(hubSpec->factionIndex == majorIndexOf(defs, "sol.navy"));
    SOL_REQUIRE(hubSpec->stations.size() == 1);
    SOL_CHECK(hubSpec->stations[0].name == "The Long Watch");
    SOL_CHECK(hubSpec->stations[0].archetype == archetypeIndexOf(defs, "sol.station_refinery"));
    SOL_REQUIRE(!hubSpec->planets.empty());
    SOL_CHECK(hubSpec->planets[0].name == "Deadfall Prime");

    // ⚑ The star, spoke by spoke. Each lane is present, each spoke is exactly
    // one jump from the hub, and each is reachable from the rest of the galaxy.
    const char* spokes[4] = {"test.deadfall_n", "test.deadfall_e", "test.deadfall_s", "test.deadfall_w"};
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t spoke = indexOfAuthored(galaxy, spokes[i]);
        SOL_REQUIRE(spoke == expectedConstellationBase(defs) + 1 + i);
        SOL_CHECK(linked(galaxy, hub, spoke));
        SOL_CHECK(sol::sim::routeBetween(galaxy, hub, spoke).size() == 2);
        SOL_CHECK(!sol::sim::routeBetween(galaxy, 0, spoke).empty());
    }

    // Authored lawlessness on a member survives the whole pipeline, which is
    // the same guard a `[[system]]` gets because it is the same code.
    SOL_CHECK(galaxy.systems[indexOfAuthored(galaxy, "test.deadfall_w")].factionIndex ==
              sol::sim::kNoFaction);

    // ⚑⚑ AND A RING ANCHORED ON A MEMBER RESOLVED, even though the member is
    // declared in a `[[constellation]]` rather than in a `[[system]]`. A
    // constellation cannot fail to be placed, so a member is an anchor whatever
    // order the file was written in.
    const std::uint32_t picket = indexOfAuthored(galaxy, "test.picket");
    SOL_REQUIRE(picket != 0xffff'ffffu);
    const std::vector<std::uint32_t> route = sol::sim::routeBetween(galaxy, hub, picket);
    SOL_REQUIRE(!route.empty());
    const std::uint32_t jumps = static_cast<std::uint32_t>(route.size()) - 1;
    std::printf("  picket is %u jump(s) from Deadfall\n", jumps);
    SOL_CHECK(jumps >= 1 && jumps <= 3);
}

// Stage A's and stage B's files both still place with stage C in the tree, and
// a galaxy with no `[[constellation]]` in it is still 80 systems: the golden's
// claim, restated at the seam.
SOL_TEST(a_file_with_no_constellation_is_the_galaxy_stage_b_left)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    // game/data still declares no constellation of its own - the worked example
    // for one lives in game/mods/example-systems/, which is not on this path.
    SOL_CHECK(defs.constellations().empty());
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kStageBAuthored, std::strlen(kStageBAuthored), "test/systems.toml", &error));
    SOL_REQUIRE(defs.validateSystems(&error));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_REQUIRE(world.generateUniverse(defs));
    // One `anywhere` and two replacements here, plus whatever the base game
    // appends on its own account.
    SOL_CHECK(world.galaxy().systems.size() == kProceduralSystems + appendedNodeCount(defs));
}

// ---------------------------------------------------------------------------
// Phase 29 stage D: the content this repository actually ships, and the mod
// beside it.
//
// ⚑⚑ THESE RUN AGAINST THE COMMITTED FILES RATHER THAN A FIXTURE, WHICH IS THE
// WHOLE POINT OF THEM. Every test above proves the machinery with a string
// literal; nothing anywhere proved that the TOML a person will read as the
// reference example is itself valid, places, and survives the pipeline. A
// worked example that does not work is worse than none.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool mergeExampleMod(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_EXAMPLE_MOD_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_EXAMPLE_MOD_DIR, error.c_str());
        return false;
    }
    return true;
}

} // namespace

// The one authored place the base game ships, end to end through the real
// defs - and addressable by the id `sol.system_by_id` looks up, which is what
// turns this phase from a generator feature into the Act 2 dependency it was
// sequenced as.
SOL_TEST(the_base_game_ships_one_authored_place_and_it_is_findable_by_its_id)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    std::string error;
    SOL_REQUIRE(defs.validateSystems(&error));
    // The claim the golden's strip depends on. If this is ever false the
    // golden stops certifying anything, and it says so there too.
    SOL_REQUIRE(!defs.systems().empty());

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    const SystemSpec* lantern = findAuthored(galaxy, "sol.lantern");
    SOL_REQUIRE(lantern != nullptr);
    const std::uint32_t index = static_cast<std::uint32_t>(lantern - galaxy.systems.data());
    std::printf(
        "  sol.lantern is system %u of %zu, '%s'\n", index, galaxy.systems.size(), lantern->name.c_str());

    // `anywhere`, so it is appended rather than taking an ordinary system's
    // slot: nothing the seed produced moved to make room for it.
    SOL_CHECK(index == expectedAnywhereIndex(defs, "sol.lantern"));
    SOL_CHECK(galaxy.systems.size() == kProceduralSystems + appendedNodeCount(defs));

    SOL_CHECK(lantern->name == "Lantern");
    SOL_CHECK(lantern->region == sol::sim::Region::Fringe);
    SOL_REQUIRE(!lantern->planets.empty());
    SOL_CHECK(lantern->planets[0].name == "Lantern's Rest");
    SOL_CHECK(lantern->primaryPlanet == 0);
    SOL_REQUIRE(lantern->stations.size() == 1);
    SOL_CHECK(lantern->stations[0].name == "The Slow Lantern");
    SOL_CHECK(lantern->stations[0].archetype == archetypeIndexOf(defs, "sol.station_refinery"));

    // ⚑⚑ AUTHORED LAWLESSNESS, AND IT IS THE ONE FIELD ON THIS ROW THAT COULD
    // FAIL QUIETLY. Every OTHER unclaimed system in the galaxy ends up
    // belonging to a pirate clan, because `spawnClans` gives each connected
    // neighbourhood of lawless systems to one - so "nobody owns it" is a claim
    // that survives only because that pass was taught to skip an authored
    // owner, and a broken skip would leave a plausible clan sitting here.
    SOL_CHECK(lantern->factionIndex == sol::sim::kNoFaction);

    // It is a place you can get to, not a place beside the galaxy.
    SOL_CHECK(!lantern->gates.empty());
    SOL_CHECK(!sol::sim::routeBetween(galaxy, 0, index).empty());
}

// The other half of decision 6: the example mod is real content that really
// merges, and it adds places without `game/data` knowing it exists.
//
// ⚑ Decision 6 refused making this a test fixture INSTEAD of a mod, on the
// grounds that a fixture "proves the merge and nothing about the runtime".
// That still holds - the runtime proof is the drive - and this is the
// regression net under it rather than a substitute for it.
SOL_TEST(the_example_mod_adds_places_without_touching_the_base_game)
{
    DefDatabase base;
    SOL_REQUIRE(loadShippedDefs(base));
    game::SpaceWorld without;
    without.spawn(game::kDefaultUniverseSeed);
    without.applyDefs(base);
    SOL_REQUIRE(without.generateUniverse(base));

    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SOL_REQUIRE(mergeExampleMod(defs));
    std::string error;
    if (!defs.validateSystems(&error)) {
        std::printf("  %s\n", error.c_str());
    }
    SOL_REQUIRE(defs.validateSystems(&error));
    SOL_REQUIRE(defs.constellations().size() == 1);

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    // ⚑⚑ `applyDefs` FIRST, AND SINCE STAGE F THAT IS LOAD-BEARING RATHER THAN
    // TIDY. `generateUniverse` calls `initializeFactions`, which returns early
    // on a null `m_defs`; `systemSecurityBaseline` now signs its answer from
    // whoever HOLDS a system, so an empty faction table makes every rating in
    // the galaxy read 0.00. The boot log says it twice - `0 faction(s)` and a
    // security histogram of nothing but zeroes.
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();
    std::printf(
        "  %zu systems with the mod, %zu without\n", galaxy.systems.size(), without.galaxy().systems.size());

    // Four members appended plus one `jumps_from` replacement, on top of
    // whatever the base game already appends.
    SOL_CHECK(galaxy.systems.size() == kProceduralSystems + appendedNodeCount(defs));
    SOL_CHECK(galaxy.systems.size() == without.galaxy().systems.size() + 4);

    // ⚑⚑ "WITHOUT TOUCHING THE BASE GAME", ASSERTED RATHER THAN ASSUMED. The
    // base game's own authored system is appended from the same stream the
    // mod's constellation draws from, so a change to the order those two
    // passes run in would move it - silently, and only for players with a mod
    // installed. Same index, same map position, both layers or one.
    const SystemSpec* lantern = findAuthored(galaxy, "sol.lantern");
    const SystemSpec* lanternAlone = findAuthored(without.galaxy(), "sol.lantern");
    SOL_REQUIRE(lantern != nullptr);
    SOL_REQUIRE(lanternAlone != nullptr);
    SOL_CHECK(lantern - galaxy.systems.data() == lanternAlone - without.galaxy().systems.data());
    SOL_CHECK(lantern->mapPosition.x == lanternAlone->mapPosition.x);
    SOL_CHECK(lantern->mapPosition.z == lanternAlone->mapPosition.z);

    // The group, contiguous and in declaration order, each member keeping what
    // its author wrote.
    const char* chain[4] = {
        "example.sable_gate", "example.sable_reach", "example.sable_deep", "example.sable_end"};
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint32_t member = indexOfAuthored(galaxy, chain[i]);
        SOL_REQUIRE(member != sol::sim::kNoSystem);
        SOL_CHECK(member == expectedConstellationBase(defs) + i);
        SOL_CHECK(!sol::sim::routeBetween(galaxy, 0, member).empty());
    }
    SOL_CHECK(findAuthored(galaxy, "example.sable_gate")->name == "Sable Gate");
    SOL_CHECK(findAuthored(galaxy, "example.sable_gate")->factionIndex == majorIndexOf(defs, "sol.navy"));
    SOL_CHECK(findAuthored(galaxy, "example.sable_end")->secret);
    SOL_REQUIRE(!findAuthored(galaxy, "example.sable_end")->planets.empty());
    SOL_CHECK(findAuthored(galaxy, "example.sable_end")->planets[0].name == "The Last Anvil");

    // ⚑⚑ THE LANES ARE DELIBERATELY NOT ASSERTED HERE, AND THAT IS STAGE C's
    // FINDING BEING APPLIED RATHER THAN FORGOTTEN. A group's members sit in a
    // tight cluster by construction, so Prim plus the extra-lane pass draw a
    // near-complete mesh over any small group BY ACCIDENT - a four-member chain
    // asserting its own three lanes passes with the seeding removed entirely.
    // The shapes that can fail are a star over five and a path through six, and
    // `a_constellation_reaches_the_galaxy_as_a_group_with_its_lanes_intact`
    // above carries both. An assertion here would read as coverage and be none.

    // The RING, which is not vacuous: a measured gate distance the generator
    // had to search for, against the file's own declared bounds.
    const std::uint32_t gate = indexOfAuthored(galaxy, "example.sable_gate");
    const std::uint32_t watch = indexOfAuthored(galaxy, "example.sable_watch");
    SOL_REQUIRE(watch != sol::sim::kNoSystem);
    const std::vector<std::uint32_t> route = sol::sim::routeBetween(galaxy, watch, gate);
    SOL_REQUIRE(!route.empty());
    const std::size_t jumps = route.size() - 1;
    std::printf("  example.sable_watch is %zu jump(s) from the chain's mouth\n", jumps);
    SOL_CHECK(jumps >= 1 && jumps <= 4);

    // ⚑ And the anchor is a constellation MEMBER declared in a group this file
    // writes BEFORE the [[system]] anchoring on it - which is legal only
    // because a group cannot fail to be placed and so resolves before any rule.
    SOL_CHECK(galaxy.systems[watch].name == "Sable Watch");

    // ⚑⚑⚑ THE SHIPPED EXAMPLE'S TWO RATINGS, END TO END THROUGH THE REAL FILES
    // (Phase 30 stage E). This is the one place in the suite where an authored
    // `security =` crosses every seam it has - the TOML reader, the def
    // database, the def-to-params translation and the generator's last pass -
    // rather than being handed to `generateGalaxy` as a struct. It is also the
    // regression net under the mod's own text: the README claims Sable Gate is
    // a fringe system held like a capital and Sable Watch is Navy space nobody
    // answers a call in, and both of those are now assertions.
    const SystemSpec* sableGate = findAuthored(galaxy, "example.sable_gate");
    SOL_REQUIRE(sableGate != nullptr);
    SOL_CHECK(sableGate->security == 0.9f);
    const SystemSpec* sableWatch = findAuthored(galaxy, "example.sable_watch");
    SOL_REQUIRE(sableWatch != nullptr);
    SOL_CHECK(sableWatch->security == 0.04f);
    // ⚑ Both are POSITIVE because the Navy holds them, and that is the sign
    // rule arriving from the owner rather than from the file - neither row
    // writes a sign, and neither could.
    SOL_CHECK(sableGate->factionIndex == majorIndexOf(defs, "sol.navy"));
    SOL_CHECK(sableWatch->factionIndex == majorIndexOf(defs, "sol.navy"));
    // ⚑⚑ AND THE NUMBERS ARE OUTSIDE THE CURVE'S OWN REACH, WHICH IS WHAT MAKES
    // THIS AN OVERRIDE RATHER THAN A COINCIDENCE. A fringe system's generated
    // band is [0.18, 0.30]: 0.90 is above every band this galaxy has and 0.04 is
    // below every one of them, so no seed and no region could have produced
    // either by itself.
    for (const SystemSpec& system : galaxy.systems) {
        if (system.authoredId != "example.sable_gate" && system.authoredId != "example.sable_watch") {
            SOL_CHECK(system.security <= 0.86f);
            SOL_CHECK(system.security == 0.0f || system.security < 0.0f || system.security >= 0.17f);
        }
    }
    // ⚑ The low one is under the band below which a call for help is not
    // answered at all, which is the whole point of putting it in the example:
    // one number in one row, and the map says NOBODY COMES in Navy space.
    SOL_CHECK(!game::securityAnswers(world.systemSecurity(indexOfAuthored(galaxy, "example.sable_watch"))));
    SOL_CHECK(game::securityAnswers(world.systemSecurity(indexOfAuthored(galaxy, "example.sable_gate"))));

    // ⚑⚑ AND `sol.lantern` IS UNTOUCHED AT EXACTLY ZERO. The base game's one
    // authored place writes no rating and is authored-lawless, so it keeps the
    // galaxy's only true zero - the value that means nobody comes - while a mod
    // three files away fortifies a system to 0.90.
    SOL_CHECK(lantern->security == 0.0f);
}

// ⚑⚑⚑ SATISFIABILITY IS A PER-SEED VERDICT AND NO LOAD-TIME CHECK CAN SETTLE
// IT, WHICH IS EXACTLY WHY THE CONTENT THIS REPOSITORY SHIPS NEEDS THIS TEST.
// `jumps_from` is a claim about a gate graph; the gate graph comes from the
// seed; and `--seed N` is a command-line flag. A ring that happens to be
// satisfiable at 1701 and nowhere else would turn every other galaxy into a
// refusal at boot - and the example mod would be a worked example of something
// that does not work.
SOL_TEST(the_authored_content_in_this_repository_places_at_every_seed_checked)
{
    constexpr std::uint64_t kSeeds[] = {1701, 1, 2, 7, 42, 9'999, 123'456, 0xDEAD'BEEFull};
    for (const std::uint64_t seed : kSeeds) {
        DefDatabase defs;
        SOL_REQUIRE(loadShippedDefs(defs));
        SOL_REQUIRE(mergeExampleMod(defs));
        std::string error;
        SOL_REQUIRE(defs.validateSystems(&error));

        game::SpaceWorld world;
        world.spawn(seed);
        world.applyDefs(defs); // so the boot histogram this prints is real - see above
        const bool placed = world.generateUniverse(defs);
        std::printf("  seed %llu: %s, %zu systems\n",
                    static_cast<unsigned long long>(seed),
                    placed ? "placed" : "REFUSED",
                    world.galaxy().systems.size());
        SOL_CHECK(placed);
    }
}
