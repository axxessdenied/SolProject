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
            // ⚑⚑⚑⚑ THIS LINE WAS ADDED BY STAGE B TO STOP THE DIGEST GOING BLIND
            // TO COMPOSITIONS, AND STAGE D MEASURED THAT IT NEVER SAW ONE.
            // `generateWithoutAuthoredContent` calls `sol::sim::generateGalaxy`
            // directly and digests THAT galaxy; `composeStations` is a
            // `SpaceWorld` pass over `SpaceWorld`'s own `m_galaxy`, and
            // `universe.cpp` does not contain the word `composition`. So every
            // station reaching this line carries `kNoComposition` - measured, 0
            // composed stations here against 125 in `world.galaxy()` - and the
            // field is a constant. Stage B's digest DID move, which is why
            // nobody looked again: it moved because the hash gained a field, not
            // because it saw a module list.
            //
            // ⚑⚑ THE LINE STAYS, because it is right for any caller that hands
            // this a composed galaxy, and `composed_shipped_galaxy_keeps_its_
            // recorded_composition` below is the instrument that actually
            // watches compositions - over `world.galaxy()`, with a guard that
            // there is something there to watch. Phase 34's own risk register
            // names this exact failure: "a stage that adds a reader of one and
            // forgets the other is the defect this phase produces if it
            // produces one."
            d.structure = hashCombine(d.structure, station.composition);
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

// ⚑⚑⚑⚑ AND THE GALAXY THIS FILE PHOTOGRAPHS IS NOT THE ONE THE GAME SHIPS ANY
// MORE - IT IS THE ONE WITH THE AUTHORED CONTENT TAKEN BACK OUT. Phase 29's
// criterion is *"a galaxy generated with NO AUTHORED SYSTEMS PRESENT is
// byte-identical to today's"*, and since stage D `game/data/systems.toml`
// exists, so loading `game/data` whole no longer produces that galaxy. The
// strip happens at the PARAMS layer rather than by filtering the defs, because
// that is where the claim is actually made: `GalaxyParams::authoredSystems`
// says of itself that empty "is the pre-29 galaxy exactly, which is what
// game.unit's golden holds this to". This is that sentence, executed.
//
// ⚑⚑ A STRIP THAT STRIPS NOTHING LOOKS EXACTLY LIKE A STRIP THAT WORKS, which
// is the same species of invisible failure as an install EXCLUDE that matches
// nothing - the other half of this same stage. So it is checked: the caller
// requires that something was removed, and the count is printed. Delete
// `game/data/systems.toml` and this test stops certifying anything, loudly.
struct StrippedGalaxy
{
    Galaxy galaxy;
    std::size_t authoredSystems = 0;
    std::size_t constellations = 0;
};

[[nodiscard]] StrippedGalaxy generateWithoutAuthoredContent(const DefDatabase& defs, game::SpaceWorld& world)
{
    StrippedGalaxy out;
    world.spawn(game::kDefaultUniverseSeed);
    if (!world.generateUniverse(defs)) {
        return out; // the caller's SOL_REQUIRE on the counts reports this
    }
    // The params the shipped defs actually produced, with the authored halves
    // emptied. Everything else - faction count, station archetypes, their
    // weights and biases, the mining rules station siting consults - stays
    // exactly as `generateUniverse` built it, so this is the shipped galaxy
    // minus one input rather than a fixture that resembles it.
    sol::sim::GalaxyParams params = world.galaxyParams();
    out.authoredSystems = params.authoredSystems.size();
    out.constellations = params.constellations.size();
    params.authoredSystems.clear();
    params.constellations.clear();
    out.galaxy = sol::sim::generateGalaxy(params, &world.miningParams());
    return out;
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
// ⚑⚑⚑ RE-RECORDED BY PHASE 33 STAGE E, FOR THE THIRD TIME IN ONE PHASE AND
// ALWAYS FOR THE SAME REASON. `stations.toml` gained the construction tier's
// two archetypes - Assembly Yard and Shipyard - so the generator now picks from
// eleven rules where it picked from nine, and this digest hashes
// `station.archetype` for every station in the galaxy. Old:
// 0xA57E01F0BEB11451ull (stage C, nine rules), 0xBC19DD29982F8D89ull (stage B,
// seven), and 0x51F04FE42530B01Aull before that (four).
//
// ⚑⚑ WHAT DID NOT MOVE IS THE EVIDENCE THAT THIS IS THE CHANGE IT LOOKS
// LIKE. 80 systems, 158 links and 10 clans are unchanged, and BOTH geometry
// digests below are untouched on both recorded libms - so not one station,
// planet or gate moved a metre. The galaxy is the same place; some of its
// stations now do a different job. Three times in three stages this digest has
// been the only thing that reported a station mix moving, and three times it
// has been right.
//
// ⚑ Stage E's prerequisite commit is the counter-example that makes the rule
// worth stating: it re-tuned nine production rates across the whole economy and
// did NOT move this number, because it touched no placement weight. A digest
// that had moved there would have been the bug.
// ⚑⚑⚑ RE-RECORDED AGAIN BY THE HEGEMONY BLOC (Phase 33 follow-up), AND
// NOT FOR THE REASON IT LOOKS LIKE. This test STRIPS authored content, so the
// seven new `[[system]]` rows are not what moved it - the Assembly Yard and
// Shipyard weights did (1.6/0.6/0.2 to 1.3/0.45/0.2), searched so that the
// shipped galaxy and the mirror `economy_tests.cpp` builds agree on the T3
// split instead of differing by three stations. Old: 0x64EF583821E22A71ull.
// ⚑ The generator change that shipped beside it - an authored owner no
// longer relaying its faction to everything downstream - moved this number by
// NOTHING, which was measured before and after rather than assumed.
// ⚑⚑⚑⚑ RE-RECORDED BY PHASE 34 STAGE B, AND THIS TIME THE PAIR OF NUMBERS IS
// THE WHOLE ARGUMENT. Every station in the galaxy now carries a COMPOSITION -
// a module list rolled for it - and the digest hashes it, so this number had to
// move. What did NOT move is both geometry digests, and that is the evidence
// for the one claim the stage rests on: the composer runs after `generateGalaxy`
// out of a stream of its own, so not one station, planet or gate shifted by a
// metre. A composition pass that had taken its draws inside `populateSystem`
// would have moved all three. Old: 0x054ECEEFDAECC620ull.
constexpr std::uint64_t kGoldenStructure = 0x06BC021EC816F5E8ull;

// ⚑⚑⚑⚑ RECORDED BY PHASE 34 STAGE D, AND IT IS A NEW INSTRUMENT RATHER THAN A
// RE-RECORDING. The number above cannot see a composition (see `digestOf`), so
// this one is taken over the composed galaxy and hashes the module IDS rolled
// for every station. Structure is about where the generator PUT things;
// composition is about what it made them OF, and Phase 34 is entirely the
// second. ⚑ It moves when a recipe, a chance or a module id moves, and - unlike
// the structure digest - when `systems.toml` gains a station too.
//
// ⚑⚑⚑ MOVED BY STAGE E, WHICH FOLDED `StationSpec::shadowOwner` IN - and the
// two goldens that did NOT move are the evidence the stage is what it claims.
// The shadow pass draws from `kShadowStream`, a third stream nothing else
// touches, so both geometry digests are untouched; and the STRUCTURE digest is
// untouched too, because it reads a galaxy `assignShadowOwners` never saw. Only
// the digest taken over the composed galaxy could move, and it did.
// Old: 0x7FD34958D6FDA639ull, then 0x64A186F7FAD64160ull.
//
// ⚑⚑⚑⚑ PHASE 35 STAGE A MOVED IT AGAIN, AND FOR A REASON WORTH READING BEFORE
// ANYBODY ACCEPTS THE NEXT MOVE. The stage added `screens = ["bar"]` to five
// modules, which this digest CANNOT SEE - it hashes module IDS and the list a
// station was composed from, so what a module OFFERS is invisible to it by
// design. What moved the number is one line in `stations.toml`: a recipe row
// for `sol.mod_resort`, a module that had no placement anywhere in the tree and
// could not appear at any seed.
//
// ⚑⚑⚑ A RECIPE *ROW* IS NOT A LOCAL EDIT, AND `composeStations` SAYS THE
// OPPOSITE IN CAPITALS ABOUT THE CASE IT DOES COVER. It walks ONE
// `kCompositionStream` Rng across every station in galaxy order and draws once
// per recipe entry, unconditionally - which makes a *chance* edit local and does
// NOT make an *added row* local. One row on the Shipyard therefore re-rolled
// every station composed after it: 120 distinct compositions became 119, the
// shadow operators fell from 10 to 6, and the fitted plants from 134 to 130.
// Nothing about the composer changed. Same shape as *adding an archetype
// resamples the mix*, one layer down.
// ⚑⚑ AND IT MOVED A SECOND TIME IN STAGE C, FOR THE SECOND OF THE TWO REASONS
// THE PHASE SAID IT WOULD, WHICH IS WHY BOTH ARE WRITTEN DOWN HERE. Stage A
// moved it because a recipe row RESAMPLED the galaxy; stage C moves it because a
// CAST WAS PLACED ON IT - 62 rooms with somebody in them, 6 of them written by
// hand. Neither move is evidence on its own, and that is not a caution invented
// for this comment: stage 34-B's composition line moved for the wrong reason and
// nobody looked again for a whole stage. The counts printed beside the hash are
// what tell the two apart - 125 stations / 119 compositions did NOT change here,
// and the seating did.
// ⚑⚑⚑ AND A THIRD TIME IN PHASE 37 STAGE A, FOR THE ONE REASON THAT IS SUPPOSED
// TO BE LOCAL: a CHANCE moved, not a row. `sol.mod_black_clinic` went 0.08 ->
// 0.30 on the Breaker Yard because at 0.08 it landed on ZERO of the eleven
// yards - authored-but-absent content whose absence left an illicit good with
// no producer. The counts beside the hash are the evidence that it WAS local
// and this is what the comment above asks a reader to check: 125 stations, 119
// distinct compositions and 130 plants are all UNCHANGED from stage C, and only
// the two docks that gained a clinic differ. A resample would have moved all
// three, which is exactly what an added row did in stage A.
// ⚑⚑⚑⚑ AND A FOURTH TIME IN PHASE 37 STAGE C, FOR A REASON THE PHASE NAMED IN
// ADVANCE AND WHICH IS THE OPPOSITE OF STAGE B'S. Stage B added a whole faction
// and this number was required NOT to move - that was its strongest assertion.
// Stage C re-points `StationSpec::shadowOwner` from a per-station pirate-clan
// roll at The Ninth Shift, and this digest hashes that field per station
// precisely so the re-point is visible. ⚑ THE EVIDENCE THAT IT IS THE RIGHT
// MOVE IS THE COUNTS BESIDE IT: 125 stations, 119 distinct compositions and 8
// shadow docks are ALL UNCHANGED. Nothing was resampled; one column was
// rewritten, which is what the stage said it would do.
constexpr std::uint64_t kGoldenComposition = 0xFD691E40F45D9348ull;

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
    const StrippedGalaxy stripped = generateWithoutAuthoredContent(defs, world);
    // The strip did something. Without this the whole file passes vacuously
    // the day `game/data/systems.toml` is emptied or renamed.
    std::printf("  stripped %zu authored system(s) and %zu constellation(s) from the shipped defs\n",
                stripped.authoredSystems,
                stripped.constellations);
    SOL_REQUIRE(stripped.authoredSystems + stripped.constellations > 0);
    const Galaxy& galaxy = stripped.galaxy;

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

// ⚑⚑⚑⚑ THE INSTRUMENT THE STRUCTURE DIGEST ABOVE CANNOT BE, AND STAGE D FOUND
// OUT BY MEASURING RATHER THAN BY READING. The digest above is taken on a galaxy
// straight out of `sol::sim::generateGalaxy`, which has never heard of a
// composition; this one is taken on `world.galaxy()`, which the composer has
// actually run over. It is the only thing in the repository that would notice
// every station in the galaxy being recomposed.
//
// ⚑⚑ IT HASHES MODULE IDS, NOT MODULE INDICES, AND THAT IS DELIBERATE. An index
// into `defs.modules()` moves when anybody inserts a row into `modules.toml`, so
// an index-based digest would fail on an edit that changed no station at all -
// the loudest possible false positive, and the reason a golden gets ignored.
// Hashing the id means this number moves when a RECIPE moves, which is what it
// is for.
//
// ⚑ Unlike the structure digest this one does NOT strip authored content:
// compositions only exist on the world's own galaxy, and the world is where
// `systems.toml` has already been applied. So a new `[[system]]` row moves this
// number and not the one above. That is a difference in scope, not a defect, and
// `m_authoredDigest` is what tells the two causes apart when it fails.
SOL_TEST(composed_shipped_galaxy_keeps_its_recorded_composition)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    std::uint64_t digest = sol::core::kFnvOffsetBasis;
    std::size_t composed = 0;
    std::size_t stations = 0;
    std::size_t shadowed = 0;
    std::size_t seated = 0;
    std::size_t cast = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            ++stations;
            digest = combineText(digest, system.stations[t].name);
            digest = hashCombine(digest, system.stations[t].archetype);
            const std::span<const std::uint32_t> modules = world.stationModules(s, t);
            composed += modules.empty() ? 0 : 1;
            digest = hashCombine(digest, modules.size());
            for (const std::uint32_t module : modules) {
                SOL_REQUIRE(module < defs.modules().size());
                digest = combineText(digest, defs.modules()[module].id);
            }
            // ⚑⚑⚑ STAGE E, AND IT GOES *HERE* RATHER THAN IN `digestOf` ABOVE
            // BECAUSE OF WHAT STAGE D FOUND ONE STAGE AGO. The structure digest
            // runs over a galaxy straight out of `sol::sim::generateGalaxy`,
            // which no game-side pass has touched - so a `shadowOwner` folded
            // into it would hash `kNoFaction` 125 times and move the golden
            // exactly once, for the field's existence rather than its contents.
            // That is precisely how stage B's composition line went blind for a
            // whole stage. This digest reads the world's own composed galaxy,
            // which is the only place an operator is ever written.
            //
            // ⚑ An INDEX rather than a name, unlike the module ids beside it,
            // and the difference is not an oversight: a faction index is what
            // the structure digest already hashes for `system.factionIndex`, and
            // it is stable under def edits in a way a module index would not be
            // - majors come from def order and clans from galaxy order, so this
            // number moving means the galaxy moved.
            digest = hashCombine(digest, system.stations[t].shadowOwner);
            shadowed += system.stations[t].shadowOwner != sol::sim::kNoFaction ? 1 : 0;

            // ⚑⚑⚑ STAGE C, AND IT GOES HERE FOR STAGE E'S REASON RESTATED
            // RATHER THAN RE-DERIVED. Who is in a room is written by a pass
            // inside `composeStations`, so the structure digest - which reads a
            // galaxy straight out of `sol::sim::generateGalaxy` - would hash 125
            // empty seats and move exactly once, for the field existing. That is
            // precisely how stage B's composition line went blind for a whole
            // stage. ⚑ The NAME rather than an index, because a regular's name
            // exists in no def at all and is the only thing that says the
            // seating actually ran; a `[[character]]` id beside it so that
            // re-wording a name in `characters.toml` is not confused with
            // somebody moving.
            const game::SpaceWorld::CastSeat* seat = world.stationCast(s, t);
            if (seat != nullptr) {
                ++seated;
                digest = combineText(digest, seat->name);
                if (seat->character < defs.characters().size()) {
                    ++cast;
                    digest = combineText(digest, defs.characters()[seat->character].id);
                }
            }
        }
    }

    // ⚑⚑⚑ THE ANTI-VACUITY GUARD, WHICH IS THE HALF THIS FILE WAS MISSING. A
    // digest over a galaxy with no compositions in it is a stable number that
    // proves nothing, and it passed for a whole stage. If this ever reads zero
    // again, the digest below is measuring the absence of the feature.
    std::printf("  %zu station(s), %zu composed, %zu distinct composition(s)\n",
                stations,
                composed,
                world.compositionCount());
    SOL_REQUIRE(composed > 0);
    SOL_CHECK(composed == stations);
    SOL_CHECK(world.compositionCount() > 1);

    // ⚑⚑⚑ THE SAME GUARD AGAIN, FOR THE FIELD STAGE E ADDED, AND IT LIVES HERE
    // RATHER THAN IN `station_shadow_tests.cpp` DELIBERATELY. Stage D's finding
    // was not "the composition digest was wrong" - it was that the anti-vacuity
    // check lived in a different file from the hash, so the hash could be
    // measuring an empty galaxy while a green suite elsewhere measured a full
    // one. A digest and the proof that it read something belong in one place.
    std::printf("  %zu station(s) name a shadow operator\n", shadowed);
    SOL_REQUIRE(shadowed > 0);
    SOL_CHECK(shadowed < stations); // and the field is not simply set everywhere

    // ⚑⚑⚑ AND THE SAME GUARD A THIRD TIME, FOR THE FIELD STAGE C ADDED. It is
    // the strongest of the three, because it can fail in two directions that
    // mean different things: nobody seated at all is a placement pass that did
    // not run, and NO AUTHORED CAST is `characters.toml` failing to be read
    // while 62 generated regulars make the digest look perfectly healthy. The
    // second is exactly the shape of failure this file exists to catch.
    std::printf("  %zu room(s) have somebody in them, %zu of them written by hand\n", seated, cast);
    SOL_REQUIRE(seated > 0);
    SOL_REQUIRE(cast > 0);
    SOL_CHECK(cast < seated); // and the cast has not quietly become the whole galaxy

    SOL_CHECK(checkDigest("composition", digest, kGoldenComposition));
}

// The other half, scoped to the toolchain it is true of.
SOL_TEST(shipped_seed_galaxy_keeps_its_recorded_geometry_on_a_known_libm)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    const StrippedGalaxy stripped = generateWithoutAuthoredContent(defs, world);
    SOL_REQUIRE(stripped.authoredSystems + stripped.constellations > 0);
    const Digests d = digestOf(stripped.galaxy);

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
    const StrippedGalaxy stripped = generateWithoutAuthoredContent(defs, world);
    const Digests base = digestOf(stripped.galaxy);

    Galaxy mutated = stripped.galaxy;
    SOL_REQUIRE(mutated.systems.size() >= 2);
    SOL_REQUIRE(!mutated.links.empty());
    SOL_REQUIRE(!mutated.clans.empty());
    SOL_REQUIRE(!mutated.systems[1].planets.empty());

    mutated.systems[1].name += "x";
    SOL_CHECK(digestOf(mutated).structure != base.structure);

    mutated = stripped.galaxy;
    mutated.links.pop_back();
    SOL_CHECK(digestOf(mutated).structure != base.structure);

    mutated = stripped.galaxy;
    mutated.clans[0].homeSystem += 1;
    SOL_CHECK(digestOf(mutated).structure != base.structure);

    // The two that must move ONE number each: geometry is not allowed to leak
    // into the digest that other platforms are held to.
    mutated = stripped.galaxy;
    mutated.systems[1].mapPosition.y += 1.0f;
    SOL_CHECK(digestOf(mutated).mapGeometry != base.mapGeometry);
    SOL_CHECK(digestOf(mutated).structure == base.structure);

    mutated = stripped.galaxy;
    mutated.systems[1].planets[0].position.z += 1.0;
    SOL_CHECK(digestOf(mutated).systemGeometry != base.systemGeometry);
    SOL_CHECK(digestOf(mutated).structure == base.structure);
}
