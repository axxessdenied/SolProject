// The photograph of the galaxy this game ships (Phase 29 stage A).
//
// ⚑⚑⚑ THIS FILE EXISTS BECAUSE A DETERMINISM TEST IS NOT A REGRESSION TEST,
// AND THE TWO READ IDENTICALLY UNTIL SOMETHING CHANGES. `universe.unit`'s
// `universe_same_seed_same_galaxy` generates the galaxy TWICE IN ONE BINARY and
// compares the halves - which is *same input, same output*, and passes exactly
// as well before and after any change whatsoever. Phase 29's exit criterion is
// the other claim, *same output as yesterday*: "a galaxy generated with no
// authored systems present is byte-identical to today's at the shipped seed".
// Nothing in the tree could state that, because there was no yesterday recorded
// anywhere.
//
// ⚑⚑ SO THE ORDER OF WORK IS THE WHOLE POINT, AND IT IS WRITTEN INTO THE PHASE
// RATHER THAN LEFT TO JUDGEMENT: these numbers are committed BEFORE a line of
// the authored-systems pre-pass exists. Recorded afterwards they would be a
// photograph of the change rather than of what it replaced, and the exit
// criterion would certify the new generator against itself.
//
// ⚑ It lives in `game.unit` rather than beside the generator because the
// shipped galaxy is not a function of `GalaxyParams{}` alone: faction count,
// station archetypes, their region weights and their `producesFrom` all come
// out of `game/data`, and `SpaceWorld::generateUniverse` is the only place that
// turns defs into params. Same split as `model_role_tests` - the test goes
// where it can see both halves.
//
// ⚑⚑⚑⚑ AND THE FIRST VERSION OF THIS FILE WAS WRONG, IN A WAY ONLY RUNNING IT
// SOMEWHERE ELSE COULD SHOW: "BYTE-IDENTICAL" IS NOT A PROPERTY OF THE GALAXY,
// IT IS A PROPERTY OF A TOOLCHAIN. One digest over every field passed on
// Windows and FAILED on Linux at the same commit - and the difference is not a
// bug in either build. `sinf`/`cosf` are not required to be correctly rounded,
// UCRT's and glibc's disagree on the last bit for a minority of arguments, and
// `scatterSystems` puts a system at `r*cos(theta), _, r*sin(theta)`. Measured
// at d9d9a60, seed 1701: **19 of 80 systems** carry a coordinate that differs,
// each by ~1 ulp; the largest downstream consequence is a gate sitting ~30 m
// from where the other toolchain puts it, on a 6e8 m playfield. Nothing
// discrete moves at all.
//
// ⚑⚑⚑ SO THE DIGEST IS SPLIT BY WHAT IT CAN HONESTLY PROMISE, AND THE SPLIT
// RULE IS MECHANICAL SO IT CANNOT ROT: **an integer or a string is STRUCTURE, a
// float or a double is GEOMETRY.** Structure is asserted on every platform - it
// was verified identical across four builds (MSVC dev, MSVC release, GCC dev,
// GCC release) - and it is the layer every way Phase 29 could go wrong lands
// in: an inserted node, an extra draw from a shared RNG stream, a reordered
// pass, a renamed system. Geometry is asserted against the C library that
// recorded it, because that is the only thing it is true of. ⚑ Both geometry
// numbers were identical between the dev and release builds of their own
// toolchain, so this is a libm difference and NOT an optimisation-level one.

#include "space_world.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/core/hash.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::core::fnv1a;
using sol::core::hashCombine;
using sol::sim::Galaxy;

namespace {

// Raw bits rather than a rounded decimal, even for the half that is not
// portable. Quantising was considered and refused by arithmetic: the noise is
// ~1 float ulp, so a quantum only ten times coarser leaves ~2400 coordinates
// each with a one-in-ten chance of straddling a bucket edge, and a quantum
// coarse enough to be safe is coarse enough to hide a real change. A number
// that is exact and scoped beats a number that is approximate and global.
[[nodiscard]] std::uint64_t combineBits(std::uint64_t seed, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return hashCombine(seed, bits);
}

[[nodiscard]] std::uint64_t combineBits(std::uint64_t seed, double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return hashCombine(seed, bits);
}

[[nodiscard]] std::uint64_t combineText(std::uint64_t seed, const std::string& text)
{
    return fnv1a(text, hashCombine(seed, text.size()));
}

[[nodiscard]] std::uint64_t combineVec(std::uint64_t seed, const sol::core::DVec3& v)
{
    return combineBits(combineBits(combineBits(seed, v.x), v.y), v.z);
}

// Three digests rather than one, for the reason `shipped_mesh_tests` splits
// positions from normals: a single number that changed says only "the galaxy is
// different", where these say which layer moved - and the first of them says it
// on every platform.
struct Digests
{
    // Integers and strings only. Portable, and the layer that carries every
    // failure mode this phase actually has.
    std::uint64_t structure = sol::core::kFnvOffsetBasis;
    // Galaxy-map coordinates: `float`, straight out of `scatterSystems`, and
    // the origin of the whole cross-toolchain difference.
    std::uint64_t mapGeometry = sol::core::kFnvOffsetBasis;
    // In-system placement: `double`, and where a 1-ulp map difference is
    // amplified into metres of gate position.
    std::uint64_t systemGeometry = sol::core::kFnvOffsetBasis;
};

[[nodiscard]] Digests digestOf(const Galaxy& galaxy)
{
    Digests d;
    d.structure = hashCombine(d.structure, galaxy.seed);
    d.structure = hashCombine(d.structure, galaxy.systems.size());
    d.structure = hashCombine(d.structure, galaxy.links.size());
    d.structure = hashCombine(d.structure, galaxy.clans.size());

    for (const sol::sim::SystemSpec& system : galaxy.systems) {
        d.structure = combineText(d.structure, system.name);
        d.structure = hashCombine(d.structure, static_cast<std::uint64_t>(system.region));
        d.structure = hashCombine(d.structure, system.factionIndex);
        d.structure = hashCombine(d.structure, system.seed);
        d.structure = hashCombine(d.structure, system.primaryPlanet);
        d.structure = hashCombine(d.structure, system.planets.size());
        d.structure = hashCombine(d.structure, system.stations.size());
        d.structure = hashCombine(d.structure, system.gates.size());

        d.mapGeometry = combineBits(d.mapGeometry, system.mapPosition.x);
        d.mapGeometry = combineBits(d.mapGeometry, system.mapPosition.y);
        d.mapGeometry = combineBits(d.mapGeometry, system.mapPosition.z);

        d.systemGeometry = combineBits(d.systemGeometry, system.starRadius);
        for (const sol::sim::PlanetSpec& planet : system.planets) {
            d.structure = combineText(d.structure, planet.name);
            d.systemGeometry = combineVec(d.systemGeometry, planet.position);
            d.systemGeometry = combineBits(d.systemGeometry, planet.radius);
        }
        for (const sol::sim::StationSpec& station : system.stations) {
            d.structure = combineText(d.structure, station.name);
            d.structure = hashCombine(d.structure, station.archetype);
            d.systemGeometry = combineVec(d.systemGeometry, station.position);
        }
        for (const sol::sim::GateSpec& gate : system.gates) {
            d.structure = hashCombine(d.structure, gate.toSystem);
            d.systemGeometry = combineVec(d.systemGeometry, gate.position);
        }
    }
    for (const sol::sim::GateLink& link : galaxy.links) {
        d.structure = hashCombine(hashCombine(d.structure, link.a), link.b);
    }
    for (const sol::sim::ClanSpec& clan : galaxy.clans) {
        d.structure = combineText(d.structure, clan.name);
        d.structure = hashCombine(d.structure, clan.templateIndex);
        d.structure = hashCombine(d.structure, clan.seed);
        d.structure = hashCombine(d.structure, clan.homeSystem);
    }
    return d;
}

// ⚑ The committed base layer, merged the way `GameContent::reloadDefs` merges
// it - `mergeDirectory` over the whole directory rather than a list of file
// stems, so adding a def file to `game/data` cannot leave this test quietly
// photographing a galaxy the game does not generate. Mods are deliberately
// absent: the criterion is about the BASE game.
[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

// A bare hash comparison is undiagnosable when it fails, so say what came out,
// in a form that can be pasted straight back into the tables below on the day
// changing a golden is a DECISION rather than an accident.
bool checkDigest(const char* layer, std::uint64_t actual, std::uint64_t expected)
{
    if (actual == expected) {
        return true;
    }
    std::printf("  galaxy %s digest: got 0x%016llXull, want 0x%016llXull\n",
                layer,
                static_cast<unsigned long long>(actual),
                static_cast<unsigned long long>(expected));
    return false;
}

// Which libm this binary calls, since that - not the compiler and not the
// optimisation level - is what the geometry digest is a fact about.
[[nodiscard]] constexpr const char* libmTag()
{
#if defined(_MSC_VER)
    return "ucrt";
#elif defined(__GLIBC__)
    return "glibc";
#else
    return nullptr; // some other libm: no golden recorded, and none guessed
#endif
}

// ⚑⚑ THE GOLDEN, RECORDED FROM `dev` AT d9d9a60 ON 2026-08-29 - BEFORE ANY PART
// OF PHASE 29's GENERATOR WORK EXISTED. Seed 1701 (`game::kDefaultUniverseSeed`)
// against the committed `game/data`.
//
// ⚑ WHEN THE STRUCTURE DIGEST FAILS, IT IS NOT A TEST TO FIX. Either the change
// was supposed to leave the procedural galaxy alone - in which case this is the
// bug, found - or the galaxy was deliberately reshaped, in which case
// re-recording this number is a commit of its own that says WHY, because every
// save ever made against the old one now describes a place that no longer
// exists.
constexpr std::size_t kGoldenSystemCount = 80;
constexpr std::size_t kGoldenLinkCount = 158;
constexpr std::size_t kGoldenClanCount = 10;
constexpr std::uint64_t kGoldenStructure = 0x51F04FE42530B01Aull;

// ⚑ One row per C library this project has actually built and run on. A libm
// that is not listed is REPORTED, not failed: nothing is broken on a machine
// this project has never measured, and the structure digest above still holds
// there. Adding a row is a measurement, never a guess.
struct GeometryGolden
{
    const char* libm;
    std::uint64_t map;
    std::uint64_t system;
};

constexpr GeometryGolden kGeometryGoldens[] = {
    {"ucrt", 0xC7BBBBDD48EDC17Aull, 0x036D91AB8C912AA8ull},  // MSVC 19.5x, Windows 11
    {"glibc", 0xB9C4A77C410B6076ull, 0x68668A5BCDC21D25ull}, // GCC 13.3, glibc 2.39
};

[[nodiscard]] const GeometryGolden* geometryGoldenForThisBuild()
{
    const char* tag = libmTag();
    if (tag == nullptr) {
        return nullptr;
    }
    for (const GeometryGolden& row : kGeometryGoldens) {
        if (std::strcmp(row.libm, tag) == 0) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

// The exit criterion of Phase 29, stated before the phase can affect it. This
// half runs everywhere and is the one that has to stay green.
SOL_TEST(shipped_seed_galaxy_keeps_its_recorded_structure)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_CHECK(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    // Counts first: a failure here says WHAT the galaxy is before a hash says
    // only that it differs, and `anywhere` placement growing `galaxy.systems`
    // past `params.systemCount` is this phase's most likely way of moving them.
    std::printf("  galaxy: %zu systems, %zu links, %zu clans\n",
                galaxy.systems.size(),
                galaxy.links.size(),
                galaxy.clans.size());
    SOL_CHECK(galaxy.systems.size() == kGoldenSystemCount);
    SOL_CHECK(galaxy.links.size() == kGoldenLinkCount);
    SOL_CHECK(galaxy.clans.size() == kGoldenClanCount);

    SOL_CHECK(checkDigest("structure", digestOf(galaxy).structure, kGoldenStructure));
}

// The other half, scoped to the toolchain it is true of.
SOL_TEST(shipped_seed_galaxy_keeps_its_recorded_geometry_on_a_known_libm)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_CHECK(world.generateUniverse(defs));
    const Digests d = digestOf(world.galaxy());

    const GeometryGolden* golden = geometryGoldenForThisBuild();
    if (golden == nullptr) {
        std::printf("  no geometry golden for this libm; map 0x%016llXull system 0x%016llXull\n",
                    static_cast<unsigned long long>(d.mapGeometry),
                    static_cast<unsigned long long>(d.systemGeometry));
        return;
    }
    SOL_CHECK(checkDigest("map geometry", d.mapGeometry, golden->map));
    SOL_CHECK(checkDigest("system geometry", d.systemGeometry, golden->system));
}

// ⚑ A golden is only worth its bytes if the digest actually reads the galaxy,
// and a hash with a mistake in it - a loop that never runs, a field folded into
// the wrong accumulator - produces a perfectly stable number that means nothing.
// So: perturb one field per layer and require that layer, and only that layer,
// to notice. This is the guard the golden cannot give itself, and it is also
// what pins the structure/geometry split: a moved coordinate must NOT show up
// in the number that is asserted cross-platform.
SOL_TEST(each_digest_layer_notices_a_change_in_its_own_fields)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    SOL_CHECK(world.generateUniverse(defs));
    const Digests base = digestOf(world.galaxy());

    Galaxy mutated = world.galaxy();
    SOL_REQUIRE(mutated.systems.size() >= 2);
    SOL_REQUIRE(!mutated.links.empty());
    SOL_REQUIRE(!mutated.clans.empty());
    SOL_REQUIRE(!mutated.systems[1].planets.empty());

    mutated.systems[1].name += "x";
    SOL_CHECK(digestOf(mutated).structure != base.structure);

    mutated = world.galaxy();
    mutated.links.pop_back();
    SOL_CHECK(digestOf(mutated).structure != base.structure);

    mutated = world.galaxy();
    mutated.clans[0].homeSystem += 1;
    SOL_CHECK(digestOf(mutated).structure != base.structure);

    // The two that must move ONE number each: geometry is not allowed to leak
    // into the digest that other platforms are held to.
    mutated = world.galaxy();
    mutated.systems[1].mapPosition.y += 1.0f;
    SOL_CHECK(digestOf(mutated).mapGeometry != base.mapGeometry);
    SOL_CHECK(digestOf(mutated).structure == base.structure);

    mutated = world.galaxy();
    mutated.systems[1].planets[0].position.z += 1.0;
    SOL_CHECK(digestOf(mutated).systemGeometry != base.systemGeometry);
    SOL_CHECK(digestOf(mutated).structure == base.structure);
}
