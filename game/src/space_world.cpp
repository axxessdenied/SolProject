#include "space_world.hpp"

#include "game_audio.hpp"
#include "model_roles.hpp"

#include "sol/assets/loadout.hpp"
#include "sol/core/hash.hpp"
#include "sol/core/log.hpp"
#include "sol/core/profiler.hpp"
#include "sol/core/random.hpp"
#include "sol/core/serialize.hpp"
#include "sol/ecs/snapshot.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/platform/time.hpp"
#include "sol/sim/collision.hpp"
#include "sol/sim/pilot_tips.hpp"
#include "sol/sim/predation.hpp"
#include "sol/sim/steering.hpp"
#include "sol/sim/trade_route.hpp"
#include "sol/sim/weapons.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace game {

using namespace sol;

namespace {

// Impact damage: k * v^2 (50 m/s ram = 25 damage); scrapes below ~10 m/s
// are ignored so docking bumps stay free.
constexpr double kImpactDamageFactor = 0.01;
constexpr double kImpactDamageMinimum = 1.0;

// Save format: header (magic, version, universe seed, current system) then
// the ECS snapshot. Bump the version on any layout change; old saves are
// rejected cleanly by the magic/version check.
constexpr std::uint32_t kSaveMagic = 0x37'4c'4f'53u; // "SOL7"
// v15: station archetypes now consult the system's asteroid fields and its
// owner's character (Phase 13), so the same seed describes a different galaxy.
// No migration, per precedent — a v14 save is rejected cleanly.
//
// v16 (Phase 27): a self-describing header - display name, wall-clock stamp
// and resolved system name - written immediately after the version so a save
// browser can build a row without restoring the world. No migration, per the
// same precedent: a v15 save is rejected cleanly.
//
// v17 (Phase 29 stage D): the authored-content digest, beside the seed.
// v19 (Phase 31 stage C1): guns became plural - `ShipArmament` replaced the
// single `ShipWeapon`, under a new component id.
// v20 (Phase 31 stage C2): a gun carries its mount's `aim` and `arc`, so the
// `ShipArmament` block is wider. Same component id: the component did not
// change identity, only size, and the version check is what stops a v19 file
// being read into it.
//
// v21 (Phase 31 stage C3): fire groups. TWO blocks moved at once - a gun
// carries the group it answers to and its mount index, and the fleet's saved
// fittings carry the durable copy of that group beside the def id. The
// component id is 23 still, for C2's reason.
//
// v22 (Phase 31 stage E): a gun carries the model it is DRAWN as, so the
// `ShipArmament` block is wider again. Component id 23 still, for C2's reason:
// the component changed size, not identity, and the version check at the
// header is what stops a v21 file being read into the new layout.
//
// v23 (Phase 31 stage E2): `ShipFittings` - what a hull carries on its outside
// that is NOT a gun - under a NEW component id 24. New rather than grown,
// because this is a different component and not a wider one.
//
// v24 (Phase 31 stage F1): `ShipMounts` - every place on the hull and how much
// of it is left - under a NEW component id 25, on E2's rule. It is a third
// thing rather than a wider `ShipFittings` for a reason the two of them make
// plain: `ShipFittings` holds only what is DRAWN, and a mount's condition is
// about mounts nobody draws and mounts nothing is fitted to.
//
// v25 (Phase 31 stage F2): a mount carries its KIND, so the `ShipMounts` block
// is wider. Component id 25 still, on C2's rule: the component changed size
// and not identity, and the version check at the header is what stops a v24
// file being read into the new layout.
//
// v26 (Phase 33 stage B): the commodity table went from four goods to seven,
// so every `StationMarket::stock` row in the save is three floats wider. ⚑⚑ The
// block is written FLAT - a market count and then every market's stock run
// back to back, with no per-market length - so a v25 file read at the new
// width does not fail, it silently reads market 1's food as market 0's alloy
// and carries on. The version check at the header is the ONLY thing between a
// player and a galaxy of nonsense here, which is exactly the case standing risk
// 1 predicted this arc would hit six times.
// v27 (Phase 33 stage C): an eighth good, `sol.salvage`, so every
// `StationMarket::stock` row is one float wider again - v26's note applies
// unchanged and is the reason this is a version bump rather than a migration.
// ⚑ The wreck and signal loot tables in a v26 save are ALSO wrong now and not
// because of their width: a saved wreck still names `sol.ore` as its scrap and
// a saved signal still holds a commodity drawn from a uniform roll, both of
// which are indices into a table that has grown. There is nothing to migrate
// them to that would be truer than a fresh galaxy.
// v28 (Phase 33 stage E): a ninth good, `sol.hull_section`, and the same flat
// market block one float wider again. ⚑ This bump carries something the width
// alone does not: two new archetypes were added and the generator RESAMPLED
// every station in the galaxy, so a v27 save's market indices name stations
// that are now doing a different job. The galaxy is the same place - the golden
// geometry digests are untouched - but which building is a Refinery has moved,
// which is precisely the thing a save cannot be migrated through.
// v29 (the Hegemony bloc): no new commodity, and the bump is entirely about
// the RESAMPLE. Seven authored systems changed hands and the T3 weights moved
// with them, so a v28 save's market indices name stations now doing a
// different job - the same reason v28 gave, arrived at from content rather
// than from an archetype. ⚑ `authoredContentDigest` would refuse those saves
// anyway, because `[[system]]` rows are exactly what it hashes; the version is
// bumped regardless, because relying on a second guard that happens to fire is
// weaker than stating the rule that made the break.
// v30 (Phase 34 stage B): station COMPOSITIONS. Every station in the galaxy now
// runs on the rates its own rolled module list adds up to rather than on its
// archetype's, so a v29 save's market indices name stations doing a different
// job for a third distinct reason - not a wider row, not a resampled mix, but
// the same station composed. ⚑⚑ THE COMPOSITION ITSELF IS NOT IN THE FILE AND
// MUST NOT BE: the galaxy is regenerated from the seed on load and the
// composer runs again with it, which is the same bargain `initializeFactions`
// and the economy layout already make. What breaks a v29 save is that its stock
// row per market was recorded against a market whose rates no longer exist.
// v31 (Phase 34 stage C): station SERVICES follow the modules too, and this is
// the narrowest of the four breaks rather than the widest - which is exactly
// why it is worth stating. Nothing about the galaxy moved: the composer takes
// no new draw, both geometry digests and the structure digest are untouched,
// and every station is composed of what it was composed of in v30. What moved
// is who ANSWERS for a service. Refining came off `[[station]]` and onto
// `sol.mod_refinery_service`, which sits in the Refinery's recipe at chance
// 0.85, so two of the galaxy's twelve refineries have the production line and
// not the counter - and a v30 save with refined metal waiting at one of those
// two could never collect it, because the tab that collects it is no longer
// drawn there. The mission board moved the same way. ⚑⚑ A save is broken by a
// station that no longer does a job the save is MID-WAY THROUGH, not only by
// one whose rates changed; that is a fourth distinct reason for a bump and the
// first that is about an outstanding order rather than about a market row.
// v32 (Phase 34 stage D): stock capacity is PER COMMODITY. Every saved market
// row is a float count of units, and what those units meant was a fraction of
// one number that every station shared; now it is a fraction of whatever holds
// that station was composed with, and for two goods of the nine it can be a
// fraction of nothing at all. ⚑⚑ A v31 save restored into this build would put
// stock into markets that have no room for it - 16 stations hold no food and 82
// hold no salvage - and the first tick would clamp it away, silently, as though
// the galaxy had been robbed. ⚑ The capacities are NOT in the file and must not
// be: they are derived from the composition, which is derived from the seed.
// v33 (Phase 35 stage C): the cast. A new trailing section - who the player has
// talked to in a bar, how often, and how they feel about it - and a v32 save
// simply stops before it, so a reader would run off the end of the stream and
// into the entity snapshot. ⚑ WHO IS IN WHICH ROOM IS NOT IN THE FILE and must
// not be: it is derived from the seed through `composeStations` exactly like
// the composition and the shadow operator, and a saved seating chart would be a
// second copy of something the generator already knows.
//
// ⚑⚑⚑ AND A `[[character]]` ROW IS AUTHORED CONTENT THAT IS DELIBERATELY *NOT*
// IN `m_authoredDigest`, WHICH IS A DECISION RATHER THAN AN OVERSIGHT. The
// digest exists to catch a mod that makes a save's SYSTEM INDEX point somewhere
// else entirely - that is what `digestAuthoredSystem` guards and why it hashes
// `[[system]]` rows and nothing else. A cast row moves no index: it changes who
// is behind one bar. Folding it in would refuse every save in existence the day
// somebody adds a barkeep, for a change that cannot corrupt anything.
//
// ⚑ The cost, stated rather than discovered later: a new `[[character]]` takes
// a seat a REGULAR had, and a regular's memory is keyed by the seat - so a
// player who had drunk there twice meets the new arrival already knowing them.
// That is a stale visit count on one dock against refusing every save on the
// disk, and it is not close.
//
// ⚑⚑ `regard` IS WRITTEN AND NOT YET READ, WHICH IS DELIBERATE AND IS RECORDED
// HERE RATHER THAN HIDDEN. Stage D makes a bar a second posting facility and
// has to keep an informal lead distinguishable from a board contract; a
// relationship is what does that, and it cannot gate anything until it has been
// accumulating. The alternative was bumping this number twice for one feature.
// v35 (Phase 37 stage A): two new commodities and a fourth goods class. A saved
// market row is a float count of units held against a PER-COMMODITY capacity
// (that is v32's own reason), and both the market vectors and the player's hold
// are indexed by commodity - so a v34 save read into this build has vectors one
// size short of what every reader now expects. ⚑ This is the same shape as v32
// rather than a new one, which is worth saying plainly: the row count moved,
// and every number in the file that was addressed by commodity index is now
// addressed against a longer list. ⚑⚑ THE CAPACITIES THEMSELVES ARE STILL NOT
// IN THE FILE and must not be - illicit capacity is derived from whether a
// station composed a shadow module, which is derived from the seed, exactly
// like every other capacity since v32.
// v36 (Phase 37 stage B): the shadow faction. This one is NOT about a vector
// getting longer - the faction table is rebuilt from the defs on load, not read
// from the file - it is about `MissionSim::load`, which writes the faction count
// into its own block and REFUSES a save whose count does not match
// (`missions.cpp:795`, "galaxy/defs mismatch"). A v35 save read into this build
// would fail there, deep inside a section, rather than at the version check
// where a player gets a sentence about it. ⚑ Bumping is what turns a confusing
// failure into an honest one; it does not create the incompatibility.
//
// ⚑⚑ AND THE FACTION TABLE ITSELF IS STILL NOT IN THE FILE, which is the same
// rule the composition and the shadow operator follow: it is derived from the
// defs and the seed. What IS in the file is every index that points at it, and
// this stage appends the new rows LAST precisely so that every one of those
// indices - majors at their generator positions, clans at `factionCount +
// clanIndex` - still means what it meant when it was written.
// ⚑⚑ v37 (Phase 38 stage A): the world is a LIST of registries now, each
// tagged with the system whose frame its positions are in, rather than one
// registry whose frame was implied. v36 files are refused whole at the header,
// as every earlier version has been - there is no migration and never has been.
// ⚑⚑ v38 (Phase 38 stage C): each of those registries now says how long it has
// left. Stage A wrote the list and stage C is the stage that puts more than one
// thing in it, so the retention clock rides beside the system index - the
// smallest field this phase could have added, and the one without which a save
// taken mid-retreat loads a world that discards its cooling bubbles on the
// first tick.
// ⚑⚑ v39 (Phase 39 stage A): the captains in your employ. A person you
// hired is the one thing about them that is NOT derivable - who is standing
// in a crew hall is a pure function of the dock and the seed, and is
// re-rolled on load exactly as the cast's seating is - so this writes
// `m_captains` and nothing else. The hall filters against it, which is why
// there is no second list of who has been taken.
// v40 (Phase 39 stage B): what you TOLD a captain, and what they are doing
// about it. The order is two market indices - safe in a save for exactly the
// reason `MarketMemory::market` already is, since the market table is one row
// per station in system-then-station order over a galaxy the content digest
// refuses to let change under a file - and the haul beside it is the leg
// clock, because a save taken mid-run has to load into the same haul rather
// than teleporting a laden hull to one end of it.
// v43 (Phase 39 stage E): the sell floor on a haul order - the margin over the
// hold's cost a captain will not sell under. A field with a meaningful zero, so
// a v42 reader would not have crashed on a v43 file; it would have done
// something worse, which is silently drop every floor the player set and start
// dumping their cargo at whatever the market offered. That is the case the
// version number exists for.
constexpr std::uint32_t kSaveVersion = 45;

// ---------------------------------------------------------------------------
// ⚑⚑⚑⚑ WHAT THE AUTHORED HALF OF THIS GALAXY WAS MADE OF, IN EIGHT BYTES
// (Phase 29 stage D, decisions/018 decision 7).
//
// A galaxy is not saved - it is regenerated from the seed on load, and
// `galaxyChanged` keys on the seed alone. That was sound while the seed was
// the only input. It stopped being sound the moment a MOD could change the
// galaxy: a player who installs one mid-campaign reloads into a world that has
// silently reshaped around them, their save's system index now pointing at
// somewhere else entirely, and nothing anywhere says so. A content version
// bump cannot see it either, because the mod is not the build.
//
// So the authored input is digested and the digest rides the save beside the
// seed. A mismatch is refused, which turns silent corruption into the same
// clean rejection a save from another version already gets.
//
// ⚑⚑ IT HASHES THE INPUT, NOT THE OUTPUT, AND THAT IS THE CHEAP HALF OF THE
// BARGAIN. Digesting the generated galaxy would answer the same question and
// would cost a full generation at load time before the answer arrived; the
// authored rows are a few dozen strings, are already in hand in
// `m_galaxyParams`, and are the only thing that can differ at a fixed seed.
// Everything else that shapes a galaxy - faction count, station archetypes,
// their weights - is already covered by the version check, because changing
// any of it means changing the build.
//
// ⚑ Deliberately covers the RESOLVED input rather than the file bytes: a
// comment rewritten in `systems.toml` is not a different galaxy, and a save
// that refused over one would be a worse instrument than none.
[[nodiscard]] std::uint64_t digestAuthoredSystem(std::uint64_t seed, const sim::AuthoredSystem& authored)
{
    seed = core::fnv1a(authored.id, seed);
    seed = core::hashCombine(seed, static_cast<std::uint64_t>(authored.placement));
    seed = core::hashCombine(seed, authored.atFactionCapital);
    seed = core::fnv1a(authored.anchorId, seed);
    seed = core::hashCombine(seed, authored.jumpsMin);
    seed = core::hashCombine(seed, authored.jumpsMax);
    seed = core::fnv1a(authored.name, seed);
    seed = core::hashCombine(seed, authored.hasName ? 1u : 0u);
    seed = core::hashCombine(seed, static_cast<std::uint64_t>(authored.region));
    seed = core::hashCombine(seed, authored.hasRegion ? 1u : 0u);
    seed = core::hashCombine(seed, authored.factionIndex);
    seed = core::hashCombine(seed, authored.hasFaction ? 1u : 0u);
    seed = core::hashCombine(seed, authored.primaryPlanet);
    seed = core::hashCombine(seed, authored.hasPrimaryPlanet ? 1u : 0u);
    seed = core::hashCombine(seed, authored.secret ? 1u : 0u);
    // ⚑⚑⚑⚑ FOLDED IN ONLY WHEN AN AUTHOR ACTUALLY WROTE ONE, AND THAT IS A
    // RULE FOR EVERY FIELD ADDED TO AN AUTHORED ROW AFTER THIS - NOT A
    // CONVENIENCE FOR THIS ONE. `security` is the first field to join
    // `AuthoredSystem` since the digest was built, and hashing it
    // unconditionally would move the digest of content NOBODY CHANGED: every
    // save in existence would be refused, with a message saying a [[system]]
    // was added, changed or removed, about a file that was not touched.
    //
    // The two halves of the bargain are stated at the top of this block and
    // they draw the line exactly here: the DIGEST covers what an author wrote,
    // and the save VERSION covers what the build changed. A field nobody wrote
    // is a build change, so it belongs to the version - and a digest that
    // reported it would be answering a question that is not its own, in words
    // that are false. Skipping it is what keeps `kSaveVersion 17` honest
    // through a stage that adds a key.
    if (authored.hasSecurity) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &authored.security, sizeof(bits));
        seed = core::hashCombine(seed, bits);
    }
    seed = core::hashCombine(seed, authored.planets.size());
    for (const sim::AuthoredPlanet& planet : authored.planets) {
        seed = core::fnv1a(planet.name, seed);
        // ⚑ Bits rather than the double: a radius is the only float an author
        // writes, and this digest is compared against one another machine
        // wrote. Reading it as an integer is exact everywhere.
        std::uint64_t bits = 0;
        std::memcpy(&bits, &planet.radius, sizeof(bits));
        seed = core::hashCombine(seed, bits);
        seed = core::hashCombine(seed, planet.hasRadius ? 1u : 0u);
    }
    seed = core::hashCombine(seed, authored.stations.size());
    for (const sim::AuthoredStation& station : authored.stations) {
        seed = core::fnv1a(station.name, seed);
        seed = core::hashCombine(seed, station.archetype);
    }
    return seed;
}

[[nodiscard]] std::uint64_t authoredContentDigestOf(const sim::GalaxyParams& params)
{
    std::uint64_t seed = core::kFnvOffsetBasis;
    // The COUNTS are folded in as well as the rows, so that removing the last
    // authored system is a different digest from never having had one.
    seed = core::hashCombine(seed, params.authoredSystems.size());
    for (const sim::AuthoredSystem& authored : params.authoredSystems) {
        seed = digestAuthoredSystem(seed, authored);
    }
    seed = core::hashCombine(seed, params.constellations.size());
    for (const sim::AuthoredConstellation& constellation : params.constellations) {
        seed = core::fnv1a(constellation.id, seed);
        seed = core::hashCombine(seed, constellation.members.size());
        for (const sim::AuthoredSystem& member : constellation.members) {
            seed = digestAuthoredSystem(seed, member);
        }
        seed = core::hashCombine(seed, constellation.links.size());
        for (const sim::AuthoredConstellationLink& link : constellation.links) {
            seed = core::hashCombine(core::hashCombine(seed, link.a), link.b);
        }
    }
    return seed;
}

// Market intel (Phase 8g): what a station's market report covers and costs.
// Deliberately shorter than the traders' own horizon — a station's brokers
// know their neighbourhood, not everywhere their freighters reach, and a
// report that covered the whole reachable galaxy would end scouting rather
// than reward it.
constexpr std::uint32_t kIntelJumpRadius = 3;
constexpr double kIntelBasePrice = 120.0;
constexpr double kIntelPricePerMarket = 18.0;
constexpr std::uint8_t kUnreachableHops = 0xff;

// Hop counts from one system over the gate graph, capped. One BFS instead of
// a routeBetween() per market — this runs over every market in the galaxy.
void hopsFrom(const sim::Galaxy& galaxy,
              std::uint32_t from,
              std::uint32_t maxHops,
              std::vector<std::uint8_t>& out)
{
    out.assign(galaxy.systems.size(), kUnreachableHops);
    if (from >= galaxy.systems.size()) {
        return;
    }
    out[from] = 0;
    std::vector<std::uint32_t> frontier{from};
    std::vector<std::uint32_t> next;
    for (std::uint8_t depth = 1; depth <= maxHops && !frontier.empty(); ++depth) {
        next.clear();
        for (const std::uint32_t index : frontier) {
            for (const sim::GateSpec& gate : galaxy.systems[index].gates) {
                if (out[gate.toSystem] == kUnreachableHops) {
                    out[gate.toSystem] = depth;
                    next.push_back(gate.toSystem);
                }
            }
        }
        frontier.swap(next);
    }
}

// A ship's name as a radio callsign: "Freighter (Solar Navy)" -> "Freighter".
// Target names carry their faction since Phase 8b, which is right on the target
// readout and wrong in the comms panel's sender column - it is the SAME
// repetition 8r had to strip from the docking lines, one item later. The
// faction is still on screen: the target panel names it twice, an inch to the
// right of whoever is talking.
[[nodiscard]] std::string radioName(const std::string& shipName)
{
    const std::size_t open = shipName.find(" (");
    return open == std::string::npos ? shipName : shipName.substr(0, open);
}

} // namespace

// What Lua calls a pilot's job. Shared by the pilot_think hook and Phase 8s's
// pilot_hail, in one place so the two can never disagree about what a "trader"
// is - a hook that branches on the string would break silently if they did.
//
// ⚑⚑ LIFTED OUT OF THE ANONYMOUS NAMESPACE IN PHASE 37 STAGE D, because the
// sentence above turned out to be a claim a test could check and could not
// reach. `pilot_think` is an `if/elseif` chain with no else, so a role the
// script has never heard of makes its pilot do NOTHING - silently, and looking
// exactly like a calm ship. `every_pilot_role_the_engine_can_name_has_a_branch_
// in_the_shipped_script` reads this function for the vocabulary rather than
// listing the words, so a fifth role fails there until the script learns it.
const char* pilotRoleName(PilotRole role)
{
    switch (role) {
    case PilotRole::Fighter:
        return "fighter";
    case PilotRole::Trader:
        return "trader";
    case PilotRole::Patrol:
        return "patrol";
    case PilotRole::Covert:
        return "covert";
    }
    return "fighter";
}

namespace {

constexpr double kCollisionRestitution = 0.15;

// Rotation taking the model's +Z onto `axis` (Phase 8w). The gate slab is thin
// on Z, so this turns its face to the lane it serves. Both ends of the
// degenerate case matter: already facing, and facing exactly backwards, where
// the cross product is zero and any perpendicular axis will do.
[[nodiscard]] core::Quat facingRotation(const core::DVec3& axis)
{
    const core::Vec3 to = core::toVec3(axis);
    const core::Vec3 from{0.0f, 0.0f, 1.0f};
    const float alignment = dot(from, to);
    if (alignment > 0.9999f) {
        return core::Quat::identity();
    }
    if (alignment < -0.9999f) {
        return core::fromAxisAngle({0.0f, 1.0f, 0.0f}, 3.14159265358979323846f);
    }
    return core::fromAxisAngle(normalize(cross(from, to)), std::acos(core::clamp(alignment, -1.0f, 1.0f)));
}

// Stable component ids for the save format; never reuse or renumber. Ids 1-3
// belonged to the retired Phase 3 swarm-world format.
ecs::Snapshot makeSnapshotSchema()
{
    ecs::Snapshot schema;
    schema.component<Transform>(10);
    schema.component<FlightBody>(11);
    schema.component<RenderShape>(12);
    schema.component<PlayerShip>(13);
    schema.component<ShipControl>(14);
    schema.component<ShipPower>(15);
    schema.component<ShipDefense>(16);
    schema.component<Projectile>(17);
    // 18 was ShipWeapon, the ship's ONE gun, retired in Phase 31 stage C1
    // when guns became plural. Retired rather than reused: the id is a
    // promise about a layout, and 23 below is a different one.
    schema.component<ShipPilot>(19);
    schema.component<MineableRock>(20);
    schema.component<WreckMarker>(21);
    schema.component<OreChunk>(22);
    // 23 stays 23 through v20, which GREW `ShipWeapon` (`aim` and `arc`)
    // rather than replacing it. An id is a promise about a layout and the
    // save VERSION is what keeps that promise: a v19 file is refused whole at
    // the header, so no reader ever meets the old layout under this id. 18
    // was retired instead because the component behind it stopped existing.
    schema.component<ShipArmament>(23); // v19: what replaced 18
    // ⚑ A NEW ID RATHER THAN A WIDER 23, because this is a different
    // component and not a bigger one. 23 grew twice (C2's `aim`/`arc`, E1's
    // model) and kept its id both times on the rule that an id promises a
    // LAYOUT and the save version keeps the promise; adding a second, separate
    // thing a ship carries is the case that rule does not cover.
    schema.component<ShipFittings>(24); // v23: Phase 31 stage E2
    // ⚑ AND A THIRD, for the same reason 24 was not a wider 23. Condition is
    // about every mount a hull declares - including the ones nothing is fitted
    // to and the ones nothing is drawn at - so it cannot ride either of the
    // two components above, both of which are lists of what a hull CARRIES.
    schema.component<ShipMounts>(25); // v24: Phase 31 stage F
    return schema;
}

// A named model, or the fallback for its kind. Phase 9 made this a def lookup;
// it used to resolve exactly three strings against a five-member enum, which is
// why a mesh could be authored and cooked and still have no way into the game.
// ⚑ The context and the fallback are ARGUMENTS since stage H, because stations
// resolve through here too. Hardcoding them was harmless while a ship def was
// the only caller and is not: the message would name the wrong def kind, and
// falling back to a shuttle where a station belongs reads as a spawn bug
// rather than as a bad name in a file.
//
// ⚑⚑ AND SINCE PHASE 19 THE FALLBACK IS A ROLE, NOT A MODEL NAME. Stage H
// moved these two literals out of this function and into its call sites, which
// looks like a generalisation and removed nothing - "station" and "ship" were
// still model names compiled into the game, and they are two of the four this
// phase found beyond the six that were recorded.
//
// ⚑ This keeps WARN-AND-FALL-BACK where `validateRoles` refuses, and the
// difference is whose mistake it is: a def naming a bad model is one broken
// ship in a file a modder can fix, while an unfilled role is the game having
// no answer at all.
ModelId modelIdFromName(const assets::DefDatabase& defs,
                        const std::string& name,
                        const char* context,
                        const char* fallbackRole)
{
    const std::uint32_t index = defs.modelIndex(name.c_str());
    if (index != assets::DefDatabase::kNoModel) {
        return static_cast<ModelId>(index);
    }
    const std::uint32_t fallback = defs.roleModelIndex(fallbackRole);
    SOL_LOG_WARN("unknown model '%s' in %s; using the '%s' role", name.c_str(), context, fallbackRole);
    return fallback == assets::DefDatabase::kNoModel ? kNoModel : static_cast<ModelId>(fallback);
}

// ⚑⚑ WHAT A FITTING IS DRAWN AS, AND THE ONE PLACE THE EMPTY CASE IS
// DECIDED (Phase 31 stage E). It is deliberately NOT `modelOverrideOr`, and
// the difference is the whole rule: an unset OVERRIDE means "whatever the role
// says", because a rock with no override is still a rock and has to be drawn
// as something. An unset fitting model means NOT DRAWN.
//
// A bare hardpoint is the honest picture of kit nobody has authored a mesh
// for, and it is what every gun in this game looked like before this stage. A
// fallback box would put a grey crate on every NPC freighter in the galaxy the
// moment somebody shipped a mod weapon without art - which is a change to
// their ship made by our default, not by their file.
//
// ⚑ A name that resolves to NOTHING is the other case and does fall back, to
// the `fitting` role, because that one is an author's mistake and a mistake
// should be visible. `modelIdFromName` warns and names the def.
//
// ⚑ No unit-radius check: a fitting is drawn at the HULL's scale, which is the
// scale `at` is already multiplied by, so its mesh is authored at real size
// exactly as the gate and the cockpit are.
[[nodiscard]] ModelId
fittingModelOf(const assets::DefDatabase& defs, const std::string& name, const char* context)
{
    if (name.empty()) {
        return kNoModel;
    }
    return modelIdFromName(defs, name, context, kRoleFitting);
}

// An optional per-def model override, or the role that backs it (Phase 19).
// Empty is the normal case and every shipped def takes it, which is what makes
// adding these keys a no-op: the world draws exactly what the role says until
// somebody writes a name in.
//
// ⚑ `unitRadius` is the contract from `model_roles.hpp` arriving where it can
// actually be broken. A role's model is pinned by a test against committed
// data; an OVERRIDE is written by whoever is editing weapons.toml or
// commodities.toml, and no test of ours will ever see their file. A rock is
// drawn at a scale that IS its radius in metres, so a model of some other
// radius resizes every instance and its mining hit sphere at once - with
// nothing on screen to say why, which is the only reason this warns at all.
} // namespace

ModelId modelOverrideOr(const assets::DefDatabase& defs,
                        const std::string& name,
                        const char* context,
                        const char* role,
                        bool unitRadius)
{
    ModelId resolved = kNoModel;
    // ⚑ THE EMPTY BRANCH IS ABOUT THE LOG, NOT THE ANSWER, AND SAYING SO IS
    // THE POINT. Deleting it leaves every result identical, because
    // `modelIdFromName("")` also finds nothing and also falls back to the
    // role - the two paths converge by construction. What it would cost is a
    // WARNING for every unset override on every system load, i.e. the normal
    // case shouting about itself. A mutation proved the results identical and
    // green, so the test that guards this asserts the log rather than the
    // return value; anything else here is tautological.
    if (name.empty()) {
        const std::uint32_t index = defs.roleModelIndex(role);
        resolved = index == assets::DefDatabase::kNoModel ? kNoModel : static_cast<ModelId>(index);
    } else {
        resolved = modelIdFromName(defs, name, context, role);
    }
    if (unitRadius && !name.empty() && resolved != kNoModel) {
        const assets::ModelDef& model = defs.models()[modelIndex(resolved)];
        if (model.radius != 1.0f) {
            SOL_LOG_WARN("%s names model '%s' with radius %.3f; this slot is drawn at a scale "
                         "that means metres, so it wants a model authored at radius 1",
                         context,
                         model.id.c_str(),
                         static_cast<double>(model.radius));
        }
    }
    return resolved;
}

namespace {

sim::ShipTuning toShipTuning(const assets::ShipFlightTuning& flight)
{
    return {
        .forwardAccel = flight.forwardAccel,
        .reverseAccel = flight.reverseAccel,
        .lateralAccel = flight.lateralAccel,
        .verticalAccel = flight.verticalAccel,
        .maxSpeed = flight.maxSpeed,
        .maxTurnRate = {flight.maxTurnRate[0], flight.maxTurnRate[1], flight.maxTurnRate[2]},
        .angularAccel = {flight.angularAccel[0], flight.angularAccel[1], flight.angularAccel[2]},
        .boostAccelScale = flight.boostAccelScale,
        .boostSpeedScale = flight.boostSpeedScale,
        .cruiseSpeedScale = flight.cruiseSpeedScale,
        .cruiseAccelScale = flight.cruiseAccelScale,
    };
}

} // namespace

void SpaceWorld::spawn(std::uint64_t universeSeed)
{
    m_universeSeed = universeSeed;

    // The first bubble, and the reason `playerRegistry()` may assume there is
    // always one: the player is created inside a system's frame and moves
    // between frames, and there is no moment at which they are in none.
    // `m_currentSystem` is 0 until a galaxy exists, exactly as it was.
    m_bubbles.clear();
    (void)openBubble(m_currentSystem);

    const ecs::Entity e = playerRegistry().create();
    playerRegistry().emplace<Transform>(e);
    playerRegistry().emplace<FlightBody>(e);
    // No model yet: the def database does not exist at construction, and
    // applyShipDef gives the player the hull its def names before the first
    // frame. An unset model draws nothing rather than the wrong thing.
    playerRegistry().emplace<RenderShape>(e, RenderShape{});
    playerRegistry().emplace<PlayerShip>(e);
    playerRegistry().emplace<ShipControl>(e);
    playerRegistry().emplace<ShipPower>(e);
    playerRegistry().emplace<ShipDefense>(e);
    playerRegistry().emplace<ShipArmament>(e);
    playerRegistry().emplace<ShipFittings>(e);
    playerRegistry().emplace<ShipMounts>(e);
}

bool SpaceWorld::generateUniverse(const assets::DefDatabase& defs)
{
    m_galaxyParams = sim::GalaxyParams{};
    m_galaxyParams.seed = m_universeSeed;
    // Majors claim territory; pirate defs are clan templates (Phase 8b).
    //
    // ⚑⚑⚑⚑ THIS WAS A TERNARY AND A TERNARY HAS NO THIRD ANSWER (Phase 37 stage
    // B). Written when `kind` had two values, it read "not a pirate" as "a
    // claimant" - so a `kind = "shadow"` def added with no other change would
    // have INCREMENTED `factionCount`, and `claimTerritory` hands out capitals
    // and territory by that number. The faction whose entire definition is
    // "claims nothing" would have been handed a capital and every system in the
    // galaxy would have been redistributed around it. The count is the
    // dangerous line in this stage, and the golden digest is the guard.
    for (const assets::FactionDef& faction : defs.factions()) {
        switch (faction.kind) {
        case assets::FactionKind::Major:
            m_galaxyParams.factionCount += 1;
            break;
        case assets::FactionKind::Pirate:
            m_galaxyParams.pirateTemplateCount += 1;
            break;
        case assets::FactionKind::Shadow:
            break; // claims nothing: counted by neither, and that is the point
        }
    }
    for (const assets::StationDef& station : defs.stations()) {
        sim::StationRule rule;
        rule.weight[0] = station.weightCore;
        rule.weight[1] = station.weightFrontier;
        rule.weight[2] = station.weightFringe;
        // The same def key the economy reads for `extracts` below, asked one
        // stage earlier: an archetype whose output comes out of the ground
        // needs rock under it before it is worth siting (Phase 13).
        rule.requiresField = station.producesFrom == "field";
        m_galaxyParams.stationRules.push_back(rule);
    }

    // What each faction BUILDS, resolved from station_bias (Phase 13). Rows are
    // in the generator's faction order, which for majors is their order among
    // the majors in def order — the same order claimTerritory hands out.
    //
    // ⚑ Pirate clans deliberately get no row and fall through at 1.0. Their
    // faction indices are factionCount + clan index and the clan COUNT is not
    // known until the galaxy is generated, so a row for them cannot exist yet;
    // and the two pirate templates describe raiders, not builders. Named as a
    // gap rather than half-built.
    //
    // ⚑⚑ A SHADOW FACTION IS SKIPPED FOR A DIFFERENT REASON AND THE ROWS ARE
    // POSITIONAL, WHICH IS WHY THE SKIP MATTERS (Phase 37 stage B). A clan has
    // no row because its index does not exist yet; a shadow faction has none
    // because it builds nothing anywhere - it owns no ground to build it on.
    // But `bias[majorIndex]` is addressed by position among the CLAIMANTS, so
    // letting a shadow def through would not merely add a dead row, it would
    // shift every major's row after it onto somebody else's territory.
    {
        const std::vector<assets::StationDef>& stationDefs = defs.stations();
        std::uint32_t majorIndex = 0;
        bool anyBias = false;
        std::vector<std::vector<float>> bias;
        for (const assets::FactionDef& faction : defs.factions()) {
            if (faction.kind != assets::FactionKind::Major) {
                continue;
            }
            std::vector<float> row(stationDefs.size(), 1.0f);
            for (const assets::StationBias& entry : faction.stationBias) {
                std::size_t archetype = stationDefs.size();
                for (std::size_t s = 0; s < stationDefs.size(); ++s) {
                    if (stationDefs[s].id == entry.stationId) {
                        archetype = s;
                        break;
                    }
                }
                if (archetype >= stationDefs.size()) {
                    // A mod may remove an archetype a base faction names, which
                    // is the same rule the economy's rate lists follow.
                    SOL_LOG_WARN("faction '%s': unknown station '%s' in station_bias",
                                 faction.id.c_str(),
                                 entry.stationId.c_str());
                    continue;
                }
                row[archetype] = entry.weight;
                anyBias = true;
            }
            bias.resize(majorIndex + 1);
            bias[majorIndex] = std::move(row);
            ++majorIndex;
        }
        // Left empty when nobody authored a character, so a galaxy with no
        // biases is bit-identical to one generated before this key existed.
        if (anyBias) {
            m_galaxyParams.factionStationBias = std::move(bias);
        }
    }

    // Authored systems (Phase 29). This is the translation point, and it does
    // the job it already does three times over just above: ids become indices
    // here so `sol::sim` never learns what a def is, exactly as `StationRule`
    // and `factionStationBias` already cross the same seam.
    //
    // ⚑ A faction index is its position among the MAJORS in def order, because
    // that is the order `claimTerritory` hands them out in - the same rule the
    // bias rows above are built on. Pirate defs are clan templates and are not
    // claimants, so they are skipped rather than counted.
    // A faction's position among the MAJORS in def order, or kNoFaction. Both
    // things an authored system can say about a faction - who owns it, and
    // whose capital it takes - resolve through this one rule, because it is
    // the rule `claimTerritory` hands capitals and territory out by.
    // ⚑ `!= Major` rather than `== Pirate` since Phase 37 stage B, for the
    // reason the bias rows above give: this counts positions among the
    // CLAIMANTS, and a shadow faction is not one.
    const auto majorIndexOf = [&defs](const std::string& id) {
        std::uint32_t majorIndex = 0;
        for (const assets::FactionDef& faction : defs.factions()) {
            if (faction.kind != assets::FactionKind::Major) {
                continue;
            }
            if (faction.id == id) {
                return majorIndex;
            }
            ++majorIndex;
        }
        return sim::kNoFaction;
    };

    // ⚑⚑ ONE TRANSLATION, TWO PLACES IT IS ASKED FOR (Phase 29 stage C). A
    // constellation member is a `SystemDef` that arrived nested rather than
    // top-level, and nothing about crossing the seam differs - so the rule is
    // written once here and a divergence between a `[[system]]` and a
    // `[[constellation.system]]` is not expressible.
    const auto translate = [&](const assets::SystemDef& def) {
        sim::AuthoredSystem authored;
        authored.id = def.id;
        authored.placement = def.placement == "anywhere"     ? sim::Placement::Anywhere
                             : def.placement == "at_system"  ? sim::Placement::AtSystem
                             : def.placement == "jumps_from" ? sim::Placement::JumpsFrom
                                                             : sim::Placement::Random;
        authored.anchorId = def.jumpsFromSystemId;
        authored.jumpsMin = def.jumpsFromMin;
        authored.jumpsMax = def.jumpsFromMax;
        authored.name = def.name;
        authored.hasName = def.hasName;
        authored.secret = def.secret;
        authored.primaryPlanet = def.primaryPlanet;
        authored.hasPrimaryPlanet = def.hasPrimaryPlanet;
        // A magnitude on both sides of the seam; the generator signs it from
        // whoever ends up holding the place (Phase 30 stage E).
        authored.security = def.security;
        authored.hasSecurity = def.hasSecurity;
        if (def.hasRegion) {
            authored.hasRegion = true;
            authored.region = def.region == "core"       ? sim::Region::Core
                              : def.region == "frontier" ? sim::Region::Frontier
                                                         : sim::Region::Fringe;
        }
        // ⚑⚑ TWO WAYS TO SAY WHO OWNS A PLACE, AND ONE OF THEM IS "NOBODY".
        // `lawless = true` is not the absence of `faction`: it is an authored
        // kNoFaction that `spawnClans` must leave alone, and the flag is the
        // only thing that can tell the two apart.
        if (def.lawless) {
            authored.hasFaction = true;
            authored.factionIndex = sim::kNoFaction;
        } else if (def.hasFaction) {
            const std::uint32_t majorIndex = majorIndexOf(def.factionId);
            if (majorIndex != sim::kNoFaction) {
                authored.hasFaction = true;
                authored.factionIndex = majorIndex;
            } else {
                // `validateSystems` refuses an unknown faction id before this
                // runs, so reaching here means the id names a PIRATE def -
                // a clan template, which claims nothing and cannot be a
                // system's owner.
                SOL_LOG_WARN("system '%s': faction '%s' is not a territory claimant; leaving it unowned",
                             def.id.c_str(),
                             def.factionId.c_str());
            }
        }
        // ⚑ `validateSystems` has already refused a non-major here, so this
        // resolves or the def never reached the generator. kNoFaction survives
        // as "no capital to take", which `placeAuthoredSystems` refuses by name.
        if (def.placement == "at_system") {
            authored.atFactionCapital = majorIndexOf(def.atSystemFactionId);
        }
        for (const assets::AuthoredPlanetDef& planet : def.planets) {
            authored.planets.push_back(
                {.name = planet.name, .radius = planet.radius, .hasRadius = planet.hasRadius});
        }
        for (const assets::AuthoredStationDef& station : def.stations) {
            std::uint32_t archetype = 0;
            for (std::size_t s = 0; s < defs.stations().size(); ++s) {
                if (defs.stations()[s].id == station.stationId) {
                    archetype = static_cast<std::uint32_t>(s);
                    break;
                }
            }
            authored.stations.push_back({.name = station.name, .archetype = archetype});
        }
        return authored;
    };

    for (const assets::SystemDef& def : defs.systems()) {
        m_galaxyParams.authoredSystems.push_back(translate(def));
    }
    // ⚑ The LANES cross the seam as member INDICES rather than as ids, for the
    // same reason a station crosses it as an archetype index: `sol::sim` has
    // never known what an id is. `validateSystems` has already refused a lane
    // naming something that is not a member of this group, so every id here
    // resolves - and a lane that somehow did not would be dropped by the
    // generator rather than indexed on trust.
    for (const assets::ConstellationDef& group : defs.constellations()) {
        sim::AuthoredConstellation constellation;
        constellation.id = group.id;
        for (const assets::SystemDef& member : group.members) {
            constellation.members.push_back(translate(member));
        }
        const auto memberIndexOf = [&group](const std::string& id) {
            for (std::size_t i = 0; i < group.members.size(); ++i) {
                if (group.members[i].id == id) {
                    return static_cast<std::uint32_t>(i);
                }
            }
            return sim::kNoSystem;
        };
        for (const assets::ConstellationLinkDef& link : group.links) {
            const std::uint32_t from = memberIndexOf(link.fromId);
            const std::uint32_t to = memberIndexOf(link.toId);
            if (from == sim::kNoSystem || to == sim::kNoSystem) {
                continue;
            }
            constellation.links.push_back({from, to});
        }
        m_galaxyParams.constellations.push_back(std::move(constellation));
    }

    // Economy: commodities + archetype rates from the defs (unknown
    // commodity ids in a rate list are warnings, not errors — a mod may
    // remove a commodity a base station references).
    //
    // ⚑ Built BEFORE the galaxy since Phase 13, and the order is load-bearing:
    // station placement now consults each system's asteroid fields, and what
    // is mineable is a pure function of the defs. Nothing here reads the
    // galaxy, so this is a reordering rather than a change.
    m_economyParams = sim::EconomyParams{};
    m_commodityIds.clear();
    m_commodityClass.clear();
    for (const assets::CommodityDef& commodity : defs.commodities()) {
        m_economyParams.commodities.push_back({.basePrice = commodity.basePrice});
        m_commodityIds.push_back(commodity.id);
        // ⚑ `goodsClass` is already `Bulk` when the author said nothing, so the
        // default is taken here by simply not asking. See `CommodityDef`: a
        // good outside the material tree is a real state and `hasTier` keeps
        // it, but a good nobody can warehouse anywhere is not.
        m_commodityClass.push_back(commodity.goodsClass);
    }
    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());

    buildMiningParams();
    // Taken from the translated params rather than from the defs, so what is
    // digested is exactly what the generator was handed - the same rule the
    // seam already follows for everything else that crosses it.
    m_authoredDigest = authoredContentDigestOf(m_galaxyParams);
    std::vector<sim::AuthoredPlacementFailure> placementFailures;
    m_galaxy = sim::generateGalaxy(m_galaxyParams, &m_miningParams, &placementFailures);

    // ⚑⚑⚑ THE REFUSAL IS COMPOSED HERE BECAUSE THIS IS THE ONLY LAYER THAT
    // KNOWS WHAT A FILE IS. `sol::sim` reported the id, the rule and the
    // reason; `SystemDef::source` supplies the file, which is the third thing
    // decision 3 says an error has to name. Refused rather than warned for the
    // `validateRoles` precedent: there is no fallback that is not a lie about
    // where the campaign starts.
    for (const sim::AuthoredPlacementFailure& failure : placementFailures) {
        const assets::SystemDef* def = defs.findSystem(failure.id.c_str());
        SOL_LOG_ERROR("%s: system '%s': placement \"%s\" found nowhere to go - %s",
                      def != nullptr ? def->source.c_str() : "<unknown source>",
                      failure.id.c_str(),
                      failure.rule.c_str(),
                      failure.reason.c_str());
    }
    if (!placementFailures.empty()) {
        return false;
    }

    for (const assets::StationDef& station : defs.stations()) {
        sim::EconomyArchetype archetype;
        archetype.production.assign(commodityCount, 0.0f);
        archetype.consumption.assign(commodityCount, 0.0f);
        archetype.feedstock.assign(commodityCount, 0.0f);
        archetype.setUniformCapacity(commodityCount, station.stockCapacity);
        const auto applyRates = [&](const std::vector<assets::StationRate>& rates, std::vector<float>& out) {
            for (const assets::StationRate& rate : rates) {
                const std::uint32_t index = commodityIndex(rate.commodityId.c_str());
                if (index < commodityCount) {
                    out[index] = rate.rate;
                } else {
                    SOL_LOG_WARN(
                        "station '%s': unknown commodity '%s'", station.id.c_str(), rate.commodityId.c_str());
                }
            }
        };
        applyRates(station.produces, archetype.production);
        applyRates(station.consumes, archetype.consumption);
        applyRates(station.feedstock, archetype.feedstock);
        // An archetype that mines is one whose output comes out of the ground
        // rather than off anyone's dock. The def says so; nothing here keys
        // off a hard-coded station id.
        archetype.extracts = station.producesFrom == "field";
        m_economyParams.archetypes.push_back(std::move(archetype));
    }
    m_baseArchetypeCount = static_cast<std::uint32_t>(m_economyParams.archetypes.size());

    // What each `[[module]]` does, cached in def order (Phase 34 stage B). The
    // same `applyRates` rule as the archetypes above, including the warning: an
    // unknown commodity on a module is a mod's business and not a refusal.
    m_modules.clear();
    m_modules.reserve(defs.modules().size());
    m_powerModules.clear();
    for (const assets::ModuleDef& module : defs.modules()) {
        ModuleRuntime runtime;
        runtime.production.assign(commodityCount, 0.0f);
        runtime.consumption.assign(commodityCount, 0.0f);
        runtime.feedstock.assign(commodityCount, 0.0f);
        const auto applyModuleRates = [&](const std::vector<assets::StationRate>& rates,
                                          std::vector<float>& out) {
            for (const assets::StationRate& rate : rates) {
                const std::uint32_t index = commodityIndex(rate.commodityId.c_str());
                if (index < commodityCount) {
                    out[index] += rate.rate;
                } else {
                    SOL_LOG_WARN(
                        "module '%s': unknown commodity '%s'", module.id.c_str(), rate.commodityId.c_str());
                }
            }
        };
        applyModuleRates(module.produces, runtime.production);
        applyModuleRates(module.consumes, runtime.consumption);
        applyModuleRates(module.feedstock, runtime.feedstock);
        runtime.powerOutput = module.powerOutput;
        runtime.powerDraw = module.powerDraw;
        for (const assets::StationScreen screen : module.screens) {
            runtime.screens |= 1u << static_cast<std::uint32_t>(screen);
        }
        // What this module can WAREHOUSE, resolved from a goods class to the
        // commodities in that class once, here (Phase 34 stage D). A hold line
        // is "units per commodity of this class", so a bulk hold of 1200 gives
        // 1200 of ore AND 1200 of plate - it is a statement about the kind of
        // space, not a shared pool.
        runtime.storage.assign(commodityCount, 0.0f);
        for (const assets::ModuleStorage& hold : module.stores) {
            for (std::uint32_t c = 0; c < commodityCount; ++c) {
                if (m_commodityClass[c] == hold.goods) {
                    runtime.storage[c] += hold.capacity;
                }
            }
        }
        // Both or neither: `parseModule` refuses a module that names one without
        // the other, and refuses a `refinery` screen with no pair behind it.
        if (!module.refineInput.empty() && !module.refineOutput.empty()) {
            runtime.refineInput = commodityIndex(module.refineInput.c_str());
            runtime.refineOutput = commodityIndex(module.refineOutput.c_str());
        }
        if (module.family == assets::ModuleFamily::Power && module.powerOutput > 0.0f) {
            m_powerModules.push_back(static_cast<std::uint32_t>(m_modules.size()));
        }
        runtime.shadow = module.family == assets::ModuleFamily::Shadow;
        runtime.recreation = module.family == assets::ModuleFamily::Recreation;
        m_modules.push_back(std::move(runtime));
    }
    // Ascending by output, which is what lets the composer take the FIRST plant
    // that covers a draw and know it is the smallest one that does.
    std::sort(m_powerModules.begin(), m_powerModules.end(), [this](std::uint32_t a, std::uint32_t b) {
        return m_modules[a].powerOutput < m_modules[b].powerOutput;
    });

    // The recipes, with every module id resolved to an index once. An id that
    // names nothing is refused at load by `validateStationRecipes`, so reaching
    // the warning below means a caller skipped that check (a test, a tool).
    m_recipes.assign(defs.stations().size(), {});
    for (std::size_t s = 0; s < defs.stations().size(); ++s) {
        for (const assets::StationModuleEntry& entry : defs.stations()[s].modules) {
            std::uint32_t index = static_cast<std::uint32_t>(defs.modules().size());
            for (std::uint32_t m = 0; m < defs.modules().size(); ++m) {
                if (defs.modules()[m].id == entry.moduleId) {
                    index = m;
                    break;
                }
            }
            if (index >= defs.modules().size()) {
                SOL_LOG_WARN("station '%s': unknown module '%s'",
                             defs.stations()[s].id.c_str(),
                             entry.moduleId.c_str());
                continue;
            }
            m_recipes[s].push_back({.module = index, .chance = entry.chance});
        }
    }
    // The cast, with every anchor resolved to an index once (Phase 35 stage C).
    // Same arrangement and same reason as the recipes above: `composeStations`
    // runs with no def database in reach, and an id that names nothing is
    // refused at load by `validateCharacters`, so reaching a `kNoIndex` below
    // means a caller skipped that check (a test, a tool).
    m_castDefs.clear();
    m_castNames.clear();
    m_castTrades.clear();
    m_castDefs.reserve(defs.characters().size());
    for (const assets::CharacterDef& character : defs.characters()) {
        CastEntry entry;
        if (!character.factionId.empty()) {
            for (std::uint32_t f = 0; f < defs.factions().size(); ++f) {
                if (defs.factions()[f].id == character.factionId) {
                    entry.faction = f;
                    break;
                }
            }
        }
        if (!character.archetypeId.empty()) {
            for (std::uint32_t a = 0; a < defs.stations().size(); ++a) {
                if (defs.stations()[a].id == character.archetypeId) {
                    entry.archetype = a;
                    break;
                }
            }
        }
        if (!character.moduleId.empty()) {
            for (std::uint32_t m = 0; m < defs.modules().size(); ++m) {
                if (defs.modules()[m].id == character.moduleId) {
                    entry.room = m;
                    break;
                }
            }
        }
        if (character.region == "core") {
            entry.region = static_cast<std::uint32_t>(sim::Region::Core);
        } else if (character.region == "frontier") {
            entry.region = static_cast<std::uint32_t>(sim::Region::Frontier);
        } else if (character.region == "fringe") {
            entry.region = static_cast<std::uint32_t>(sim::Region::Fringe);
        }
        entry.lawless = character.lawless;
        entry.shadow = character.shadow;
        m_castDefs.push_back(entry);
        m_castNames.push_back(character.name);
        m_castTrades.push_back(character.trade);
    }
    composeStations();

    if (!m_economyParams.commodities.empty()) {
        m_economy.initialize(m_galaxy, m_economyParams, m_universeSeed);
    }
    m_feedstock.mining = &m_mining;
    m_feedstock.galaxy = &m_galaxy;
    m_feedstock.economy = &m_economy;
    // Sized with the markets and cleared with them: a hold on an outpost's
    // draw belongs to the run the ship died in (Phase 8x stage 6).
    m_minerHold.assign(m_economy.markets().size(), 0.0);
    m_feedstock.minerHold = &m_minerHold;
    m_playerCargo.assign(commodityCount, 0.0f);

    // Start in the first core system with a station (deterministic per seed).
    std::uint32_t start = 0;
    for (std::uint32_t i = 0; i < m_galaxy.systems.size(); ++i) {
        if (m_galaxy.systems[i].region == sim::Region::Core && !m_galaxy.systems[i].stations.empty()) {
            start = i;
            break;
        }
    }
    m_startSystem = start;
    // A new pilot has not met anybody. The seating chart itself is already
    // fresh - `composeStations` rebuilt it above - but this half is the run's,
    // not the galaxy's, and nothing else clears it.
    m_castMemory.clear();
    // A new pilot is broadcasting. Same reason as the line above: the switch is
    // the RUN's state, not the galaxy's, and a new game inheriting the last
    // one's would open with a ship no station will clear and no way to know why.
    m_transponderOn = true;
    resetFleetToStarter();
    initializeFactions(); // before loadSystem: ambient wings need the table
    initializeSurvey();   // before loadSystem: arrival writes the first entry
    initializeMining();   // before loadSystem: it instantiates the rocks
    loadSystem(start, kNoIndex);
    // The station a new pilot launches from is known to them (Phase 8z §B).
    // loadSystem parks a fresh start 800 m off station 0, and without this the
    // game opens with the player floating beside an unidentified contact that
    // happens to be their home port. Stated here rather than inferred inside
    // identifyTouchedObjects, because a death respawn also arrives with no
    // origin system and must NOT get a station for free.
    if (!m_galaxy.systems[start].stations.empty()) {
        (void)m_survey.notifyStationIdentified(m_galaxy, start, 0);
        refreshStaticTargetNames();
    }
    SOL_LOG_INFO("universe: seed %llu, %zu systems, %zu lanes, %zu faction(s) "
                 "(%zu clans); starting in '%s'",
                 static_cast<unsigned long long>(m_universeSeed),
                 m_galaxy.systems.size(),
                 m_galaxy.links.size(),
                 m_factionTable.size(),
                 m_galaxy.clans.size(),
                 currentSystemName());
    // The security gradient, beside the line that already says how big the
    // galaxy is (Phase 30 stage A). Baselines only: the live rating moves under
    // the player, and what this reports is a property of the GENERATOR.
    // Logged rather than left to a console probe because five lines of answer
    // scroll out of the dev console within a second under the faction sim's
    // own chatter, and because `--frames N` then answers the whole of stage A's
    // exit criterion without a GUI at all.
    {
        const SecurityHistogram gradient = securityHistogram();
        SOL_LOG_INFO("security: core %u [%.2f..%.2f] mean %.3f | frontier %u [%.2f..%.2f] mean %.3f "
                     "| fringe %u [%.2f..%.2f] mean %.3f | clan-held %u deepest %.2f | unpoliced %u",
                     gradient.seen[0],
                     static_cast<double>(gradient.lowest[0]),
                     static_cast<double>(gradient.highest[0]),
                     gradient.mean(0),
                     gradient.seen[1],
                     static_cast<double>(gradient.lowest[1]),
                     static_cast<double>(gradient.highest[1]),
                     gradient.mean(1),
                     gradient.seen[2],
                     static_cast<double>(gradient.lowest[2]),
                     static_cast<double>(gradient.highest[2]),
                     gradient.mean(2),
                     gradient.clanHeld,
                     static_cast<double>(gradient.deepest),
                     gradient.unpoliced);
    }
    return true;
}

// ⚑⚑ COUNTED THROUGH THE ACCESSOR, NEVER OFF THE SPEC. Since stage F the
// stored field is an unsigned magnitude and the sign - which is the whole shape
// this reports - is a view over the CURRENT owner. Reading `spec.security` here
// would file every clan neighbourhood under whichever region band it sits in
// and report the galaxy as entirely policed.
SpaceWorld::SecurityHistogram SpaceWorld::securityHistogram() const
{
    SecurityHistogram out;
    float lowest[3] = {2.0f, 2.0f, 2.0f};
    float highest[3] = {-2.0f, -2.0f, -2.0f};
    for (std::uint32_t i = 0; i < m_galaxy.systems.size(); ++i) {
        const float rating = systemSecurityBaseline(i);
        if (rating < 0.0f) {
            ++out.clanHeld;
            out.clanSum += static_cast<double>(rating);
            out.deepest = std::min(out.deepest, rating);
            continue;
        }
        if (rating == 0.0f) {
            ++out.unpoliced;
            continue;
        }
        const auto tier = static_cast<std::size_t>(m_galaxy.systems[i].region);
        out.sum[tier] += static_cast<double>(rating);
        ++out.seen[tier];
        lowest[tier] = std::min(lowest[tier], rating);
        highest[tier] = std::max(highest[tier], rating);
    }
    for (std::size_t tier = 0; tier < 3; ++tier) {
        out.lowest[tier] = out.seen[tier] > 0 ? lowest[tier] : 0.0f;
        out.highest[tier] = out.seen[tier] > 0 ? highest[tier] : 0.0f;
    }
    return out;
}

// The stream the composition roll comes out of (Phase 34 stage B), chosen the
// way `universe.cpp`'s `kStreamAuthored` was and for the same reason: the
// generator's per-system streams are `kStreamContents + systemIndex`, so they
// run off the end of that enum and over anything sitting above it. A number no
// system index can reach is not elegant, it is the only thing that is true.
constexpr std::uint64_t kCompositionStream = 2'000'000;

// And the one the shadow operator comes out of (Phase 34 stage E). A SECOND
// stream rather than a continuation of the first, for the reason the first one
// exists at all: composing and staffing are independent edits, and sharing a
// stream would make re-tuning one recipe's fence chance re-roll who runs every
// other fence in the galaxy.

// And the one the cast comes out of (Phase 35 stage C). A THIRD stream for the
// second one's reason restated: who is in the room and who runs the fence are
// independent edits, and sharing a stream would make adding a `[[character]]`
// row rename every regular in the galaxy.
constexpr std::uint64_t kCastStream = 4'000'000;

// And the one the crew halls come out of (Phase 39 stage A). A FOURTH stream,
// for the third one's reason restated once more: who is drinking in a room and
// who is looking for a berth are independent edits, and sharing the cast's
// stream would make adding a `[[character]]` row re-roll every captain on
// offer in the galaxy.
constexpr std::uint64_t kCaptainStream = 5'000'000;

// What a captain FLEW before you met them.
//
// ⚑⚑ A ROLLED TABLE HERE WHERE `regularTrade` REFUSED ONE, AND THE
// DIFFERENCE IS REAL RATHER THAN A LAPSE. A regular's trade is read off the
// station's own composition because a regular IS the room - a prospector on a
// mine is content the composer already paid for. Somebody standing in a crew
// hall looking for a berth is passing THROUGH: their history is not this
// station's, and reading it off the dock would make every captain in a mining
// system an ex-prospector, which says something false rather than something
// free.
constexpr const char* kCaptainTrades[] = {
    "Long hauler",
    "Ex-navy",
    "Freighter master",
    "Belt runner",
    "Convoy escort",
    "Yard test pilot",
    "Tramp skipper",
    "Survey pilot",
};

void SpaceWorld::composeStations()
{
    // Everything composed is derived, never saved, so a re-compose starts by
    // throwing the last one away - `loadFrom` calls this against a galaxy that
    // was regenerated under a different seed.
    m_compositions.clear();
    if (m_economyParams.archetypes.size() > m_baseArchetypeCount) {
        m_economyParams.archetypes.resize(m_baseArchetypeCount);
    }
    for (sim::SystemSpec& system : m_galaxy.systems) {
        for (sim::StationSpec& station : system.stations) {
            station.composition = sim::kNoComposition;
            station.shadowOwner = sim::kNoFaction;
        }
    }
    if (m_recipes.empty() || m_modules.empty()) {
        return; // no `[[module]]` content: every station keeps its archetype
    }

    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());
    core::Rng rng;
    rng.seed(m_universeSeed, kCompositionStream);

    std::vector<std::uint32_t> rolled;
    for (sim::SystemSpec& system : m_galaxy.systems) {
        for (sim::StationSpec& station : system.stations) {
            if (station.archetype >= m_recipes.size() || m_recipes[station.archetype].empty()) {
                continue; // an archetype with no recipe runs on its own rates
            }
            rolled.clear();
            float draw = 0.0f;
            // ⚑ THE ROLL IS TAKEN FOR EVERY LINE, INCLUDING THE CERTAIN ONES.
            // A recipe is mostly chance-1.0 rows, and skipping their draw would
            // make the stream depend on how many certainties an archetype
            // happens to have - so editing an unrelated recipe would re-roll
            // every station after it. Drawing unconditionally costs nothing and
            // keeps one station's composition a function of its own line.
            for (const RecipeEntry& entry : m_recipes[station.archetype]) {
                const float roll = rng.nextFloat01();
                if (entry.chance < 1.0f && roll >= entry.chance) {
                    continue;
                }
                rolled.push_back(entry.module);
                draw += m_modules[entry.module].powerDraw;
            }
            // ⚑⚑ POWER IS FITTED, NOT ROLLED, WHICH IS HOW THE RULING IS
            // SATISFIED BY CONSTRUCTION RATHER THAN BY REJECTION. The spec said
            // the composer "rejects or re-draws" a composition whose draw is not
            // covered; selecting the smallest plant that covers it is the same
            // rule with no unbounded loop in it, and it makes the plant a
            // consequence of the station: an outpost gets a solar array and a
            // shipyard gets a fusion plant, visibly, without anyone authoring
            // either one.
            float output = 0.0f;
            for (const std::uint32_t module : rolled) {
                output += m_modules[module].powerOutput; // a recipe may not name one; belt and braces
            }
            while (output < draw && !m_powerModules.empty()) {
                std::uint32_t chosen = m_powerModules.back(); // the largest, if nothing else covers
                for (const std::uint32_t candidate : m_powerModules) {
                    if (m_modules[candidate].powerOutput >= draw - output) {
                        chosen = candidate;
                        break; // ascending by output, so the first that covers is the smallest
                    }
                }
                rolled.push_back(chosen);
                output += m_modules[chosen].powerOutput;
                draw += m_modules[chosen].powerDraw; // a plant of its own runs on something
            }

            // Identical stations share a row: a composition is (archetype,
            // module list), and the archetype is part of it because extraction
            // and capacity still come from there.
            std::uint32_t index = static_cast<std::uint32_t>(m_compositions.size());
            for (std::uint32_t i = 0; i < m_compositions.size(); ++i) {
                if (m_compositions[i].archetype == station.archetype && m_compositions[i].modules == rolled) {
                    index = i;
                    break;
                }
            }
            if (index == m_compositions.size()) {
                m_compositions.push_back({.archetype = station.archetype, .modules = rolled});
            }
            station.composition = m_baseArchetypeCount + index;
        }
    }

    // Each composition becomes a row of `EconomyParams::archetypes`, which is
    // what `Economy::initialize` will index for every market that has one.
    for (const StationComposition& composition : m_compositions) {
        sim::EconomyArchetype archetype;
        archetype.production.assign(commodityCount, 0.0f);
        archetype.consumption.assign(commodityCount, 0.0f);
        archetype.feedstock.assign(commodityCount, 0.0f);
        for (const std::uint32_t module : composition.modules) {
            const ModuleRuntime& runtime = m_modules[module];
            for (std::uint32_t c = 0; c < commodityCount; ++c) {
                archetype.production[c] += runtime.production[c];
                archetype.consumption[c] += runtime.consumption[c];
                archetype.feedstock[c] += runtime.feedstock[c];
            }
        }
        // ⚑ EXTRACTION STAYS WITH THE ARCHETYPE AND THAT IS NOT AN OVERSIGHT.
        // `produces_from = "field"` is also the PLACEMENT veto ("a system with
        // no rock supports no mine"), so splitting it across two files would be
        // saying one fact twice.
        //
        // ⚑⚑ CAPACITY NO LONGER DOES, WHICH IS THE WHOLE OF STAGE D. Stage B
        // left it here on purpose - "moving it here early would change every
        // price in the galaxy, since a price is stock over capacity" - and this
        // is the stage that was told to move it. A composed station warehouses
        // exactly what its holds add up to, per commodity, and a good no hold
        // admits sits at a capacity of zero: it cannot be stocked, delivered,
        // produced or sold there at all.
        const sim::EconomyArchetype& base = m_economyParams.archetypes[composition.archetype];
        archetype.extracts = base.extracts;
        archetype.stockCapacity.assign(commodityCount, 0.0f);
        for (const std::uint32_t module : composition.modules) {
            const ModuleRuntime& runtime = m_modules[module];
            for (std::uint32_t c = 0; c < commodityCount; ++c) {
                archetype.stockCapacity[c] += runtime.storage[c];
            }
        }
        m_economyParams.archetypes.push_back(std::move(archetype));
    }

    assignShadowOwners();
    assignCast();
}

// ⚑⚑⚑⚑ WHOSE BACK ROOM IT IS, AND SINCE PHASE 37 STAGE C THE ANSWER IS
// ALWAYS THE SAME PEOPLE. Phase 34 stage E had to point this field at a pirate
// clan picked by a uniform roll, because the shadow faction had not shipped and
// pointing it at nothing would have left a null column on all 125 stations and
// called it vocabulary. It has shipped. Ruling 1 of this phase is that there is
// exactly ONE hand-authored black market, so the operator stopped being a
// CHOICE and became an IDENTITY - and a function that picks between candidates
// is the wrong shape for a question with one answer.
//
// ⚑⚑⚑ WHAT WENT WITH `shadowOperatorFor`, DELETED RATHER THAN LEFT STANDING:
// the uniform roll, the step off the founding holder (a fence the local boss
// runs is his own shop, not a shadow presence - true of a clan, meaningless for
// a faction that can never hold a system), the single-clan case, and the whole
// `kShadowStream` RNG. Dead code left standing reads as a rule that still
// applies, and every one of those four was a rule about clans.
//
// ⚑⚑ THE GOLDEN DIGEST MOVES HERE AND THAT IS EXPECTED, unlike stage B where
// it must not. `galaxy_golden_tests.cpp` hashes `shadowOwner` per station
// precisely so that re-pointing it is visible; the recorded value is updated in
// the same commit that moves it.
void SpaceWorld::assignShadowOwners()
{
    // ⚑⚑⚑⚑ THE INDEX IS COMPUTED, NOT LOOKED UP, AND THE REASON IS THE ONE
    // PHASE 34 STAGE E ALREADY WROTE DOWN HERE: THIS PASS RUNS INSIDE
    // `composeStations`, WHICH RUNS BEFORE `initializeFactions`. There is no
    // `m_factionTable` yet - `m_shadowBase` is still `kNoFaction` at this point
    // - so reading the accessor would have quietly assigned nobody and left
    // every shadow test looking at an empty column. The old pass said exactly
    // this about `clanBase` under the comment "the arithmetic is the table
    // lookup, without the table"; the same sentence now covers a second index.
    //
    // ⚑⚑⚑ AND THIS IS WHERE STAGE B'S "APPENDED LAST" EARNS ITS KEEP A SECOND
    // TIME. Majors take [0, factionCount), clans take the `clanCount` after
    // them, and the shadow rows begin where the clans end - so the index is
    // knowable from the galaxy alone, before any table is built. Had the row
    // been slotted among the majors there would be no arithmetic to do it with.
    // `the_arithmetic_that_names_the_operator_agrees_with_the_table` is the
    // guard that the two ways of getting this number stay the same number.
    bool authored = false;
    if (m_defs != nullptr) {
        for (const assets::FactionDef& def : m_defs->factions()) {
            authored = authored || def.kind == assets::FactionKind::Shadow;
        }
    }
    if (!authored) {
        return; // no black market in this def set: nobody runs a back room
    }
    const std::uint32_t shadowBase =
        m_galaxyParams.factionCount + static_cast<std::uint32_t>(m_galaxy.clans.size());
    for (std::uint32_t s = 0; s < m_galaxy.systems.size(); ++s) {
        sim::SystemSpec& system = m_galaxy.systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            for (const std::uint32_t module : stationModules(s, t)) {
                if (m_modules[module].shadow) {
                    system.stations[t].shadowOwner = shadowBase;
                    break;
                }
            }
        }
    }
}

std::uint32_t SpaceWorld::stationShadowOwner(std::uint32_t system, std::uint32_t station) const
{
    if (system >= m_galaxy.systems.size()) {
        return sim::kNoFaction;
    }
    const std::vector<sim::StationSpec>& stations = m_galaxy.systems[system].stations;
    return station < stations.size() ? stations[station].shadowOwner : sim::kNoFaction;
}

bool SpaceWorld::stationHasShadowPresence(std::uint32_t system, std::uint32_t station) const
{
    // ⚑⚑⚑ THE LIVE-HOLDER COMPARISON WENT WITH THE CLAN PICKER (Phase 37 stage
    // C). This used to read `owner != systemOwnerFaction(system)`, and it had to:
    // the operator was a clan, a clan can TAKE the system its own fence sits in,
    // and on the day it did the fence stopped being a shadow presence and became
    // the local boss's shop. A faction that claims nothing can never be the
    // holder, so that comparison is now true for every station in every galaxy
    // - and a condition that cannot be false is a comment pretending to be
    // code. ⚑ The half that survives is the one that always mattered: is there
    // a back room here at all.
    return stationShadowOwner(system, station) != sim::kNoFaction;
}

std::span<const std::uint32_t> SpaceWorld::stationModules(std::uint32_t system, std::uint32_t station) const
{
    if (system >= m_galaxy.systems.size()) {
        return {};
    }
    const std::vector<sim::StationSpec>& stations = m_galaxy.systems[system].stations;
    if (station >= stations.size()) {
        return {};
    }
    const std::uint32_t composition = stations[station].composition;
    if (composition < m_baseArchetypeCount || composition - m_baseArchetypeCount >= m_compositions.size()) {
        return {};
    }
    return m_compositions[composition - m_baseArchetypeCount].modules;
}

namespace {

// ---------------------------------------------------------------------------
// The cast (Phase 35 stage C).
//
// ⚑⚑⚑⚑ THE NAMES ARE BUILT THE WAY A SYSTEM'S NAME IS BUILT, AND THAT IS A
// PRECEDENT FOLLOWED RATHER THAN A SHORTCUT TAKEN. `universe.cpp` has generated
// every system name in this galaxy out of three syllable tables since Phase 6,
// and every clan name out of a suffix table since 8b; a regular's name is the
// same trick pointed at people. It is the whole reason 62 rooms of 62 can have
// a face while the authored cast stays six rows long - the alternative was
// sixty rows of filler, which the phase's own risk register calls worse than
// none.
// ---------------------------------------------------------------------------
constexpr const char* kGivenNames[] = {
    "Ada",  "Bo",   "Cass", "Dov",  "Esme",  "Fen", "Gita",  "Hal",  "Ines",  "Joss",
    "Kira", "Lem",  "Mira", "Nils", "Odell", "Pia", "Quill", "Rane", "Sofi",  "Tarek",
    "Uma",  "Vess", "Wren", "Xan",  "Yusuf", "Zia", "Ari",   "Bex",  "Corin", "Dara",
};
constexpr const char* kFamilyNames[] = {
    "Aldiss",  "Brannt", "Cerova",  "Dunmore",   "Eklund",  "Farrow",   "Gunnar",   "Halvard",
    "Iversen", "Jarrah", "Kessel",  "Lindqvist", "Moreau",  "Nakamura", "Oyelaran", "Prieto",
    "Quintal", "Reyes",  "Stavros", "Torvald",   "Ustinov", "Vance",    "Whelan",   "Yates",
};

// What a regular does for a living, read off the station's own composition
// rather than rolled - so the person in the room is OF the place. A prospector
// on a mine and a yard hand where there are berths is content the composer has
// already paid for; a rolled trade would have been a second table saying
// nothing the first one does not.
//
// ⚑ In priority order, most specific first, and the last row catches
// everything: a dock with nothing distinctive still has somebody hauling
// crates through it.
const char* regularTrade(bool shadow, bool shipyard, bool mines, bool farms, bool industry)
{
    if (shadow) {
        return "Fixer";
    }
    if (shipyard) {
        return "Yard hand";
    }
    if (mines) {
        return "Prospector";
    }
    if (farms) {
        return "Crop hand";
    }
    if (industry) {
        return "Line worker";
    }
    return "Hauler";
}

} // namespace

std::uint64_t SpaceWorld::castKeyForCharacter(const char* id)
{
    // Masked to 63 bits so a person's key can never collide with `kSeatKey`'s
    // space. It hashes the ID rather than the NAME because a name is content an
    // author may re-word, and re-wording a line must not make a character
    // forget the player.
    return core::fnv1a(id != nullptr ? id : "", core::kFnvOffsetBasis) & ~kSeatKey;
}

bool SpaceWorld::castSeatSuits(const CastEntry& entry, const CastSeatFacts& seat, std::uint32_t clanBase)
{
    // ⚑⚑⚑⚑ THE FACTION ANCHOR READS THE *FOUNDING CLAIM*, WHICH IS THE OPPOSITE
    // OF WHAT STAGE E CONCLUDED ABOUT THE SAME FIELD - deliberately, and for a
    // reason that can be stated rather than a lapse nobody caught. Stage E
    // derives shadow-ness from the LIVE holder because "is this a shadow
    // presence" is a question about the CURRENT relationship between two
    // parties, so an answer stored at generation time is wrong within a minute
    // of play. Where a person SITS is not that kind of question. A Guild broker
    // whose system is taken by the Hegemony this afternoon is still in the same
    // bar: people do not move when a border does, and a seat re-derived from
    // the live owner would have the whole cast teleporting several times a
    // minute.
    //
    // ⚑ So both readings of `factionIndex` are right, about different
    // questions. The trap stage E named is a STORED answer to a MOVING
    // question; this is a stored answer to a fixed one.
    if (entry.faction != sim::kNoFaction && seat.founder != entry.faction) {
        return false;
    }
    if (entry.lawless && (seat.founder == sim::kNoFaction || seat.founder < clanBase)) {
        return false;
    }
    if (entry.archetype != kNoIndex && seat.archetype != entry.archetype) {
        return false;
    }
    if (entry.room != kNoIndex && seat.room != entry.room) {
        return false;
    }
    if (entry.region != kNoIndex && seat.region != entry.region) {
        return false;
    }
    if (entry.shadow && !seat.hasShadow) {
        return false;
    }
    return true;
}

void SpaceWorld::assignCast()
{
    m_cast.assign(m_galaxy.systems.size(), {});
    for (std::uint32_t s = 0; s < m_galaxy.systems.size(); ++s) {
        m_cast[s].assign(m_galaxy.systems[s].stations.size(), {});
    }
    if (m_modules.empty()) {
        return; // no `[[module]]` content: no rooms, so nobody to seat
    }

    const std::uint32_t clanBase = m_galaxyParams.factionCount;
    core::Rng rng;
    rng.seed(m_universeSeed, kCastStream);

    // --- Pass one: a regular in every room.
    //
    // ⚑ THE DRAWS ARE TAKEN FOR EVERY STATION, INCLUDING THE ONES WITH NO ROOM,
    // and it is the rule the two passes above already follow for the same
    // reason. Drawing only where a room landed would make the stream depend on
    // WHICH stations rolled one, so nudging a single recipe's bar chance would
    // rename every regular after it in galaxy order - and it would make pass
    // two below depend on the composition as well. Two wasted draws per
    // roomless station buy a stream whose length is a function of the galaxy
    // and nothing else.
    std::vector<CastSeatFacts> facts;
    std::vector<std::uint64_t> seats; // (system << 32) | station, parallel to facts
    for (std::uint32_t s = 0; s < m_galaxy.systems.size(); ++s) {
        for (std::uint32_t t = 0; t < m_galaxy.systems[s].stations.size(); ++t) {
            const std::uint32_t given = rng.range(static_cast<std::uint32_t>(std::size(kGivenNames)));
            const std::uint32_t family = rng.range(static_cast<std::uint32_t>(std::size(kFamilyNames)));

            CastSeatFacts seat;
            seat.founder = m_galaxy.systems[s].factionIndex;
            seat.archetype = m_galaxy.systems[s].stations[t].archetype;
            seat.region = static_cast<std::uint32_t>(m_galaxy.systems[s].region);
            bool shipyard = false;
            bool farms = false;
            bool industry = false;
            // An archetype whose output comes out of the ground rather than off
            // anybody's dock, which is the def's own word for it and is already
            // resolved on the economy archetype.
            const bool mines = seat.archetype < m_baseArchetypeCount &&
                               seat.archetype < m_economyParams.archetypes.size() &&
                               m_economyParams.archetypes[seat.archetype].extracts;
            float bestDraw = -1.0f;
            for (const std::uint32_t module : stationModules(s, t)) {
                const ModuleRuntime& runtime = m_modules[module];
                if (runtime.shadow) {
                    seat.hasShadow = true;
                }
                const std::uint32_t shipyardBit =
                    1u << static_cast<std::uint32_t>(assets::StationScreen::Shipyard);
                if ((runtime.screens & shipyardBit) != 0) {
                    shipyard = true;
                }
                if (runtime.recreation && runtime.powerDraw > bestDraw) {
                    bestDraw = runtime.powerDraw;
                    seat.room = module;
                }
                for (std::uint32_t c = 0; c < runtime.production.size(); ++c) {
                    if (runtime.production[c] <= 0.0f) {
                        continue;
                    }
                    industry = true;
                    // Food is the galaxy's only `cryo` good (Phase 34 stage D),
                    // so a module producing one is a farm without anything
                    // having to be told that it is.
                    if (c < m_commodityClass.size() && m_commodityClass[c] == assets::GoodsClass::Cryo) {
                        farms = true;
                    }
                }
            }
            if (seat.room == kNoIndex) {
                continue; // no recreation module: no room, and nobody in it
            }
            facts.push_back(seat);
            seats.push_back((static_cast<std::uint64_t>(s) << 32u) | t);
            m_cast[s][t].character = kNoCharacter;
            m_cast[s][t].name = std::string(kGivenNames[given]) + " " + kFamilyNames[family];
            m_cast[s][t].trade = regularTrade(seat.hasShadow, shipyard, mines, farms, industry);
        }
    }

    // --- Pass two: the authored cast, in def order, each taking one free seat
    // its anchors allow.
    //
    // ⚑⚑ DEF ORDER, WHICH MAKES *WRITE THE TIGHTEST ANCHOR FIRST* AN AUTHORING
    // RULE RATHER THAN AN ENGINE ONE - and it is written into `characters.toml`
    // beside the cast. Sorting by how many seats an anchor selects would take
    // that control away from the author and would make adding a row re-seat
    // everybody; def order means an APPENDED row moves nobody, and a starved
    // anchor is caught by name in `station_cast_tests.cpp` rather than silently
    // dropped.
    std::vector<std::uint32_t> eligible;
    for (std::uint32_t i = 0; i < m_castDefs.size(); ++i) {
        eligible.clear();
        for (std::uint32_t index = 0; index < facts.size(); ++index) {
            const std::uint32_t s = static_cast<std::uint32_t>(seats[index] >> 32u);
            const std::uint32_t t = static_cast<std::uint32_t>(seats[index] & 0xFFFF'FFFFull);
            if (m_cast[s][t].character != kNoCharacter) {
                continue; // somebody written is already sitting there
            }
            if (castSeatSuits(m_castDefs[i], facts[index], clanBase)) {
                eligible.push_back(index);
            }
        }
        // ⚑ THE ROLL IS TAKEN WHETHER OR NOT THERE IS ANYWHERE TO SIT, for the
        // rule this file has now stated three times: a character whose anchor
        // selects nothing must not shift the seat of the character written
        // after them.
        const std::uint32_t roll = rng.nextU32();
        if (eligible.empty()) {
            continue; // no free seat: they are not in this galaxy, and a test says so
        }
        const std::uint32_t index = eligible[roll % eligible.size()];
        const std::uint32_t s = static_cast<std::uint32_t>(seats[index] >> 32u);
        const std::uint32_t t = static_cast<std::uint32_t>(seats[index] & 0xFFFF'FFFFull);
        m_cast[s][t].character = i;
        m_cast[s][t].name = m_castNames[i];
        m_cast[s][t].trade = m_castTrades[i];
    }
}

void SpaceWorld::castCandidates(std::uint32_t from, std::vector<sim::CastCandidate>& out) const
{
    out.clear();
    if (from >= m_galaxy.systems.size()) {
        return;
    }
    std::vector<std::uint8_t> depths;
    m_missions.jumpDepths(m_galaxy, from, depths);
    if (depths.size() != m_galaxy.systems.size()) {
        return;
    }
    for (std::uint32_t s = 0; s < m_galaxy.systems.size(); ++s) {
        // 0xff is jumpDepths' own "out of reach", and 0 is this system.
        if (depths[s] == 0xffu || depths[s] == 0) {
            continue;
        }
        for (std::uint32_t t = 0; t < m_galaxy.systems[s].stations.size(); ++t) {
            const CastSeat* seat = stationCast(s, t);
            if (seat == nullptr) {
                continue;
            }
            const CastMemory* memory = castMemory(castKeyAt(s, t));
            out.push_back({.system = s,
                           .station = t,
                           .jumps = depths[s],
                           .visits = memory != nullptr ? memory->visits : 0,
                           .authored = seat->character != kNoCharacter});
        }
    }
}

const SpaceWorld::CastSeat* SpaceWorld::stationCast(std::uint32_t system, std::uint32_t station) const
{
    if (system >= m_cast.size() || station >= m_cast[system].size()) {
        return nullptr;
    }
    const CastSeat& seat = m_cast[system][station];
    return seat.name.empty() ? nullptr : &seat;
}

std::uint64_t SpaceWorld::castKeyAt(std::uint32_t system, std::uint32_t station) const
{
    const CastSeat* seat = stationCast(system, station);
    if (seat == nullptr) {
        return 0;
    }
    if (seat->character == kNoCharacter) {
        return castKeyForSeat(system, station);
    }
    const bool known = m_defs != nullptr && seat->character < m_defs->characters().size();
    return castKeyForCharacter(known ? m_defs->characters()[seat->character].id.c_str() : "");
}

const SpaceWorld::CastMemory* SpaceWorld::castMemory(std::uint64_t who) const
{
    for (const CastMemory& memory : m_castMemory) {
        if (memory.who == who) {
            return &memory;
        }
    }
    return nullptr;
}

void SpaceWorld::noteCastVisit(std::uint64_t who)
{
    if (who == 0) {
        return;
    }
    for (CastMemory& memory : m_castMemory) {
        if (memory.who == who) {
            ++memory.visits;
            return;
        }
    }
    m_castMemory.push_back({.who = who, .visits = 1, .regard = 0});
}

void SpaceWorld::adjustCastRegard(std::uint64_t who, std::int32_t delta)
{
    if (who == 0) {
        return;
    }
    for (CastMemory& memory : m_castMemory) {
        if (memory.who == who) {
            memory.regard += delta;
            return;
        }
    }
    m_castMemory.push_back({.who = who, .visits = 0, .regard = delta});
}

std::uint32_t SpaceWorld::stationScreens(std::uint32_t system, std::uint32_t station) const
{
    constexpr std::uint32_t kEveryScreen = (1u << assets::kStationScreenCount) - 1u;
    const std::span<const std::uint32_t> modules = stationModules(system, station);
    if (modules.empty()) {
        return kEveryScreen; // no composition: the galaxy that shipped before this phase
    }
    std::uint32_t screens = 0;
    for (const std::uint32_t module : modules) {
        screens |= m_modules[module].screens;
    }
    return screens;
}

std::uint32_t SpaceWorld::dockedStationScreens() const
{
    if (!isDocked()) {
        return 0;
    }
    return stationScreens(m_currentSystem, m_dockedStation);
}

bool SpaceWorld::stationStocks(std::uint32_t system, std::uint32_t station, std::uint32_t commodity) const
{
    std::uint32_t market = 0;
    if (!sim::marketAt(m_economy.markets(), system, station, &market)) {
        return false;
    }
    return m_economy.capacityOf(market, commodity) > 0.0f;
}

bool SpaceWorld::dockedStationStocks(std::uint32_t commodity) const
{
    if (!isDocked()) {
        return false;
    }
    const std::uint32_t market = dockedMarket();
    if (market >= m_economy.markets().size()) {
        return false;
    }
    return m_economy.capacityOf(market, commodity) > 0.0f;
}

void SpaceWorld::initializeFactions()
{
    m_factionTable.clear();
    m_shadowBase = sim::kNoFaction;
    if (m_defs == nullptr) {
        return;
    }
    // Majors in def order (their generator indices), then clans, then shadow.
    //
    // ⚑⚑⚑⚑ THE SHADOW ROWS GO LAST AND THAT IS LOAD-BEARING RATHER THAN TIDY
    // (Phase 37 stage B). A clan's faction index is `factionCount + clanIndex`,
    // computed by hand in three places - `assignShadowOwners` says so under the
    // comment "the arithmetic is the table lookup, without the table" - so a
    // shadow row inserted among the majors would shift every clan index by one
    // and every one of those hand computations would silently name the wrong
    // faction. Appending keeps majors at their generator indices and clans at
    // theirs, and the new rows sit past the end of everything that arithmetic
    // can reach.
    //
    // ⚑⚑ THE COST OF APPENDING, STATED HERE BECAUSE IT IS INVISIBLE AT THE
    // CALL SITE: `m_factionTable.size() - m_galaxy.clans.size()` is no longer
    // the major count. Nothing in this file computed it that way; one test did,
    // and it now asks `shadowFactionBase()` instead.
    std::vector<const assets::FactionDef*> pirateTemplates;
    std::vector<const assets::FactionDef*> shadowDefs;
    for (const assets::FactionDef& def : m_defs->factions()) {
        if (def.kind == assets::FactionKind::Pirate) {
            pirateTemplates.push_back(&def);
            continue;
        }
        if (def.kind == assets::FactionKind::Shadow) {
            shadowDefs.push_back(&def);
            continue;
        }
        m_factionTable.push_back({.defId = def.id,
                                  .name = def.name,
                                  .color = {def.color[0], def.color[1], def.color[2]},
                                  .kind = assets::FactionKind::Major,
                                  .aggression = def.aggression,
                                  .forgiveness = def.forgiveness,
                                  .shipsPatrol = def.shipsPatrol,
                                  .shipsRaider = def.shipsRaider,
                                  .shipsTrader = def.shipsTrader,
                                  .buildsNo = {def.buildsNo[0], def.buildsNo[1], def.buildsNo[2]}});
    }
    const std::size_t majorCount = m_factionTable.size();
    for (const sim::ClanSpec& clan : m_galaxy.clans) {
        if (clan.templateIndex >= pirateTemplates.size()) {
            continue; // template roster shrank since generation; skip cleanly
        }
        const assets::FactionDef& base = *pirateTemplates[clan.templateIndex];
        core::Rng jitter(clan.seed, 1);
        const auto jitterChannel = [&](float value) {
            return core::clamp(value * (0.75f + 0.5f * jitter.nextFloat01()), 0.05f, 1.0f);
        };
        const auto jitterWeight = [&](float value) {
            return core::clamp(value + 0.3f * jitter.nextFloat01() - 0.15f, 0.0f, 1.0f);
        };
        m_factionTable.push_back({.defId = base.id,
                                  .name = clan.name,
                                  .color = {jitterChannel(base.color[0]),
                                            jitterChannel(base.color[1]),
                                            jitterChannel(base.color[2])},
                                  .kind = assets::FactionKind::Pirate,
                                  .aggression = jitterWeight(base.aggression),
                                  .forgiveness = jitterWeight(base.forgiveness),
                                  .shipsPatrol = base.shipsPatrol,
                                  .shipsRaider = base.shipsRaider,
                                  .shipsTrader = base.shipsTrader,
                                  .buildsNo = {base.buildsNo[0], base.buildsNo[1], base.buildsNo[2]}});
    }

    // ⚑⚑ UNJITTERED, UNLIKE A CLAN, AND THAT IS RULING 1 IN ONE LINE. A clan
    // is a TEMPLATE stamped once per lawless neighbourhood with its colour and
    // personality jittered per clan seed, so no two are quite the same faction.
    // There is exactly one Ninth Shift and every fence in the galaxy is theirs,
    // which is what makes the opposed axis Phase 37 stage E is after ONE number
    // a player can watch move rather than N weak ones.
    if (!shadowDefs.empty()) {
        m_shadowBase = static_cast<std::uint32_t>(m_factionTable.size());
    }
    for (const assets::FactionDef* def : shadowDefs) {
        m_factionTable.push_back({.defId = def->id,
                                  .name = def->name,
                                  .color = {def->color[0], def->color[1], def->color[2]},
                                  .kind = assets::FactionKind::Shadow,
                                  .aggression = def->aggression,
                                  .forgiveness = def->forgiveness,
                                  .shipsPatrol = def->shipsPatrol,
                                  .shipsRaider = def->shipsRaider,
                                  .shipsTrader = def->shipsTrader,
                                  .buildsNo = {def->buildsNo[0], def->buildsNo[1], def->buildsNo[2]}});
    }

    // FactionSim params: authored relations resolve def ids to table
    // indices (clans inherit their template's rows); unspecified
    // major-pirate pairs open at the default enmity.
    //
    // ⚑⚑⚑ A SHADOW FACTION TAKES THE CROSS-KIND DEFAULT AGAINST EVERY CLAN AND
    // THAT IS THE ONE THING `pirate = false` GETS WRONG FOR FREE (Phase 37
    // stage B). The pair loop below asks whether two rows differ in
    // pirate-ness, which is the right question when there are two kinds; with
    // three it opens the black market at -60 against the very people who run
    // its fences. Fixed in the DATA rather than here - `factions.toml` declares
    // the two rows against the pirate templates and clans inherit their
    // template's, which is the mechanism this file already has. ⚑ The other
    // half is right for free and is worth naming so nobody "fixes" it:
    // shadow-vs-major gets NO default enmity, because a secret organisation is
    // not at war with the law, it is hidden from it.
    const std::uint32_t count = static_cast<std::uint32_t>(m_factionTable.size());
    sim::FactionSimParams params;
    params.agents.reserve(count);
    for (const GameFaction& faction : m_factionTable) {
        params.agents.push_back({.aggression = faction.aggression,
                                 .forgiveness = faction.forgiveness,
                                 .pirate = faction.pirate(),
                                 // The one place the kind reaches the sim, and it
                                 // reaches it as a capability rather than a name:
                                 // `sol::sim` still never learns what a def is.
                                 .territorial = !faction.shadow()});
        params.initialStandings.push_back(faction.pirate() ? kClanInitialStanding : 0.0f);
    }
    params.baselineRelations.assign(static_cast<std::size_t>(count) * count, 0.0f);
    const auto setPair = [&](std::uint32_t a, std::uint32_t b, float value) {
        params.baselineRelations[static_cast<std::size_t>(a) * count + b] = value;
        params.baselineRelations[static_cast<std::size_t>(b) * count + a] = value;
    };
    for (std::uint32_t a = 0; a < count; ++a) {
        for (std::uint32_t b = a + 1; b < count; ++b) {
            if (m_factionTable[a].pirate() != m_factionTable[b].pirate()) {
                setPair(a, b, kDefaultPirateRelation);
            }
        }
    }
    for (std::uint32_t a = 0; a < count; ++a) {
        const assets::FactionDef* def = m_defs->findFaction(m_factionTable[a].defId.c_str());
        if (def == nullptr) {
            continue;
        }
        for (const assets::FactionRelation& relation : def->relations) {
            bool found = false;
            for (std::uint32_t b = 0; b < count; ++b) {
                if (b != a && m_factionTable[b].defId == relation.otherId) {
                    setPair(a, b, relation.standing);
                    found = true;
                }
            }
            // ⚑ Clans silence per-clan repeats - ten clans stamped from one
            // template would print one typo ten times. A shadow row is a single
            // hand-authored def like a major, so it warns like one (Phase 37
            // stage B), and it is the row most likely to name a faction that
            // does not exist because its whole relation list is authored
            // against OTHER people's ids.
            if (!found && (a < majorCount || m_factionTable[a].shadow())) {
                SOL_LOG_WARN("faction '%s': relation to unknown faction '%s' ignored",
                             def->id.c_str(),
                             relation.otherId.c_str());
            }
        }
    }
    m_factionSim.initialize(m_galaxy, params, m_universeSeed);
    // Missions layout is pinned to the same faction table + commodity roster
    // (Phase 8c); a save's mission block loads over this fresh state.
    // The fleet size comes from the economy as BUILT rather than from the
    // params that asked for it: with no commodities there is no economy and no
    // fleet, and an escort contract on a trader that does not exist is exactly
    // what this count is here to refuse.
    m_missions.initialize(m_galaxy,
                          sim::MissionParams{},
                          count,
                          static_cast<std::uint32_t>(m_commodityIds.size()),
                          static_cast<std::uint32_t>(m_economy.traders().size()),
                          m_universeSeed);
    m_missionEvents.clear();
    m_dockEventPending = false;
}

const char* SpaceWorld::playerAttitudeName(std::uint32_t faction) const
{
    if (faction >= m_factionTable.size()) {
        return "";
    }
    if (m_factionSim.playerHostile(faction)) {
        return "hostile";
    }
    return m_factionSim.playerFriendly(faction) ? "friendly" : "neutral";
}

// ⚑⚑⚑⚑ ONE COUNTER'S ANSWER, AND IT IS THE HALF THAT EXISTED BEFORE PHASE 37
// STAGE C. The allowlist is checked against THIS seller's def id and the
// `min_rep` against THIS seller's standing, which is what makes a gate a
// question about a relationship rather than about a station.
bool SpaceWorld::counterSells(std::uint32_t seller, const assets::CatalogGate& gate) const
{
    if (seller >= m_factionTable.size()) {
        return false;
    }
    const GameFaction& faction = m_factionTable[seller];
    if (!gate.factions.empty() &&
        std::find(gate.factions.begin(), gate.factions.end(), faction.defId) == gate.factions.end()) {
        return false;
    }
    // Pirate stations fence anything their defs allow, standing be damned
    // (docking already required non-hostile standing).
    return faction.pirate() || m_factionSim.standing(seller) >= gate.minRep;
}

// ⚑⚑⚑⚑ WHOSE COUNTER IS THIS, WHICH IS THE ONE GENUINELY NEW MECHANISM IN
// PHASE 37 AND THE THING THE SPEC SAID THE PHASE WAS FOR. This function opened
// with `systemOwnerFaction(m_currentSystem)` and had exactly one notion of who
// was selling - the holder of the SYSTEM. A shadow presence is a fact about a
// STATION: `decisions/016` chose `StationSpec::shadowOwner` precisely because
// "a station today has no owner at all", and a back room inside somebody else's
// walls is the case a per-system question cannot express. So there are two
// counters at a fence dock, and this asks both.
//
// ⚑⚑⚑ EITHER, NOT BOTH, AND THE ORDER DOES NOT MATTER. A gate that names no
// faction is a thing anybody sells and the lawful counter answers first; a gate
// that names the black market is refused by the lawful counter and taken by the
// fence. What the two owners share is nothing at all - different allowlists,
// different standings - which is exactly why one number could not carry it.
//
// ⚑⚑ THE FUNCTION STAYS ONE QUESTION, which is the bargain its own comment
// struck at Phase 32 stage D: "a condition every caller has to remember is a
// condition one of them will eventually forget, and the one that forgets would
// be a shop window listing something the counter refuses to sell." Six call
// sites ask `stationSells` and none of them had to learn what a fence is.
// `stationSellsAtFence` exists for the ONE caller that has to know which shelf
// a row belongs on, and it is a UI question rather than a permission one.
bool SpaceWorld::stationSells(const assets::CatalogGate& gate) const
{
    if (!isDocked()) {
        return false;
    }
    // ⚑⚑ FOLDED IN HERE RATHER THAN ASKED BESIDE IT AT EIGHT CALL SITES, which
    // is Phase 32 stage D's finding turned round: a condition every caller has to
    // remember is a condition one of them will eventually forget, and the one
    // that forgets would be a shop window listing something the counter refuses
    // to sell. "Will this station sell me this" is one question.
    if (!stationStocksRequirement(gate)) {
        return false;
    }
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    // ⚑ An ownerless station is an open market and always was - `sol.lantern`,
    // the one authored lawless system, is the whole of that case. It is checked
    // BEFORE the fence, so a back room in nobody's space is not asked to
    // justify itself either.
    if (owner >= m_factionTable.size()) {
        return true;
    }
    return counterSells(owner, gate) || stationSellsAtFence(gate);
}

// ⚑⚑⚑ WHETHER THE *FENCE* WOULD SELL IT, ASKED SEPARATELY BECAUSE ONE CALLER
// GENUINELY NEEDS TO KNOW AND IT IS NOT A PERMISSION QUESTION. The dock screen
// has two shelves now and a row has to go on one of them; "the lawful outfitter
// will not carry this and the back room will" is what puts it behind the
// curtain. Every other caller wants `stationSells` and the difference is none
// of their business.
//
// ⚑⚑ IT IGNORES `requiresCommodity` ON PURPOSE - `stationSells` has already
// asked, and asking twice would let a material shortage move a row from one
// shelf to the other rather than off the screen.
bool SpaceWorld::stationSellsAtFence(const assets::CatalogGate& gate) const
{
    return counterSells(dockedFenceFaction(), gate);
}

std::uint32_t SpaceWorld::dockedFenceFaction() const
{
    return isDocked() ? stationShadowOwner(m_currentSystem, dockedStationIndex()) : sim::kNoFaction;
}

// ⚑⚑⚑⚑ WHOSE LINE IS THIS, WHICH IS A DIFFERENT QUESTION FROM WHETHER THEY
// WOULD SELL IT TO YOU, AND THE DIFFERENCE IS THE STAGE'S ONE VISIBLE ROW.
// `stationSellsAtFence` folds the allowlist and the standing together; this asks
// only the first half. The Null Signature Suite names the black market and wants
// +25 with them, and player standing there is 0 until Phase 37 stage E - so the
// two answers disagree at every fence in the galaxy today. A shelf built on
// "would they sell it" alone would have been EMPTY, and an empty shelf and a
// fence with nothing to sell are the same picture.
//
// ⚑⚑ IT IS THE ALLOWLIST AND NOTHING ELSE, so it is data rather than policy: an
// item is the fence's when its def names the fence's owner. A gate that names
// nobody is carried by everybody and stays on the lawful shelf, which is why the
// empty-allowlist case returns false rather than true.
bool SpaceWorld::stationFenceCarries(const assets::CatalogGate& gate) const
{
    if (gate.factions.empty()) {
        return false;
    }
    const std::uint32_t fence = dockedFenceFaction();
    if (fence >= m_factionTable.size()) {
        return false;
    }
    return std::find(gate.factions.begin(), gate.factions.end(), m_factionTable[fence].defId) !=
           gate.factions.end();
}

bool SpaceWorld::stationStocksRequirement(const assets::CatalogGate& gate) const
{
    // ⚑⚑⚑ PHASE 33 STAGE B, AND IT IS THE LAST HOP OF THE MATERIAL TREE.
    // gdd.md 6 puts T2 components in the tree because "the gun you buy was
    // manufactured somewhere by somebody out of things somebody mined" - and
    // until this check existed the whole chain could run from the rock to a
    // warehouse and a player would never once meet it, because the outfitting
    // list was the same at every station their standing let them dock at.
    //
    // ⚑⚑ IN STOCK AT ALL, NOT A THRESHOLD. A station that has run out of hull
    // plate stops offering hull plating; the moment a hauler arrives, it offers
    // it again. A quantity here would be a second tuning surface with no
    // authored home, and 'any' is the only reading that needs no number.
    if (gate.requiresCommodity.empty()) {
        return true;
    }
    if (!isDocked()) {
        return false;
    }
    const std::uint32_t commodity = commodityIndex(gate.requiresCommodity.c_str());
    if (commodity == kNoIndex) {
        // `DefDatabase::validateCatalogGates` refuses this at load, so reaching
        // it means the commodity table and the defs came apart underneath a
        // running game. Refusing to sell is the honest answer: the requirement
        // was written and cannot be met.
        return false;
    }
    return m_economy.stock(dockedMarket(), commodity) > 0.0f;
}

bool SpaceWorld::commitFactionRaid(std::uint32_t faction, std::uint32_t targetSystem)
{
    if (!m_factionSim.commitRaid(m_galaxy, &m_economy, faction, targetSystem)) {
        return false;
    }
    if (faction < m_factionTable.size() && targetSystem < m_galaxy.systems.size()) {
        SOL_LOG_INFO("faction raid: %s hit '%s'",
                     m_factionTable[faction].name.c_str(),
                     m_galaxy.systems[targetSystem].name.c_str());
    }
    return true;
}

bool SpaceWorld::warpToStationOffset(std::uint32_t station, const core::DVec3& offset)
{
    if (isDocked() || m_currentSystem >= m_galaxy.systems.size()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (station >= spec.stations.size()) {
        return false;
    }
    clearCommand();
    Transform& transform = playerRegistry().storage<Transform>().get(playerEntityIndex());
    const core::DVec3 position = spec.stations[station].position + offset;
    transform.position = position;
    transform.previousPosition = position;
    playerRegistry().storage<FlightBody>().get(playerEntityIndex()) = FlightBody{};
    SOL_LOG_WARN("dev warp: player moved to '%s' offset (%.0f, %.0f, %.0f)",
                 spec.stations[station].name.c_str(),
                 offset.x,
                 offset.y,
                 offset.z);
    return true;
}

// ⚑⚑⚑⚑ TAKING A LEAD IS WHERE THE RELATIONSHIP MOVES, AND THE SITE IS THE
// RULING RATHER THAN A CONVENIENCE (the user, 2026-09-01: the bar and the board
// are told apart by `regard`). Completing the work would be the obvious place
// and it is the one place this cannot go: a `MissionEvent` carries a `Mission`
// snapshot, `m_active` is SAVED, and for the journal to know whose lead it was
// carrying, `Mission` would need a field - which is the second `kSaveVersion`
// bump this phase was ruled not to pay, and which stage C's own note in the
// header predicted. So it is settled in the room, at the moment it is taken,
// while the person who offered it is still standing in front of you. What the
// FINISHED work pays is faction standing, which the board already wires.
//
// ⚑ One point, and it is spent on the same currency the room reads back: the
// war lead - the one thing a room can offer that the board beside it cannot -
// asks for `kRegardForFront`. Two jobs taken with somebody is what makes you a
// regular there.
bool SpaceWorld::acceptLead(std::uint32_t leadIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("accept_lead: not docked", outError);
    }
    if (leadIndex >= m_missions.leads().size()) {
        return refuse("accept_lead: no such lead", outError);
    }
    const std::uint32_t poster = m_missions.leads()[leadIndex].poster;
    std::string error;
    if (!m_missions.acceptLead(leadIndex, m_factionSim.standing(poster), &error)) {
        return refuse("accept_lead: " + error, outError);
    }
    adjustCastRegard(castKeyAt(currentSystemIndex(), dockedStationIndex()), 1);
    return true;
}

bool SpaceWorld::acceptMission(std::uint32_t offerIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("accept_mission: not docked", outError);
    }
    if (offerIndex >= m_missions.offers().size()) {
        return refuse("accept_mission: no such offer", outError);
    }
    const std::uint32_t poster = m_missions.offers()[offerIndex].poster;
    std::string error;
    if (!m_missions.accept(offerIndex, m_factionSim.standing(poster), &error)) {
        return refuse("accept_mission: " + error, outError);
    }
    return true;
}

bool SpaceWorld::abandonMission(std::uint32_t activeIndex)
{
    return m_missions.abandon(activeIndex);
}

void SpaceWorld::processMissionDeliveries()
{
    if (!isDocked()) {
        return;
    }
    const std::uint32_t market = dockedMarket();
    for (std::uint32_t i = 0; i < m_missions.active().size();) {
        const std::size_t countBefore = m_missions.active().size();
        const sim::Mission& mission = m_missions.active()[i];
        const sim::MissionObjective& objective = mission.objectives[mission.currentObjective];
        const std::uint32_t commodity = objective.commodity;
        const std::string title = mission.title; // survives a completion
        const float available = commodity < m_playerCargo.size() ? m_playerCargo[commodity] : 0.0f;
        const float delivered = m_missions.recordDelivery(i, m_currentSystem, m_dockedStation, available);
        if (delivered > 0.0f) {
            m_playerCargo[commodity] -= delivered;
            if (market < m_economy.markets().size()) {
                m_economy.deliver(market, commodity, delivered); // fills the shortage
            }
            SOL_LOG_INFO(
                "[missions] '%s': handed in %.0f units", title.c_str(), static_cast<double>(delivered));
        }
        if (m_missions.active().size() == countBefore) {
            ++i;
        }
    }
}

void SpaceWorld::processMissionEvents()
{
    m_missionEventScratch.clear();
    m_missions.takeEvents(m_missionEventScratch);
    for (const sim::MissionEvent& event : m_missionEventScratch) {
        const sim::Mission& mission = event.mission;
        const bool posterValid = mission.poster < m_factionTable.size();
        // ⚑ An escort is the first contract that can settle itself while the
        // player is flying, with nothing on screen to say so: a haul is handed
        // in at a dock, a bounty ends on a kill they made, a Hold ends on a
        // border they can see move. This one ends when a ship somewhere else
        // lands or dies. So it speaks, once, and SHORT - the comms cell clips
        // at roughly 45 characters (8r/8s).
        const bool escort = event.objective < mission.objectives.size() &&
                            mission.objectives[event.objective].kind == sim::ObjectiveKind::Escort;
        const auto announce = [&](const char* line) {
            if (escort && !isDocked()) {
                say(kFleetcom, line);
            }
        };
        switch (event.kind) {
        case sim::MissionEventKind::Accepted:
            SOL_LOG_INFO("[missions] accepted '%s': %s",
                         mission.title.c_str(),
                         mission.objectives.front().text.c_str());
            break;
        case sim::MissionEventKind::ObjectiveComplete:
            if (event.objective + 1 < mission.objectives.size()) {
                SOL_LOG_INFO("[missions] '%s': %s",
                             mission.title.c_str(),
                             mission.objectives[event.objective + 1].text.c_str());
            }
            break;
        case sim::MissionEventKind::Completed:
            m_playerCredits += mission.rewardCredits;
            if (posterValid) {
                m_factionSim.addStanding(mission.poster, mission.standingReward);
            }
            SOL_LOG_INFO("[missions] completed '%s': +%.0f cr, %s +%.1f rep",
                         mission.title.c_str(),
                         mission.rewardCredits,
                         posterValid ? m_factionTable[mission.poster].name.c_str() : "?",
                         static_cast<double>(mission.standingReward));
            announce("Our hauler is docked. Contract paid.");
            break;
        case sim::MissionEventKind::Lost:
            // The contest resolved against the side this contract named
            // (Phase 8u), or the hauler it named was destroyed by somebody
            // else (Phase 8x). No standing penalty either way, deliberately:
            // the player flew the battle and lost it, which is not the same as
            // letting a deadline run out - the unfairness Phase 8l recorded
            // and could not fix inside its own scope.
            SOL_LOG_WARN("[missions] lost '%s': %s (no penalty)",
                         mission.title.c_str(),
                         escort ? "the hauler was destroyed" : "the system fell");
            announce("We lost the hauler. Stand down.");
            break;
        case sim::MissionEventKind::Failed:
        case sim::MissionEventKind::Abandoned:
            // Campaign missions charge nothing (decisions/008: the spine is
            // ignorable); procedural contracts dock standing with the poster.
            if (!mission.campaign() && posterValid) {
                m_factionSim.addStanding(mission.poster, -mission.standingPenalty);
            }
            SOL_LOG_WARN("[missions] %s '%s'%s",
                         event.kind == sim::MissionEventKind::Failed ? "failed" : "abandoned",
                         mission.title.c_str(),
                         mission.campaign() ? " (campaign: no penalty)" : "");
            // Deliberately neutral: an escort reaches Failed either by running
            // out of clock or because the player shot their own charge (Phase
            // 8x §E), and the event carries no way to tell those apart. A line
            // that named the betrayal would be a lie half the time it fired,
            // and the standing charge above is the part that does the talking.
            if (event.kind == sim::MissionEventKind::Failed) {
                announce("Escort contract void.");
            }
            break;
        }
        m_missionEvents.push_back(event);
    }
}

void SpaceWorld::takeMissionEvents(std::vector<sim::MissionEvent>& out)
{
    out.insert(out.end(), m_missionEvents.begin(), m_missionEvents.end());
    m_missionEvents.clear();
}

// --- Exploration & scanning (Phase 8e) ---------------------------------------

void SpaceWorld::initializeSurvey()
{
    m_surveyParams = sim::SurveyParams{};
    m_survey.initialize(
        m_galaxy, m_surveyParams, static_cast<std::uint32_t>(m_commodityIds.size()), m_universeSeed);
    m_surveyEvents.clear();
    m_signals.clear();
    m_dynamicTargets.clear();
    m_pulseCooldown = 0.0;
    m_scanProgress = 0.0f;
    m_scanActive = false;
}

namespace {

// Defined with the outfitting helpers further down: salvaging a component runs
// through the same fit validation a purchase does.
[[nodiscard]] std::vector<sol::assets::FittedMount>
fitMounts(const assets::DefDatabase& defs, const assets::ShipDef& base, const OwnedShip& ship);
[[nodiscard]] std::vector<const assets::CrewDef*> fitCrew(const assets::DefDatabase& defs,
                                                          const OwnedShip& ship);

} // namespace

// ⚑ One anonymous name for everything unidentified, and one stable ordinal
// behind it (Phase 8z). A contact is deliberately not told apart by its label:
// a station, a gate and a derelict all read "Contact 4" until a scan says
// otherwise, which is what makes identifying one worth the flight.
//
// The ordinal is the object's own position in the system's fixed
// [stations, gates, signals] order rather than the order it was found in, so a
// contact's designation never changes under the player when a *different* one
// is identified. That is a small change to how sites were numbered before 8z
// and strictly an improvement: "Contact 3" now stays Contact 3.
std::string anonymousContactName(std::size_t ordinal)
{
    return "Contact " + std::to_string(ordinal + 1);
}

std::string signalTargetName(sim::SignalKind kind, bool resolved, bool emptied, std::size_t ordinal)
{
    if (!resolved) {
        return anonymousContactName(ordinal);
    }
    std::string name = kind == sim::SignalKind::Derelict ? "Derelict Hull" : "Supply Cache";
    if (emptied) {
        name += " (empty)";
    }
    return name;
}

const sim::MissionObjective* SpaceWorld::trackedObjective() const
{
    const std::vector<sim::Mission>& active = m_missions.active();
    if (m_missions.tracked() >= active.size()) {
        return nullptr;
    }
    const sim::Mission& mission = active[m_missions.tracked()];
    return mission.currentObjective < mission.objectives.size()
               ? &mission.objectives[mission.currentObjective]
               : nullptr;
}

bool SpaceWorld::traderBodyPosition(std::uint32_t traderIndex, core::DVec3* out) const
{
    const ecs::Pool<TraderPuppet>& puppets = playerRegistry().storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        if (puppets.values()[i].traderIndex != traderIndex) {
            continue;
        }
        const Transform* transform = playerRegistry().storage<Transform>().tryGet(puppets.entityIndices()[i]);
        if (transform == nullptr) {
            return false;
        }
        if (out != nullptr) {
            *out = transform->position;
        }
        return true;
    }
    return false;
}

bool SpaceWorld::objectiveMarker(ObjectiveMarker* out) const
{
    const sim::MissionObjective* objective = trackedObjective();
    if (objective == nullptr) {
        return false;
    }
    if (objective->kind == sim::ObjectiveKind::FlyTo) {
        if (objective->system != m_currentSystem) {
            return false;
        }
        if (out != nullptr) {
            *out = {.position = objective->position, .radius = objective->radius};
        }
        return true;
    }
    // An escort's marker is the hauler, and only while the hauler is here.
    // ⚑ Its `system` is deliberately NOT consulted: that is where the haul
    // ENDS, and the whole job is flying with the ship somewhere else. Presence
    // of a body is the test, which is also the honest one — there is nothing
    // to point at while the trader is in the gate network.
    if (objective->kind == sim::ObjectiveKind::Escort) {
        core::DVec3 position;
        if (!traderBodyPosition(objective->trader, &position)) {
            return false;
        }
        if (out != nullptr) {
            *out = {.position = position, .radius = 0.0, .moving = true};
        }
        return true;
    }
    return false;
}

std::string SpaceWorld::objectiveDestinationText() const
{
    const sim::MissionObjective* objective = trackedObjective();
    if (objective == nullptr) {
        return {};
    }
    const auto systemName = [&](std::uint32_t system) -> std::string {
        return system < m_galaxy.systems.size() ? m_galaxy.systems[system].name
                                                : std::string("an unknown system");
    };
    const bool here = objective->system == m_currentSystem;
    switch (objective->kind) {
    case sim::ObjectiveKind::FlyTo:
        // In this system it already has a marker, a radar dot and a nav slot.
        return here ? std::string() : systemName(objective->system);
    case sim::ObjectiveKind::Dock:
    case sim::ObjectiveKind::Deliver: {
        std::string where = "a station";
        if (objective->system < m_galaxy.systems.size()) {
            const std::vector<sim::StationSpec>& stations = m_galaxy.systems[objective->system].stations;
            if (objective->station < stations.size()) {
                where = stations[objective->station].name;
            }
        }
        return here ? where : where + ", " + systemName(objective->system);
    }
    case sim::ObjectiveKind::Kill: {
        const std::string victim = objective->faction < m_factionTable.size()
                                       ? m_factionTable[objective->faction].name
                                       : std::string("anyone");
        if (objective->system == sim::kAnySystem) {
            return victim + ", anywhere";
        }
        return here ? victim + ", here" : victim + ", " + systemName(objective->system);
    }
    case sim::ObjectiveKind::Hold:
        // A contest has no position and so gets no marker at all: the prose
        // is the only thing that can say where the fight is (Phase 8i's rule,
        // arriving at the one objective kind that needs it most).
        return here ? std::string("here") : systemName(objective->system);
    case sim::ObjectiveKind::Escort: {
        // Two facts, and the second is the one a marker cannot give: where the
        // haul ends, and whether the ship being escorted is in this sky at all.
        // A hauler in the gate network is neither here nor lost, and without
        // this the HUD would simply go quiet while the mission was running.
        const std::string destination = here ? std::string("here") : systemName(objective->system);
        return traderBodyPosition(objective->trader, nullptr) ? destination : destination + " (in transit)";
    }
    }
    return {};
}

void SpaceWorld::syncObjectiveTarget()
{
    ObjectiveMarker marker;
    const bool want = objectiveMarker(&marker);
    const std::size_t slot = objectiveTargetIndex();
    const bool have = slot != kNoTarget;
    if (want == have) {
        // The position matters as much as the presence: when one FlyTo
        // completes and the next is also in this system the slot stays put and
        // only moves, which is what carries a selection (and an engaged
        // autopilot) to the next leg instead of dropping it.
        //
        // Written in place rather than rebuilt, because an escort's marker is
        // a ship under way: it moves EVERY tick, and a rebuild per tick would
        // re-lay the whole target tail (and its strings) for a slot whose only
        // changing field is three doubles.
        if (want && m_targets[slot].position != marker.position) {
            m_targets[slot].position = marker.position;
        }
        return;
    }
    rebuildDynamicTargets();
}

void SpaceWorld::rebuildDynamicTargets()
{
    // Slots are append-only in discovery order: a scan in flight and the
    // player's target index both point into this tail, so nothing may shift
    // under them when a later pulse finds a lower-numbered site or a fight
    // leaves a new wreck. Only a slot whose object is gone (a wreck that
    // decayed) is removed, and the indices that pointed past it follow.
    auto hasSlot = [&](NavKind kind, std::uint32_t index) {
        for (const DynamicTarget& slot : m_dynamicTargets) {
            if (slot.kind == kind && slot.index == index) {
                return true;
            }
        }
        return false;
    };

    for (const SignalInstance& signal : m_signals) {
        if (m_survey.signalDiscovered(m_currentSystem, signal.index) &&
            !hasSlot(NavKind::Signal, signal.index)) {
            m_dynamicTargets.push_back({.kind = NavKind::Signal, .index = signal.index});
        }
    }
    // Fields need no finding: an asteroid field is a visible thing in the
    // playfield, and it is the field you fly to, not the individual rock.
    for (std::uint32_t i = 0; i < m_fields.size(); ++i) {
        if (!hasSlot(NavKind::Field, i)) {
            m_dynamicTargets.push_back({.kind = NavKind::Field, .index = i});
        }
    }
    std::vector<std::uint32_t> wreckIds;
    m_mining.wrecksIn(m_currentSystem, wreckIds);
    for (const std::uint32_t id : wreckIds) {
        if (!hasSlot(NavKind::Wreck, id)) {
            m_dynamicTargets.push_back({.kind = NavKind::Wreck, .index = id});
        }
    }
    // Bookmarks (Phase 8h) key on their SurveySim id, not their ledger index,
    // so deleting one in another system cannot renumber a slot here.
    std::vector<std::uint32_t> bookmarkIds;
    m_survey.bookmarksIn(m_currentSystem, bookmarkIds);
    for (const std::uint32_t id : bookmarkIds) {
        if (!hasSlot(NavKind::Bookmark, id)) {
            m_dynamicTargets.push_back({.kind = NavKind::Bookmark, .index = id});
        }
    }
    // The tracked mission's destination (Phase 8i). There is at most one, so
    // the slot carries no index of its own: it exists while the tracked
    // objective is a FlyTo here, and its position is re-read below every
    // rebuild. That is deliberate — when one leg completes and the next is
    // also in this system, the marker moves to the new waypoint and a live
    // selection (and an engaged autopilot) carry over instead of dropping.
    const bool wantObjective = objectiveMarker(nullptr);
    if (wantObjective && !hasSlot(NavKind::Objective, 0)) {
        m_dynamicTargets.push_back({.kind = NavKind::Objective, .index = 0});
    }
    // The berth a station has just cleared you for (Phase 8r). One slot,
    // indexless, exactly the shape the objective above takes and for the same
    // reason 8i gave: a nav slot buys the radar blip, the target cycle, the map
    // marker and Autopilot in one move rather than four. It lives as long as
    // the clearance does, and when the clearance ends the compaction below
    // carries the selection off it — including disengaging an autopilot that
    // was still flying to it, which is the bug 8i found and fixed here.
    const bool wantBerth = hasClearance();
    if (wantBerth && !hasSlot(NavKind::Berth, 0)) {
        m_dynamicTargets.push_back({.kind = NavKind::Berth, .index = 0});
    }

    // Compact slots whose object is gone, carrying the player's selection and
    // any scan in flight with them.
    std::size_t write = 0;
    for (std::size_t read = 0; read < m_dynamicTargets.size(); ++read) {
        const DynamicTarget& slot = m_dynamicTargets[read];
        const bool alive = slot.kind == NavKind::Signal      ? slot.index < m_signals.size()
                           : slot.kind == NavKind::Field     ? slot.index < m_fields.size()
                           : slot.kind == NavKind::Bookmark  ? m_survey.bookmark(slot.index) != nullptr
                           : slot.kind == NavKind::Objective ? wantObjective
                           : slot.kind == NavKind::Berth     ? wantBerth
                                                             : m_mining.wreck(slot.index) != nullptr;
        if (!alive) {
            const std::size_t removed = m_signalTargetBase + write;
            // The selection itself going away is not a reason to fly to
            // whatever slid into its slot. Found flying the objective: the
            // FlyTo completed at its radius while autopilot was still closing,
            // the slot vanished, and the ship carried on to a neutral
            // interceptor four kilometres away without being asked (Phase 8i).
            if (m_targetIndex == removed && commandNeedsTarget(m_commandMode)) {
                SOL_LOG_INFO("%s: disengaged, the destination is gone", commandModeName(m_commandMode));
                clearCommand();
            }
            if (m_targetIndex > removed) {
                --m_targetIndex;
            }
            if (m_scanActive && m_scanTarget > removed) {
                --m_scanTarget;
            }
            continue;
        }
        m_dynamicTargets[write++] = slot;
    }
    m_dynamicTargets.resize(write);

    if (m_targets.size() > m_signalTargetBase) {
        m_targets.resize(m_signalTargetBase);
    }
    for (const DynamicTarget& slot : m_dynamicTargets) {
        switch (slot.kind) {
        case NavKind::Signal: {
            const SignalInstance& signal = m_signals[slot.index];
            // The contact ordinal continues the static head's numbering, and
            // m_planetTargetBase is exactly the count of stations plus gates
            // that came before it.
            m_targets.push_back(
                {.name = signalTargetName(signal.kind,
                                          m_survey.signalResolved(m_currentSystem, slot.index),
                                          m_survey.signalEmptied(m_currentSystem, slot.index),
                                          m_planetTargetBase + slot.index),
                 .position = signal.position,
                 .surfaceRadius = 0.0});
            break;
        }
        case NavKind::Field: {
            const sim::AsteroidFieldSpec& field = m_fields[slot.index];
            m_targets.push_back({.name = "Asteroid Field " + std::to_string(slot.index + 1),
                                 .position = field.center,
                                 .surfaceRadius = 0.0});
            break;
        }
        case NavKind::Bookmark: {
            const sim::Bookmark* bookmark = m_survey.bookmark(slot.index);
            m_targets.push_back(
                {.name = "* " + bookmark->name, .position = bookmark->position, .surfaceRadius = 0.0});
            break;
        }
        case NavKind::Objective: {
            // One glyph of prefix, as bookmarks take, so the cycle says what a
            // slot is without a legend. Deliberately NOT the objective's own
            // text: that is a sentence of mission prose ("Fly to the
            // calibration beacon (8 km out)"), and as a target name it
            // overran the map's name column, collided with its neighbour's
            // label on the map itself, and was truncated in the HUD readout.
            // The prose belongs on the mission line, which already carries it.
            ObjectiveMarker marker;
            (void)objectiveMarker(&marker);
            // Named apart because it behaves apart: a waypoint sits still and
            // a charge under escort does not, and a pilot cycling targets is
            // owed that distinction in the one word the column has room for.
            m_targets.push_back({.name = marker.moving ? "> Escort" : "> Objective",
                                 .position = marker.position,
                                 .surfaceRadius = 0.0});
            break;
        }
        case NavKind::Berth: {
            // Short, like the objective's name and for the same reason: the
            // station it belongs to is named on the comms line that assigned
            // it, and a name long enough to repeat that would overrun the
            // map's name column into the detail beside it.
            m_targets.push_back({.name = "Berth " + std::to_string(m_clearance.berth + 1),
                                 .position = clearedBerthPoint(),
                                 .surfaceRadius = 0.0});
            break;
        }
        default: {
            const sim::WreckRecord* wreck = m_mining.wreck(slot.index);
            m_targets.push_back(
                {.name = "Wreck: " + wreck->name, .position = wreck->position, .surfaceRadius = 0.0});
            break;
        }
        }
    }
}

std::uint32_t SpaceWorld::targetSignalIndex() const
{
    const std::size_t total = m_targets.size() + playerShips().size();
    if (total == 0) {
        return kNoIndex;
    }
    const std::size_t index = m_targetIndex % total;
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slot.kind == NavKind::Signal ? slot.index : kNoIndex;
}

std::uint32_t SpaceWorld::targetBodyIndex() const
{
    const std::size_t total = m_targets.size() + playerShips().size();
    if (total == 0) {
        return kNoIndex;
    }
    const std::size_t index = m_targetIndex % total;
    if (index == m_starTargetIndex) {
        return 0; // body 0 is the star, matching SurveySim's numbering
    }
    if (index >= m_planetTargetBase && index < m_planetTargetBase + planets().size()) {
        return static_cast<std::uint32_t>(index - m_planetTargetBase + 1);
    }
    return kNoIndex;
}

float SpaceWorld::pulseCharge() const
{
    if (m_pulseCooldown <= 0.0) {
        return 1.0f;
    }
    return static_cast<float>(1.0 - m_pulseCooldown / kPulseCooldownSeconds);
}

const char* SpaceWorld::scanTargetName() const
{
    if (!m_scanActive || m_scanTarget >= m_targets.size()) {
        return "";
    }
    return m_targets[m_scanTarget].name.c_str();
}

int SpaceWorld::pulseScan()
{
    if (isDocked() || m_pulseCooldown > 0.0) {
        return -1;
    }
    m_pulseCooldown = kPulseCooldownSeconds;
    const core::DVec3 position = shipState().position;
    const double range = static_cast<double>(m_scanRange);
    int found = 0;
    for (const SignalInstance& signal : m_signals) {
        if (m_survey.signalDiscovered(m_currentSystem, signal.index) ||
            length(signal.position - position) > range) {
            continue;
        }
        if (m_survey.notifySignalDiscovered(m_currentSystem, signal.index)) {
            ++found;
            m_surveyEvents.push_back({.kind = SurveyEvent::Kind::SignalDiscovered,
                                      .system = m_currentSystem,
                                      .index = signal.index,
                                      .signalKind = signal.kind,
                                      .seed = signal.seed,
                                      .name = "contact"});
        }
    }
    if (found > 0) {
        rebuildDynamicTargets();
    }
    // ⚑ Stations and gates answer to the same sweep (Phase 8z). They are not
    // added to the target list here — they have been in it since the system
    // loaded, because it is world state NPCs anchor to — only un-hidden, which
    // is the whole point of the fog being a predicate rather than a filter.
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    int stations = 0;
    int beacons = 0;
    for (std::uint32_t i = 0; i < spec.stations.size(); ++i) {
        if (length(spec.stations[i].position - position) <= range &&
            m_survey.notifyStationDiscovered(m_galaxy, m_currentSystem, i)) {
            ++stations;
        }
    }
    // ⚑ A GATE IS FOUND AT ANY RANGE INSIDE THE SYSTEM, and the exception is
    // the whole reason the phase is playable. A gate is a massive powered
    // structure running a lane beacon; a derelict is a cold hull and a station
    // is somewhere in between. Without this a new pilot cannot leave: gates sit
    // at 6.0e8 m from the hub, stations at 1.0e8-4.0e8, and the starter pulse
    // reaches 2.5e8 — measured live, the nearest gate to any of Lyrioa's three
    // stations was 278,512 km against a 250,000 km reach, and a fresh game has
    // no arrival gate to leave by. Everywhere else you arrive THROUGH a gate,
    // so you can always go back the way you came; the start system is the one
    // place that is not true, which is exactly where a new player is.
    //
    // What identifying a gate buys is unchanged: a beacon says "a gate is
    // here", not where it goes. You still fly to it to learn that.
    for (std::uint32_t i = 0; i < m_gates.size(); ++i) {
        if (m_survey.notifyGateDiscovered(m_galaxy, m_currentSystem, i)) {
            ++beacons;
        }
    }
    const int structures = stations + beacons;
    if (structures > 0) {
        refreshStaticTargetNames();
    }
    // Two numbers, because they answer to two different rules and one figure
    // covering both would be a lie: the drive read "5 new contact(s) within
    // 250000 km" for a sweep whose gates were 813,507 km away.
    SOL_LOG_INFO(
        "scan pulse: %d within %.0f km, %d gate beacon(s)", found + stations, range / 1000.0, beacons);
    found += structures;
    return found;
}

void SpaceWorld::tickScanning(double dt)
{
    if (m_pulseCooldown > 0.0) {
        m_pulseCooldown -= dt;
        if (m_pulseCooldown < 0.0) {
            m_pulseCooldown = 0.0;
        }
    }
    const auto stopScan = [&]() {
        m_scanActive = false;
        m_scanProgress = 0.0f;
    };
    if (isDocked()) {
        stopScan();
        return;
    }
    const std::size_t total = m_targets.size() + playerShips().size();
    if (total == 0) {
        stopScan();
        return;
    }
    const std::size_t index = m_targetIndex % total;
    const std::uint32_t signalIndex = targetSignalIndex();
    const std::uint32_t bodyIndex = targetBodyIndex();
    const bool scannableSignal =
        signalIndex != kNoIndex && !m_survey.signalResolved(m_currentSystem, signalIndex);
    const bool scannableBody = bodyIndex != kNoIndex && !m_survey.bodyScanned(m_currentSystem, bodyIndex);
    // Phase 8z: a discovered-but-unidentified station or gate is the third
    // scannable thing. Hidden ones cannot be selected at all, so reaching here
    // with one is impossible rather than guarded against.
    const bool scannableStructure = navKnowledge(index) == NavKnowledge::Contact;
    if (!scannableSignal && !scannableBody && !scannableStructure) {
        stopScan();
        return;
    }

    const sim::ShipState state = shipState();
    const core::DVec3 toTarget = m_targets[index].position - state.position;
    const double distance = length(toTarget);
    // Sites must be approached; bodies are read at whatever range they sit at
    // (they are AU-scale scenery — a survey scan of a planet is a telescope
    // pointed at it, not a flyby).
    //
    // ⚑ A station or a gate is approach-gated like a SITE, and that is the
    // choice the whole phase rests on. Inherit the body's rule instead and the
    // player pulses once from the arrival gate and identifies an entire system
    // without flying anywhere — which is exactly the complaint 8z exists to
    // answer. Identification costs the flight.
    if ((scannableSignal || scannableStructure) && distance > targetScanRange()) {
        stopScan();
        return;
    }
    if (distance > 1.0) {
        const core::Vec3 forwardF = rotate(state.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
        const core::DVec3 direction = toTarget * (1.0 / distance);
        const double aim = static_cast<double>(forwardF.x) * direction.x +
                           static_cast<double>(forwardF.y) * direction.y +
                           static_cast<double>(forwardF.z) * direction.z;
        if (aim < kScanConeCosine) {
            stopScan(); // scanning is a held aim, not a checkbox
            return;
        }
    }

    if (!m_scanActive || m_scanTarget != index) {
        m_scanTarget = index;
        m_scanActive = true;
        m_scanProgress = 0.0f;
    }
    m_scanProgress += static_cast<float>(dt * static_cast<double>(m_scanSpeed) / kTargetScanSeconds);
    if (m_scanProgress < 1.0f) {
        return;
    }
    stopScan();
    if (scannableSignal) {
        if (m_survey.notifySignalResolved(m_galaxy, m_currentSystem, signalIndex)) {
            const SignalInstance& signal = m_signals[signalIndex];
            // Scriptless default first, so a site always holds something even
            // if no script answers; the Lua hook may replace it this frame.
            (void)m_survey.setLoot(m_currentSystem, signalIndex, defaultLoot(signal));
            m_surveyEvents.push_back({.kind = SurveyEvent::Kind::SignalResolved,
                                      .system = m_currentSystem,
                                      .index = signalIndex,
                                      .signalKind = signal.kind,
                                      .seed = signal.seed,
                                      .name = sim::signalKindName(signal.kind)});
            rebuildDynamicTargets();
            SOL_LOG_INFO("scan resolved: %s", sim::signalKindName(signal.kind));
        }
        return;
    }
    if (scannableStructure) {
        if (identifyStructure(index)) {
            SOL_LOG_INFO("scan resolved: %s", m_targets[index].name.c_str());
        }
        return;
    }
    if (m_survey.notifyBodyScanned(m_galaxy, m_currentSystem, bodyIndex)) {
        m_surveyEvents.push_back({.kind = SurveyEvent::Kind::BodyScanned,
                                  .system = m_currentSystem,
                                  .index = bodyIndex,
                                  .signalKind = sim::SignalKind::Derelict,
                                  .seed = 0,
                                  .name = m_targets[index].name});
        SOL_LOG_INFO("scan resolved: %s", m_targets[index].name.c_str());
    }
}

bool SpaceWorld::scanCurrentTarget()
{
    const std::uint32_t signalIndex = targetSignalIndex();
    if (signalIndex != kNoIndex) {
        if (!m_survey.notifySignalResolved(m_galaxy, m_currentSystem, signalIndex)) {
            return false;
        }
        const SignalInstance& signal = m_signals[signalIndex];
        (void)m_survey.setLoot(m_currentSystem, signalIndex, defaultLoot(signal));
        m_surveyEvents.push_back({.kind = SurveyEvent::Kind::SignalResolved,
                                  .system = m_currentSystem,
                                  .index = signalIndex,
                                  .signalKind = signal.kind,
                                  .seed = signal.seed,
                                  .name = sim::signalKindName(signal.kind)});
        rebuildDynamicTargets();
        return true;
    }
    // Phase 8z: the same lever finishes a structure scan, through the same
    // choke point the held scan uses. A console shortcut that identified a
    // station by its own route would be a second implementation (8u).
    const std::size_t total = m_targets.size() + playerShips().size();
    if (total > 0 && identifyStructure(m_targetIndex % total)) {
        return true;
    }
    const std::uint32_t bodyIndex = targetBodyIndex();
    if (bodyIndex == kNoIndex || !m_survey.notifyBodyScanned(m_galaxy, m_currentSystem, bodyIndex)) {
        return false;
    }
    m_surveyEvents.push_back({.kind = SurveyEvent::Kind::BodyScanned,
                              .system = m_currentSystem,
                              .index = bodyIndex,
                              .signalKind = sim::SignalKind::Derelict,
                              .seed = 0,
                              .name = ""});
    return true;
}

sim::SignalLoot SpaceWorld::defaultLoot(const SignalInstance& signal) const
{
    sim::SignalLoot loot;
    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());
    if (commodityCount == 0) {
        return loot;
    }
    core::Rng rng(signal.seed, 7);
    const std::uint32_t stacks = signal.kind == sim::SignalKind::Derelict ? 1 + rng.range(2) : 1;
    for (std::uint32_t i = 0; i < stacks; ++i) {
        // ⚑⚑ THE SPEC NAMED ONE SITE OF THE UNIFORM ROLL AND THERE WERE TWO.
        // This one is 600 lines up and has the identical defect, so it gets the
        // identical fix - with the assembly bound OPEN, which is the one place
        // the two composers differ and it is a difference about what the thing
        // is: a derelict is a large abandoned ship and a cache is somebody's
        // stash, and a module kit is a believable thing to find in either. A
        // wreck has a hull to be measured against; a signal does not.
        const std::uint32_t hauled = rollHauledCommodity(rng, /*canCarryAssemblies=*/true);
        if (hauled >= commodityCount) {
            continue;
        }
        loot.cargo.push_back({.commodity = hauled, .units = static_cast<float>(5 + rng.range(21))});
    }
    if (signal.kind == sim::SignalKind::Cache) {
        loot.credits = 200.0 + 1'000.0 * rng.nextDouble01();
    } else if (m_defs != nullptr && !m_defs->components().empty() && rng.nextFloat01() < 0.25f) {
        const std::vector<assets::ComponentDef>& components = m_defs->components();
        loot.componentId = components[rng.range(static_cast<std::uint32_t>(components.size()))].id;
    }
    return loot;
}

bool SpaceWorld::applySignalLoot(std::uint32_t system, std::uint32_t signal, sim::SignalLoot loot)
{
    return m_survey.setLoot(system, signal, std::move(loot));
}

void SpaceWorld::takeSurveyEvents(std::vector<SurveyEvent>& out)
{
    out.insert(out.end(), m_surveyEvents.begin(), m_surveyEvents.end());
    m_surveyEvents.clear();
}

double SpaceWorld::nearestSalvageDistance() const
{
    if (isDocked()) {
        return -1.0;
    }
    const core::DVec3 position = shipState().position;
    double nearest = -1.0;
    for (const SignalInstance& signal : m_signals) {
        if (!m_survey.signalResolved(m_currentSystem, signal.index) ||
            m_survey.signalEmptied(m_currentSystem, signal.index)) {
            continue;
        }
        const double distance = length(signal.position - position);
        if (nearest < 0.0 || distance < nearest) {
            nearest = distance;
        }
    }
    return nearest;
}

bool SpaceWorld::trySalvageNearest(double range)
{
    if (isDocked()) {
        return false;
    }
    const core::DVec3 position = shipState().position;
    const SignalInstance* best = nullptr;
    double bestDistance = range;
    for (const SignalInstance& signal : m_signals) {
        if (!m_survey.signalResolved(m_currentSystem, signal.index) ||
            m_survey.signalEmptied(m_currentSystem, signal.index)) {
            continue;
        }
        const double distance = length(signal.position - position);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = &signal;
        }
    }
    if (best == nullptr) {
        return false;
    }
    const sim::SignalLoot* stored = m_survey.loot(m_currentSystem, best->index);
    sim::SignalLoot remaining = stored != nullptr ? *stored : sim::SignalLoot{};

    const double credits = remaining.credits;
    m_playerCredits += credits;
    remaining.credits = 0.0;

    float unitsTaken = 0.0f;
    std::vector<sim::SignalCargo> left;
    for (const sim::SignalCargo& stack : remaining.cargo) {
        const float space = m_playerCargoCapacity - playerCargoTotal();
        const float take = std::min(stack.units, space > 0.0f ? space : 0.0f);
        if (take > 0.0f && stack.commodity < m_playerCargo.size()) {
            m_playerCargo[stack.commodity] += take;
            unitsTaken += take;
        }
        if (stack.units - take > 0.001f) {
            left.push_back({.commodity = stack.commodity, .units = stack.units - take});
        }
    }
    remaining.cargo = std::move(left);

    std::string componentTaken;
    if (tryFitSalvagedComponent(remaining.componentId, componentTaken)) {
        remaining.componentId.clear();
    }

    const bool empty = remaining.cargo.empty() && remaining.componentId.empty();
    if (empty) {
        (void)m_survey.notifySignalEmptied(m_currentSystem, best->index);
    } else {
        (void)m_survey.setLoot(m_currentSystem, best->index, remaining);
    }
    rebuildDynamicTargets();
    SOL_LOG_INFO("salvaged %s: %.0f units, %.0f cr%s%s",
                 sim::signalKindName(best->kind),
                 static_cast<double>(unitsTaken),
                 credits,
                 componentTaken.empty() ? "" : ", fitted ",
                 componentTaken.c_str());
    if (!empty) {
        SOL_LOG_INFO("salvage: no room for the rest - it stays aboard the wreck");
    }
    return true;
}

double SpaceWorld::sellSurveyData()
{
    if (!isDocked()) {
        return 0.0;
    }
    const double credits = m_survey.sellLedger();
    if (credits <= 0.0) {
        return 0.0;
    }
    m_playerCredits += credits;
    // Data is commerce: the buyer's faction warms to you like any trade.
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner < m_factionTable.size()) {
        m_factionSim.addStanding(owner, static_cast<float>(credits * kSurveyStandingRate));
    }
    SOL_LOG_INFO("sold survey data for %.0f cr", credits);
    return credits;
}

bool SpaceWorld::plotRoute(std::uint32_t destination)
{
    if (destination >= m_galaxy.systems.size() || destination == m_currentSystem) {
        m_survey.clearRoute();
        return false;
    }
    m_survey.setRoute(sim::routeBetween(m_galaxy, m_currentSystem, destination));
    return !m_survey.route().empty();
}

// --- Mining, salvage & refining (Phase 8f) -----------------------------------

namespace {

// Display name for a commodity id, falling back to the id when the def has
// gone (a mod may drop a commodity a save still carries in the hold).
[[nodiscard]] std::string commodityDisplayName(const assets::DefDatabase& defs, const std::string& id)
{
    const assets::CommodityDef* def = defs.findCommodity(id.c_str());
    return def != nullptr ? def->name : id;
}

} // namespace

bool SpaceWorld::tryFitSalvagedComponent(const std::string& componentId, std::string& outName)
{
    outName.clear();
    if (componentId.empty() || m_defs == nullptr || m_fleet.empty()) {
        return false;
    }
    const assets::ComponentDef* component = m_defs->findComponent(componentId.c_str());
    const assets::ShipDef* base = m_defs->findShip(m_fleet[m_activeShip].defId.c_str());
    if (component == nullptr || base == nullptr) {
        return true; // the def is gone; there is nothing left to salvage
    }
    // ⚑ Salvage takes the first EMPTY mount that accepts it and never swaps.
    // Buying is a decision the player made; a component floating out of a
    // wreck is not, and silently selling the shield booster they fitted to
    // make room for one they found would be theft dressed as a convenience.
    OwnedShip candidate = m_fleet[m_activeShip];
    std::string target;
    for (const assets::ShipMount& mount : base->mounts) {
        if (candidate.fittingAt(mount.id) == nullptr &&
            assets::mountAccepts(mount, component->mount, component->size)) {
            target = mount.id;
            break;
        }
    }
    if (target.empty()) {
        return false; // no free mount takes it: it stays in the wreck
    }
    candidate.fittings.push_back({.mountId = target, .defId = component->id});
    if (!assets::validateLoadout(*base, fitMounts(*m_defs, *base, candidate), fitCrew(*m_defs, candidate))) {
        return false; // over the power budget: it stays put
    }
    m_fleet[m_activeShip] = std::move(candidate);
    applyActiveLoadout();
    outName = component->name;
    return true;
}

void SpaceWorld::ensureWorldPools(ecs::Registry& registry)
{
    // Through the schema rather than by naming pools here, because the schema
    // is already the list of what the world keeps and a second copy of that
    // list would drift from it (Phase 38 stage A). The two transient pools are
    // named beside it: a puppet is rebuilt from the coarse record and is
    // deliberately NOT serialised, so the schema does not know about it - and
    // the const probes read both pools.
    makeSnapshotSchema().ensurePools(registry);
    (void)registry.storage<TraderPuppet>();
    (void)registry.storage<MinerPuppet>();
    (void)registry.storage<CaptainPuppet>(); // Phase 39 stage B, same footing
}

bool SpaceWorld::bubbleReportAt(std::size_t slot, BubbleReport& out) const
{
    if (slot >= m_bubbles.size()) {
        return false;
    }
    const SystemBubble& bubble = *m_bubbles[slot];
    const ecs::Registry& registry = bubble.registry;
    out = BubbleReport{};
    out.system = bubble.system;
    out.entities = registry.aliveCount();
    out.ships = registry.storage<ShipPilot>().size();
    out.projectiles = registry.storage<Projectile>().size();
    out.wrecks = registry.storage<WreckMarker>().size();
    out.player = registry.storage<PlayerShip>().size() == 1;
    out.holdSeconds = bubble.holdSeconds;
    out.starRadius = bubble.star.radius;
    out.planets = bubble.planets.size();
    const ecs::Pool<ShipDefense>& defenses = registry.storage<ShipDefense>();
    const ecs::Pool<ShipPilot>& pilots = registry.storage<ShipPilot>();
    const ecs::Pool<Transform>& transforms = registry.storage<Transform>();
    bool any = false;
    std::size_t placed = 0;
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const std::uint32_t entityIndex = pilots.entityIndices()[i];
        if (pilots.values()[i].state == PilotState::Attack) {
            ++out.fighting;
        }
        if (const Transform* transform = transforms.tryGet(entityIndex)) {
            out.shipCentroid = out.shipCentroid + transform->position;
            ++placed;
        }
        const ShipDefense* defense = defenses.tryGet(entityIndex);
        if (defense == nullptr || defense->tuning.hull <= 0.0f) {
            continue;
        }
        const float fraction = defense->state.hull / defense->tuning.hull;
        out.worstHull = any ? std::min(out.worstHull, fraction) : fraction;
        any = true;
    }
    if (placed > 0) {
        out.shipCentroid = out.shipCentroid * (1.0 / static_cast<double>(placed));
    }
    return true;
}

ecs::Registry* SpaceWorld::registryFor(std::uint32_t system)
{
    for (const std::unique_ptr<SystemBubble>& bubble : m_bubbles) {
        if (bubble->system == system) {
            return &bubble->registry;
        }
    }
    return nullptr;
}

const ecs::Registry* SpaceWorld::registryFor(std::uint32_t system) const
{
    for (const std::unique_ptr<SystemBubble>& bubble : m_bubbles) {
        if (bubble->system == system) {
            return &bubble->registry;
        }
    }
    return nullptr;
}

void SpaceWorld::furnishBubble(SystemBubble& bubble)
{
    // Before anything reads it, and that ordering is the whole reason this
    // function exists rather than a bare push_back: a bubble whose pools are
    // created lazily by the first EMPLACE asserts on the first const READ, and
    // a system with no NPC in it never emplaces a ShipPilot.
    ensureWorldPools(bubble.registry);
    // ⚑⚑ A STREAM PER SYSTEM (Phase 38 stage B). The seed is the one the
    // generator already uses for a system's own draws at `:6090` and `:6206`,
    // so "this system's randomness" means one thing in this file. The stream a
    // bubble draws from must not depend on how many OTHER bubbles are
    // instantiated - that is Phase 37 stage B's finding, and a shared stream is
    // exactly how a sixteenth faction displaced every trader loss after it.
    bubble.chunkRng = core::Rng(m_universeSeed ^ (static_cast<std::uint64_t>(bubble.system) << 32), 909);
    // The statics of its own system, because avoidance and the collision build
    // both push them and both run per bubble now. A bubble whose star came
    // from the player's system is a system whose ships dodge nothing and fly
    // into a sun that is not drawn.
    bubble.planets.clear();
    if (bubble.system >= m_galaxy.systems.size()) {
        // `spawn()` opens the first bubble before a galaxy exists. The
        // placeholder star it used to assign to `m_star` directly lives here
        // now, for the same reason it existed: rendering before
        // generateUniverse has to stay sane.
        bubble.star = {.name = "(void)", .position = {}, .radius = 6.96e8};
        return;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[bubble.system];
    bubble.star = {.name = spec.name, .position = {}, .radius = spec.starRadius};
    for (const sim::PlanetSpec& planet : spec.planets) {
        bubble.planets.push_back({.name = planet.name, .position = planet.position, .radius = planet.radius});
    }
}

void SpaceWorld::fillSystemSky(SystemBubble& bubble)
{
    const sim::SystemSpec& spec = m_galaxy.systems[bubble.system];
    instantiateSystemEntities(bubble.registry, spec);
    // Rocks and wrecks (Phase 8f): depletion decides which rocks are still
    // there to spawn, and both the field list and the depletion are a pure
    // function of the seed and the system - so this is the same sky whether
    // the player is standing in it or not.
    instantiateMiningEntities(bubble);
    // Ambient faction presence (Phase 8b): owner wings + raid incursions.
    spawnAmbientPilots(bubble, spec);
}

bool SpaceWorld::instantiateSystem(std::uint32_t system, bool overCap)
{
    // ⚑⚑ THE CAP IS CHECKED HERE TOO, AND IT IS THE POLICY CAP NOW (Phase 38
    // stage C). It was `kMaxBubbles`, the save-file sanity limit, because
    // stage B had no policy to check against. This door is the one the tests
    // and the console open a system through, so a cap it did not honour would
    // be a cap the O(n^2) pass could still be surprised by.
    //
    // ⚑⚑⚑ `overCap` IS THE ONE EXEMPTION AND ONLY THE CAPTAIN TICK PASSES IT
    // (stage C). The spec named what happens without it: past six systems a
    // captain's ship "is not destroyed and not refused - it is never simulated,
    // silently", because this returns `false` and nothing downstream asks why.
    // It is the same soft cap `enforceBubbleCap` keeps, said at the other door,
    // and it is a parameter rather than a second function so that there is one
    // place where a bubble comes into existence. `kMaxBubbles` still binds
    // absolutely: that one is the save format's limit and not a policy.
    if (system >= m_galaxy.systems.size()) {
        return false;
    }
    if (!overCap && m_bubbles.size() >= kMaxInstantiatedSystems) {
        return false;
    }
    if (m_bubbles.size() >= kMaxBubbles) {
        return false;
    }
    // Already open: hand it back as it stands. Building the sky a second time
    // on top of the first is exactly the failure stage A shipped and caught
    // (`re_entering_the_system_you_are_in_rebuilds_its_sky_rather_than_
    // doubling_it`), and this is the second door into that room.
    if (registryFor(system) != nullptr) {
        return true;
    }
    (void)openBubble(system);
    fillSystemSky(*m_bubbles.back());
    // ⚑⚑⚑ AND IT GOES ON THE CLOCK, BECAUSE EVERY BUBBLE BUT THE PLAYER'S IS
    // HELD BY ONE (Phase 38 stage C). That is the stage's invariant said as
    // code: a bubble the player is not standing in exists only because
    // something is holding it open, the only holder is the retention window,
    // and a non-front bubble on zero seconds is a contradiction the sweep
    // would resolve by dropping it on the next tick. A system opened through
    // this door is on the same policy as one the player backed out of - it is
    // the mechanism, not an exemption from the mechanism.
    m_bubbles.back()->holdSeconds = kCoolingSeconds;
    return true;
}

ecs::Registry& SpaceWorld::openBubble(std::uint32_t system)
{
    m_bubbles.push_back(std::make_unique<SystemBubble>());
    SystemBubble& bubble = *m_bubbles.back();
    bubble.system = system;
    furnishBubble(bubble);
    return bubble.registry;
}

// Split out of initializeMining at Phase 13 so the galaxy generator can have
// it: station placement needs to know which systems have rock, and a field is
// a function of the mining params and the system seed. Depends only on the
// defs and on m_commodityIds — nothing about the galaxy — which is what makes
// calling it before generateGalaxy legal.
void SpaceWorld::buildMiningParams()
{
    m_miningParams = sim::MiningParams{};
    m_miningParams.ores.clear();
    // What a rock can be made of is a data question: any commodity whose def
    // carries an ore weight is something the galaxy has deposits of.
    if (m_defs == nullptr) {
        return;
    }
    for (std::uint32_t i = 0; i < m_commodityIds.size(); ++i) {
        const assets::CommodityDef* def = m_defs->findCommodity(m_commodityIds[i].c_str());
        if (def == nullptr ||
            (def->oreWeightCore <= 0.0f && def->oreWeightFrontier <= 0.0f && def->oreWeightFringe <= 0.0f)) {
            continue;
        }
        sim::OreEntry entry;
        entry.commodity = i;
        entry.weight[0] = def->oreWeightCore;
        entry.weight[1] = def->oreWeightFrontier;
        entry.weight[2] = def->oreWeightFringe;
        m_miningParams.ores.push_back(entry);
    }
}

void SpaceWorld::initializeMining()
{
    ensureWorldPools(playerRegistry());
    // Re-derived rather than assumed: the load path reaches here without
    // having run generateUniverse, and it is the same pure function either way.
    buildMiningParams();
    m_mining.initialize(
        m_galaxy, m_miningParams, static_cast<std::uint32_t>(m_commodityIds.size()), m_universeSeed);
    m_fields.clear();
    m_wreckEvents.clear();
    m_rockEvents.clear();
    m_collectTicker = 0.0f;
    m_collectTickerAge = 0.0;
    m_collectName.clear();
}

void SpaceWorld::instantiateMiningEntities(SystemBubble& bubble)
{
    ecs::Registry& registry = bubble.registry;
    // Phase 19: what a rock and its chunks are drawn as comes from the ORE's
    // def row, so ice and raw ore need not be the same grey lump. Resolved
    // once into a table rather than per rock, for the same reason the station
    // models are: a name lookup is a string compare and a field holds dozens.
    // Every shipped commodity leaves both keys empty and gets the role.
    std::vector<ModelId> rockModels;
    std::vector<ModelId> chunkModels;
    if (m_defs != nullptr) {
        rockModels.reserve(m_commodityIds.size());
        chunkModels.reserve(m_commodityIds.size());
        for (const std::string& id : m_commodityIds) {
            const assets::CommodityDef* def = m_defs->findCommodity(id.c_str());
            const std::string context = "commodity '" + id + "'";
            rockModels.push_back(modelOverrideOr(
                *m_defs, def == nullptr ? std::string() : def->model, context.c_str(), kRoleRock, true));
            chunkModels.push_back(modelOverrideOr(*m_defs,
                                                  def == nullptr ? std::string() : def->chunkModel,
                                                  context.c_str(),
                                                  kRoleOreChunk,
                                                  true));
        }
    }
    m_chunkModels = chunkModels; // the chunk spawn happens later, on a cut

    // ⚑ The fields of THIS system rather than `m_fields`, which is the
    // player's side data (Phase 38 stage B). Both are a pure function of the
    // seed, so asking again costs a generator call and buys the right answer
    // for a bubble the player is not standing in.
    std::vector<sim::AsteroidFieldSpec> fields;
    m_mining.fieldsFor(m_galaxy, bubble.system, fields);
    std::vector<sim::RockSpec> rocks;
    for (std::uint32_t field = 0; field < fields.size(); ++field) {
        m_mining.rocksFor(m_galaxy, bubble.system, field, rocks);
        for (std::uint32_t index = 0; index < rocks.size(); ++index) {
            const sim::RockSpec& rock = rocks[index];
            if (m_mining.unitsLeft(bubble.system, field, index, rock.yieldUnits) <= 0.0f) {
                continue; // cut to nothing on an earlier visit; it broke up
            }
            const ecs::Entity entity = registry.create();
            registry.emplace<Transform>(
                entity, Transform{.position = rock.position, .previousPosition = rock.position});
            const float scale = static_cast<float>(rock.radius);
            registry.emplace<RenderShape>(entity,
                                          RenderShape{.scale = {scale, scale, scale},
                                                      .model = rock.commodity < rockModels.size()
                                                                   ? rockModels[rock.commodity]
                                                                   : roleModel(kRoleRock)});
            registry.emplace<MineableRock>(entity,
                                           MineableRock{.field = field,
                                                        .index = index,
                                                        .commodity = rock.commodity,
                                                        .totalUnits = rock.yieldUnits,
                                                        .tumbleAxis = rock.tumbleAxis,
                                                        .tumbleRate = rock.tumbleRate});
        }
    }
}

void SpaceWorld::spawnCutChunk(SystemBubble& bubble,
                               const core::DVec3& cutter,
                               const core::DVec3& origin,
                               double surface,
                               std::uint32_t commodity,
                               float units)
{
    // Ore breaks off *toward the beam* — with a spread, but not at random.
    // Scattering it evenly means most of what you cut simply leaves, and the
    // loop becomes chasing debris rather than mining.
    const core::DVec3 toShip = cutter - origin;
    const double distance = length(toShip);
    core::DVec3 direction =
        distance > 1.0 ? toShip * (1.0 / distance) : sim::randomPlayfieldDirection(bubble.chunkRng);
    direction = direction + sim::randomPlayfieldDirection(bubble.chunkRng) * kChunkSpread;
    const double spread = length(direction);
    direction = spread > 1.0e-6 ? direction * (1.0 / spread) : core::DVec3{0.0, 0.0, 1.0};
    spawnOreChunk(bubble,
                  origin + direction * surface,
                  direction * (kChunkDriftSpeed * (0.6 + 0.8 * bubble.chunkRng.nextDouble01())),
                  commodity,
                  units);
}

void SpaceWorld::spawnOreChunk(SystemBubble& bubble,
                               const core::DVec3& position,
                               const core::DVec3& velocity,
                               std::uint32_t commodity,
                               float units)
{
    ecs::Registry& registry = bubble.registry;
    const ecs::Entity entity = registry.create();
    registry.emplace<Transform>(entity, Transform{.position = position, .previousPosition = position});
    registry.emplace<RenderShape>(entity,
                                  RenderShape{.scale = {6.0f, 6.0f, 6.0f},
                                              .model = commodity < m_chunkModels.size()
                                                           ? m_chunkModels[commodity]
                                                           : roleModel(kRoleOreChunk)});
    registry.emplace<OreChunk>(
        entity,
        OreChunk{
            .velocity = velocity, .lifetime = kChunkLifetimeSeconds, .commodity = commodity, .units = units});
}

float SpaceWorld::cutRock(SystemBubble& bubble,
                          const core::DVec3& cutter,
                          std::uint32_t entityIndex,
                          float units)
{
    ecs::Registry& registry = bubble.registry;
    MineableRock* rock = registry.storage<MineableRock>().tryGet(entityIndex);
    if (rock == nullptr) {
        return 0.0f;
    }
    // ⚑ `m_currentSystem` until stage B. Depletion is recorded against a
    // system, so cutting a rock in a bubble the player is not standing in would
    // have taken the ore out of the player's OWN field.
    const float taken = m_mining.mineRock(bubble.system, rock->field, rock->index, rock->totalUnits, units);
    if (taken <= 0.0f) {
        return 0.0f;
    }
    // What comes off drifts: the beam breaks the rock, the ship still has to
    // go and get it. Chunks are capped so a fat bite arrives as several.
    const core::DVec3 origin = registry.storage<Transform>().get(entityIndex).position;
    const double surface = static_cast<double>(registry.storage<RenderShape>().get(entityIndex).scale.x);
    float remaining = taken;
    while (remaining > 0.0f) {
        const float chunk = std::min(remaining, kChunkUnitCeiling);
        remaining -= chunk;
        spawnCutChunk(bubble, cutter, origin, surface, rock->commodity, chunk);
    }
    return taken;
}

float SpaceWorld::cutWreck(SystemBubble& bubble,
                           const core::DVec3& cutter,
                           std::uint32_t entityIndex,
                           float units)
{
    const ecs::Registry& registry = bubble.registry;
    const WreckMarker* marker = registry.storage<WreckMarker>().tryGet(entityIndex);
    if (marker == nullptr) {
        return 0.0f;
    }
    std::uint32_t commodity = 0;
    const float taken = m_mining.cutWreckCargo(marker->id, units, &commodity);
    const core::DVec3 origin = registry.storage<Transform>().get(entityIndex).position;
    if (taken > 0.0f) {
        float remaining = taken;
        while (remaining > 0.0f) {
            const float chunk = std::min(remaining, kChunkUnitCeiling);
            remaining -= chunk;
            spawnCutChunk(bubble, cutter, origin, 25.0, commodity, chunk);
        }
        return taken;
    }

    // Nothing left to cut loose: the hull gives up what it was carrying that
    // does not float — credits and, if it fits, a component off its own mounts.
    const sim::WreckRecord* wreck = m_mining.wreck(marker->id);
    if (wreck == nullptr) {
        return 0.0f;
    }
    const double credits = wreck->contents.credits;
    const std::string componentId = wreck->contents.componentId;
    const std::string name = wreck->name;
    m_playerCredits += credits;
    std::string componentTaken;
    (void)tryFitSalvagedComponent(componentId, componentTaken);
    (void)m_mining.removeWreck(marker->id);
    SOL_LOG_INFO("cut open the wreck of %s: %.0f cr%s%s",
                 name.c_str(),
                 credits,
                 componentTaken.empty() ? "" : ", fitted ",
                 componentTaken.c_str());
    return 0.0f;
}

std::uint32_t SpaceWorld::entityAhead(double range, bool& outIsWreck) const
{
    outIsWreck = false;
    if (isDocked() || range <= 0.0) {
        return kNoIndex;
    }
    const sim::ShipState state = shipState();
    const core::Vec3 forward = rotate(state.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
    const core::DVec3 muzzle = state.position;
    const core::DVec3 beamEnd = muzzle + toDVec3(forward) * range;

    const ecs::Pool<Transform>& transforms = playerRegistry().storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = playerRegistry().storage<RenderShape>();
    const ecs::Pool<MineableRock>& rocks = playerRegistry().storage<MineableRock>();
    const ecs::Pool<WreckMarker>& wrecks = playerRegistry().storage<WreckMarker>();
    double bestT = 2.0;
    std::uint32_t best = kNoIndex;
    const auto sweep = [&](std::uint32_t entityIndex, bool isWreck) {
        const RenderShape& shape = shapes.get(entityIndex);
        const double radius = modelBaseRadius(shape.model) * static_cast<double>(shape.scale.x);
        double hitT = 0.0;
        if (sim::segmentHitsSphere(muzzle, beamEnd, transforms.get(entityIndex).position, radius, hitT) &&
            hitT < bestT) {
            bestT = hitT;
            best = entityIndex;
            outIsWreck = isWreck;
        }
    };
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        sweep(rocks.entityIndices()[i], false);
    }
    for (std::size_t i = 0; i < wrecks.size(); ++i) {
        sweep(wrecks.entityIndices()[i], true);
    }
    return best;
}

ProspectInfo SpaceWorld::prospectAhead() const
{
    ProspectInfo info;
    // The scanner is what lets you read a rock at a distance; the beam is what
    // lets you cut it. Reading further than you can cut is the point.
    const double readRange = std::max(targetScanRange(), 5'000.0);
    bool isWreck = false;
    const std::uint32_t entityIndex = entityAhead(readRange, isWreck);
    if (entityIndex == kNoIndex) {
        return info;
    }
    info.valid = true;
    info.wreck = isWreck;
    info.distance =
        length(playerRegistry().storage<Transform>().get(entityIndex).position - shipState().position);
    // ⚑ The mining beams' reach, never the longest gun's (Phase 31 stage C1).
    // A ship carrying a 3 km cannon beside an 800 m laser can cut at 800 m,
    // and the readout used to be able to read the one gun there was.
    const ArmamentSummary armament = playerArmament();
    info.inRange = armament.canMine && info.distance <= static_cast<double>(armament.miningRange);
    if (isWreck) {
        const WreckMarker& marker = playerRegistry().storage<WreckMarker>().get(entityIndex);
        const sim::WreckRecord* wreck = m_mining.wreck(marker.id);
        if (wreck == nullptr) {
            info.valid = false;
            return info;
        }
        info.name = wreck->name;
        for (const sim::SignalCargo& cargo : wreck->contents.cargo) {
            info.unitsLeft += cargo.units;
        }
        info.unitsTotal = info.unitsLeft;
        return info;
    }
    const MineableRock& rock = playerRegistry().storage<MineableRock>().get(entityIndex);
    info.name = rock.commodity < m_commodityIds.size() && m_defs != nullptr
                    ? commodityDisplayName(*m_defs, m_commodityIds[rock.commodity])
                    : "Ore";
    info.unitsTotal = rock.totalUnits;
    info.unitsLeft = m_mining.unitsLeft(m_currentSystem, rock.field, rock.index, rock.totalUnits);
    return info;
}

bool SpaceWorld::mineAhead()
{
    bool isWreck = false;
    const std::uint32_t entityIndex = entityAhead(std::max(targetScanRange(), 5'000.0), isWreck);
    if (entityIndex == kNoIndex) {
        return false;
    }
    // Dev path: one press empties what the beam would take a while to grind.
    if (isWreck) {
        (void)cutWreck(playerBubble(), shipState().position, entityIndex, 1.0e6f);
        return true;
    }
    const MineableRock rock = playerRegistry().storage<MineableRock>().get(entityIndex);
    if (cutRock(playerBubble(), shipState().position, entityIndex, 1.0e6f) <= 0.0f) {
        return false;
    }
    playerRegistry().destroy(playerRegistry().entityFromIndex(entityIndex));
    m_rockEvents.push_back({.commodity = rock.commodity, .units = rock.totalUnits});
    return true;
}

bool SpaceWorld::warpToNearestRock()
{
    if (isDocked()) {
        return false;
    }
    const core::DVec3 position = shipState().position;
    const ecs::Pool<MineableRock>& rocks = playerRegistry().storage<MineableRock>();
    const ecs::Pool<Transform>& transforms = playerRegistry().storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = playerRegistry().storage<RenderShape>();
    std::uint32_t best = kNoIndex;
    double bestDistance = 0.0;
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const std::uint32_t entityIndex = rocks.entityIndices()[i];
        const double distance = length(transforms.get(entityIndex).position - position);
        if (best == kNoIndex || distance < bestDistance) {
            best = entityIndex;
            bestDistance = distance;
        }
    }
    if (best == kNoIndex) {
        return false;
    }
    const double standoff = static_cast<double>(shapes.get(best).scale.x) + 400.0; // clear of the hull
    return warpTo(transforms.get(best).position, standoff);
}

bool SpaceWorld::warpTo(const core::DVec3& target, double standoff)
{
    if (isDocked()) {
        return false;
    }
    const core::DVec3 position = shipState().position;
    // Approach from wherever the ship already is, so the parked view looks
    // like an arrival rather than a fixed camera angle.
    core::DVec3 approach = position - target;
    const double length2 = length(approach);
    approach = length2 > 1.0 ? approach * (1.0 / length2) : core::DVec3{0.0, 0.0, 1.0};
    const core::DVec3 parked = target + approach * standoff;

    clearCommand();
    Transform& transform = playerRegistry().storage<Transform>().get(playerEntityIndex());
    transform.position = parked;
    transform.previousPosition = parked;
    // Point the nose at the rock: the rotation taking -Z to the look vector.
    const core::Vec3 look = toVec3(approach * -1.0);
    const core::Vec3 nose{0.0f, 0.0f, -1.0f};
    const float alignment = dot(look, nose);
    core::Quat orientation = core::Quat::identity();
    if (alignment < 0.9999f) {
        core::Vec3 axis = cross(nose, look);
        if (length(axis) < 1.0e-5f) {
            axis = {0.0f, 1.0f, 0.0f}; // exactly behind: any perpendicular does
        }
        orientation = core::fromAxisAngle(normalize(axis), std::acos(std::clamp(alignment, -1.0f, 1.0f)));
    }
    transform.orientation = orientation;
    transform.previousOrientation = orientation;
    playerRegistry().storage<FlightBody>().get(playerEntityIndex()) = FlightBody{};
    SOL_LOG_WARN("dev warp: parked %.0f m off the target", standoff);
    return true;
}

bool SpaceWorld::applyWreckLoot(std::uint32_t id, sim::SignalLoot loot)
{
    return m_mining.setWreckContents(id, std::move(loot));
}

void SpaceWorld::takeWreckEvents(std::vector<WreckEvent>& out)
{
    out.insert(out.end(), m_wreckEvents.begin(), m_wreckEvents.end());
    m_wreckEvents.clear();
}

void SpaceWorld::takeRockEvents(std::vector<RockEvent>& out)
{
    out.insert(out.end(), m_rockEvents.begin(), m_rockEvents.end());
    m_rockEvents.clear();
}

namespace {

// ⚑⚑⚑⚑ WHAT A DEAD SHIP TURNS OUT TO HAVE BEEN CARRYING (Phase 33 stage C),
// AND IT IS THE FIRST THING IN THE SIM THAT READS `CommodityDef::tier`.
//
// Both loot composers used to roll `rng.range(commodityCount)` - a uniform pick
// over the entire commodity table. At four goods that reads as "a hauler's
// cargo" and nobody ever noticed; the material tree is what turns it into a
// defect, because a uniform pick over gdd.md §6's forty goods is an interceptor
// wreck yielding station module kits one time in forty. ⚑ *The roll did not
// break. The table grew underneath it, and nothing was watching the join.*
//
// ⚑⚑ THE WEIGHTS ARE A STATEMENT ABOUT FREIGHT, NOT ABOUT THE TREE. Raw and
// consumer goods are bulk: they move constantly, in quantity, on every hull in
// the galaxy, and they are what most ships are carrying most of the time.
// Refined goods move less and are worth more; components move less again. That
// ordering is deliberately NOT `tier`'s ordinal - `CommodityTier` ships no band
// table on purpose because `consumer` is a branch rather than a fifth step, and
// this table is exactly the kind of thing that would have gone quietly wrong if
// it had leaned on `<`. It is written out, one tier at a time, so that adding a
// tier is a compile error rather than a silent zero.
//
// ⚑⚑ AN UNSTATED TIER IS NOT A ZERO. `tier` is optional and carries `hasTier`
// for `ShipDef::hullClass`'s reason, so a mod's commodity that says nothing
// about itself must not silently vanish from every wreck in the galaxy; it
// weighs what the rarest stated tier does, which is the old uniform behaviour's
// footing rather than an opinion this table invented on the author's behalf.
[[nodiscard]] float haulWeight(const assets::CommodityDef& commodity, bool canCarryAssemblies)
{
    if (!commodity.hasTier) {
        return 1.0f;
    }
    switch (commodity.tier) {
    case assets::CommodityTier::Raw:
        return 4.0f;
    case assets::CommodityTier::Consumer:
        return 3.0f;
    case assets::CommodityTier::Refined:
        return 2.0f;
    case assets::CommodityTier::Component:
        return 1.0f;
    // ⚑⚑ THE ONE HARD BOUND, AND IT IS THE DEFECT THE SPEC ACTUALLY NAMED. A
    // ship-or-station module kit is freight for a hull built to carry freight;
    // no weighting makes it merely rare on a fighter, it has to be impossible.
    case assets::CommodityTier::Assembly:
        return canCarryAssemblies ? 1.0f : 0.0f;
    case assets::CommodityTier::Count:
        break;
    }
    return 1.0f;
}

// ⚑⚑⚑ THE BOUND IS READ OFF `mass`, AND THE FIELD THAT LOOKS RIGHT IS THE ONE
// THAT IS NOT SAFE TO USE. `ShipDef::cargoCapacity` is what a hull can hold and
// is the obvious input here - but `ships.toml` authors it on exactly ONE of the
// three shipped hulls, so the interceptor whose own comment reads "two drives
// and no hold" carries the struct default of 50 units as far as any code that
// reads the field is concerned, and a rule keyed on it would make that hull a
// hauler. `hull_class` would be better still and appears in no shipped ship at
// all. `mass` is authored on all three (8 t, 10 t, 40 t) and is already this
// function's own vocabulary - `defaultWreckLoot` values the scrap by it.
//
// ⚑ 25 t sits between the freighter and everything else this project ships,
// which is the honest place for it: the galaxy's hauler can have been carrying
// a module kit and its escort cannot. Nothing reaches this today because no T3
// good exists until stage E - it is the guard being in place BEFORE the tier it
// guards, which is the only order in which it was ever going to be written.
constexpr float kAssemblyHaulMassKg = 25'000.0f;

} // namespace

// One commodity, drawn from what a hull is plausibly carrying rather than from
// the whole table. Falls back to the old uniform pick when there is nothing to
// read a tier off - a def database that has gone away, or a commodity table
// wider than the defs behind it - because a loot roll that returns nothing is a
// wreck that is empty for a reason no player can see.
std::uint32_t SpaceWorld::rollHauledCommodity(core::Rng& rng, bool canCarryAssemblies) const
{
    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());
    if (commodityCount == 0) {
        return kNoIndex;
    }
    if (m_defs == nullptr || m_defs->commodities().size() < commodityCount) {
        return rng.range(commodityCount);
    }
    const std::vector<assets::CommodityDef>& commodities = m_defs->commodities();
    float total = 0.0f;
    for (std::uint32_t c = 0; c < commodityCount; ++c) {
        total += haulWeight(commodities[c], canCarryAssemblies);
    }
    if (total <= 0.0f) {
        return kNoIndex; // a table of nothing but module kits, on a hull too small
    }
    float pick = rng.nextFloat01() * total;
    for (std::uint32_t c = 0; c < commodityCount; ++c) {
        pick -= haulWeight(commodities[c], canCarryAssemblies);
        if (pick < 0.0f) {
            return c;
        }
    }
    return commodityCount - 1; // only reachable if the float sum rounded short
}

sim::SignalLoot SpaceWorld::defaultWreckLoot(const assets::ShipDef* def, std::uint64_t seed) const
{
    sim::SignalLoot loot;
    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());
    if (commodityCount == 0) {
        return loot;
    }
    core::Rng rng(seed, 11);
    // ⚑⚑⚑ SCRAP IS THE HULL ITSELF AND SINCE PHASE 33 STAGE C IT IS `sol.salvage`
    // RATHER THAN `sol.ore`. The line was always right about what it was doing -
    // its own comment read "the hull itself, as ore" - and wrong about what to
    // call it, because before this stage there was no T0 good that came off a
    // ship rather than out of a rock. Nothing else in the function moves: the
    // mass scaling and the spread are unchanged, and `commodities.toml` prices
    // salvage within a credit of raw ore so a kill is worth what it was.
    const float hullScrap = def != nullptr ? std::max(4.0f, def->mass * 0.0016f) : 8.0f;
    const std::uint32_t salvage = commodityIndex("sol.salvage");
    loot.cargo.push_back({.commodity = salvage < commodityCount ? salvage : 0,
                          .units = hullScrap * (0.7f + 0.6f * rng.nextFloat01())});
    // Whatever it was hauling, sometimes — weighted by what a hull that size
    // would plausibly have had in the hold rather than picked off the whole
    // table (see `haulWeight` above).
    if (rng.nextFloat01() < 0.5f) {
        const bool heavyEnoughForAKit = def != nullptr && def->mass >= kAssemblyHaulMassKg;
        const std::uint32_t hauled = rollHauledCommodity(rng, heavyEnoughForAKit);
        if (hauled < commodityCount) {
            loot.cargo.push_back(
                {.commodity = hauled, .units = static_cast<float>(3 + rng.range(15))});
        }
    }
    loot.credits = 40.0 + 260.0 * rng.nextDouble01();
    // Its own hardware, at salvage odds: the gun or a component off its mounts.
    const bool wasArmed =
        def != nullptr && std::any_of(def->mounts.begin(), def->mounts.end(), [](const auto& mount) {
            return assets::mountTakesWeapon(mount.kind) && !mount.fit.empty();
        });
    if (wasArmed && rng.nextFloat01() < 0.2f && m_defs != nullptr && !m_defs->components().empty()) {
        const std::vector<assets::ComponentDef>& components = m_defs->components();
        loot.componentId = components[rng.range(static_cast<std::uint32_t>(components.size()))].id;
    }
    return loot;
}

const assets::FactionDef* SpaceWorld::jurisdictionOf(std::uint32_t systemIndex) const
{
    if (m_defs == nullptr) {
        return nullptr;
    }
    const std::uint32_t owner = systemOwnerFaction(systemIndex);
    if (owner >= m_factionTable.size()) {
        return nullptr; // nobody holds this place
    }
    // ⚑ A clan carries its TEMPLATE's def id (`GameFaction::defId`), so ten
    // Reaver clans all consult the one Reaver Kindred table. That is the right
    // answer and not an approximation: a clan is that faction's character
    // jittered, and its law is that faction's law.
    return m_defs->findFaction(m_factionTable[owner].defId.c_str());
}

const char* SpaceWorld::jurisdictionName(std::uint32_t systemIndex) const
{
    const std::uint32_t owner = systemOwnerFaction(systemIndex);
    if (owner >= m_factionTable.size()) {
        return ""; // nobody holds this place; the caller says so in its own words
    }
    // ⚑ The LIVE faction table rather than the def, which is the whole point:
    // `GameFaction::name` is the generated clan's name where `defId` is only
    // its template's. For a major the two are the same string, so nothing about
    // Navy or Hegemony space moves.
    return m_factionTable[owner].name.c_str();
}

assets::Legality SpaceWorld::commodityLegality(std::uint32_t systemIndex, std::uint32_t commodity) const
{
    const assets::FactionDef* holder = jurisdictionOf(systemIndex);
    if (holder == nullptr) {
        return assets::Legality::Unpoliced;
    }
    if (commodity >= m_commodityIds.size()) {
        return assets::Legality::Legal; // a good this galaxy does not have
    }
    return assets::factionLegalityOf(*holder, m_commodityIds[commodity]);
}

bool SpaceWorld::dockedRefinePair(std::uint32_t& input, std::uint32_t& output) const
{
    input = kNoIndex;
    output = kNoIndex;
    if (!isDocked() || m_defs == nullptr || m_currentSystem >= m_galaxy.systems.size()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (m_dockedStation >= spec.stations.size()) {
        return false;
    }
    // ⚑⚑⚑ THE SERVICE IS THE MODULE'S NOW, AND THAT IS THE WHOLE OF WHAT STAGE C
    // MOVED HERE. `sol.mod_refinery_service` sits in the Refinery's recipe at
    // chance 0.85, so roughly one refinery in seven has the production line and
    // not the counter - which is the first time two stations of one archetype
    // differ in a SERVICE rather than in a rate. The archetype below is still
    // the answer for a station with no composition, the same fallback every
    // other reader of a composed station takes.
    const std::span<const std::uint32_t> modules = stationModules(m_currentSystem, m_dockedStation);
    for (const std::uint32_t module : modules) {
        const ModuleRuntime& runtime = m_modules[module];
        if (runtime.refineInput == kNoIndex || runtime.refineOutput == kNoIndex) {
            continue;
        }
        input = runtime.refineInput;
        output = runtime.refineOutput;
        return true;
    }
    if (!modules.empty()) {
        return false; // composed, and none of its modules offers the service
    }
    const std::vector<assets::StationDef>& stations = m_defs->stations();
    const std::uint32_t archetype = spec.stations[m_dockedStation].archetype;
    if (archetype >= stations.size()) {
        return false;
    }
    const assets::StationDef& def = stations[archetype];
    if (def.refineInput.empty() || def.refineOutput.empty()) {
        return false; // this archetype offers no refining service
    }
    const std::uint32_t in = commodityIndex(def.refineInput.c_str());
    const std::uint32_t out = commodityIndex(def.refineOutput.c_str());
    if (in == kNoIndex || out == kNoIndex) {
        return false; // the commodity defs went away under the station def
    }
    input = in;
    output = out;
    return true;
}

bool SpaceWorld::dockedStationRefines() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    return dockedRefinePair(input, output);
}

std::uint32_t SpaceWorld::refineInputCommodity() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    (void)dockedRefinePair(input, output);
    return input;
}

std::uint32_t SpaceWorld::refineOutputCommodity() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    (void)dockedRefinePair(input, output);
    return output;
}

float SpaceWorld::refinedReadyHere() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    if (!dockedRefinePair(input, output)) {
        return 0.0f;
    }
    return m_mining.readyAt(dockedMarket(), output);
}

double SpaceWorld::refineWaitHere() const
{
    if (!dockedStationRefines()) {
        return -1.0;
    }
    return m_mining.soonestAt(dockedMarket());
}

float SpaceWorld::MiningFeedstock::draw(std::uint32_t market, std::uint32_t commodity, float units)
{
    if (mining == nullptr || galaxy == nullptr || economy == nullptr || market >= economy->markets().size()) {
        return 0.0f;
    }
    // ⚑ An outpost with no miner digs nothing (Phase 8x stage 6). The draw was
    // always abstract — ore simply appeared out of the ground on the coarse
    // clock — and stage 6 gives it a ship, so killing that ship has to reach
    // the books or the ship is scenery. Held for as long as a replacement
    // would take to fly out, then it resumes on its own; nothing here is
    // saved, because the only way to set it is to shoot something in front of
    // the player.
    if (minerHold != nullptr && market < minerHold->size() && (*minerHold)[market] > 0.0) {
        return 0.0f;
    }
    // An outpost works the rock in its own system and nowhere else, which is
    // what makes "where does ore come from" a question the map can answer.
    const std::uint32_t system = economy->markets()[market].systemIndex;
    return mining->drawFromSystem(*galaxy, system, commodity, units);
}

double SpaceWorld::intelPrice() const
{
    // Priced off how much is actually out there to learn, so a package in a
    // dense core cluster costs more than one on the frontier and a system
    // with nothing in reach is nearly free.
    return kIntelBasePrice + kIntelPricePerMarket * static_cast<double>(intelMarketCount());
}

std::uint32_t SpaceWorld::intelMarketCount() const
{
    if (!isDocked()) {
        return 0;
    }
    std::vector<std::uint8_t> hops;
    hopsFrom(m_galaxy, m_currentSystem, kIntelJumpRadius, hops);
    std::uint32_t count = 0;
    for (const sim::StationMarket& market : m_economy.markets()) {
        const std::uint32_t system = market.systemIndex;
        // You are standing in the local one; that reading is always free.
        if (system != m_currentSystem && hops[system] != kUnreachableHops) {
            ++count;
        }
    }
    return count;
}

bool SpaceWorld::buyMarketIntel(std::string* outError)
{
    if (!isDocked()) {
        return refuse("not docked", outError);
    }
    const std::uint32_t count = intelMarketCount();
    if (count == 0) {
        return refuse("no markets in reach to report on", outError);
    }
    const double price = intelPrice();
    if (price > m_playerCredits) {
        return refuse("cannot afford the market report", outError);
    }
    m_playerCredits -= price;

    std::vector<std::uint8_t> hops;
    hopsFrom(m_galaxy, m_currentSystem, kIntelJumpRadius, hops);
    std::vector<float> prices(m_commodityIds.size(), 0.0f);
    for (std::uint32_t m = 0; m < m_economy.markets().size(); ++m) {
        const std::uint32_t system = m_economy.markets()[m].systemIndex;
        if (system == m_currentSystem || hops[system] == kUnreachableHops) {
            continue;
        }
        for (std::uint32_t c = 0; c < prices.size(); ++c) {
            prices[c] = m_economy.price(m, c);
        }
        m_survey.recordMarket(m, prices, m_worldSeconds);
    }
    SOL_LOG_INFO(
        "bought market data on %u markets within %u jumps for %.0f cr", count, kIntelJumpRadius, price);
    return true;
}

void SpaceWorld::recordDockedMarket()
{
    const std::uint32_t market = dockedMarket();
    if (market == kNoIndex) {
        return;
    }
    std::vector<float> prices(m_commodityIds.size(), 0.0f);
    for (std::uint32_t c = 0; c < prices.size(); ++c) {
        prices[c] = m_economy.price(market, c);
    }
    m_survey.recordMarket(market, prices, m_worldSeconds);
}

bool SpaceWorld::bestKnownPrice(
    std::uint32_t commodity, std::uint32_t* outSystem, float* outPrice, double* outAge, bool* outStale) const
{
    std::uint32_t market = 0;
    double age = 0.0;
    if (!m_survey.bestRemembered(commodity, dockedMarket(), m_worldSeconds, &market, outPrice, &age)) {
        return false;
    }
    if (outSystem != nullptr) {
        *outSystem = m_economy.markets()[market].systemIndex;
    }
    if (outAge != nullptr) {
        *outAge = age;
    }
    if (outStale != nullptr) {
        *outStale = age > m_survey.params().intelStaleSeconds;
    }
    return true;
}

float SpaceWorld::marketSatisfaction(std::uint32_t market) const
{
    return m_economy.satisfaction(market);
}

const char* SpaceWorld::marketLimiting(std::uint32_t market) const
{
    const std::uint32_t commodity = m_economy.limitingCommodity(market);
    return commodity < m_commodityIds.size() ? m_commodityIds[commodity].c_str() : "";
}

bool SpaceWorld::orderRefine(float units, std::string* outError)
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    if (!dockedRefinePair(input, output)) {
        return refuse("no refinery here", outError);
    }
    const float available = playerCargo(input);
    const float order = std::min(units, available);
    if (!(order > 0.0f)) {
        // ⚑ Named rather than "no ore", since Phase 33 stage C: the Reclamation
        // Plant refines SALVAGE, and a station telling a pilot who is holding
        // salvage that they have no ore is a station describing a different
        // game. `refine_input` has always been per archetype; this message was
        // the last place that assumed there was only ever one of them.
        const std::string& inputId = m_commodityIds[input];
        return refuse("no " +
                          (m_defs != nullptr ? commodityDisplayName(*m_defs, inputId) : inputId) +
                          " aboard to refine",
                      outError);
    }
    const double fee = m_mining.refineFee(order);
    if (fee > m_playerCredits) {
        return refuse("cannot afford the refining fee", outError);
    }
    if (!m_mining.startRefineJob(dockedMarket(), input, order, output)) {
        return refuse("the refinery queue is full", outError);
    }
    m_playerCargo[input] -= order;
    m_playerCredits -= fee;
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner < m_factionTable.size()) {
        m_factionSim.addStanding(owner, static_cast<float>(fee * kRefineStandingRate));
    }
    SOL_LOG_INFO("refining %.0f units for %.0f cr; ready in %.0f s",
                 static_cast<double>(order),
                 fee,
                 m_mining.refineDuration(order));
    return true;
}

float SpaceWorld::collectRefined()
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    if (!dockedRefinePair(input, output)) {
        return 0.0f;
    }
    const float space = m_playerCargoCapacity - playerCargoTotal();
    if (!(space > 0.0f)) {
        SOL_LOG_WARN("no hold space for the refined metal; it waits here");
        return 0.0f;
    }
    const float taken = m_mining.collectAt(dockedMarket(), output, space);
    if (taken <= 0.0f) {
        return 0.0f;
    }
    m_playerCargo[output] += taken;
    SOL_LOG_INFO("collected %.0f units of refined output", static_cast<double>(taken));
    return taken;
}

void SpaceWorld::tickSystemMining(SystemBubble& bubble, double dt)
{
    // ⚑⚑ THE COARSE HALF LEFT THIS FUNCTION AT STAGE B. `m_mining.tick(dt)`
    // opened it until then - wreck decay and refinery orders, which happen
    // whether the player is watching them or three systems away
    // (decisions/005). That is galaxy-wide work and it now runs ONCE, above the
    // per-system loop in `tick`; leaving it here would have aged every wreck in
    // the game once per instantiated system.
    ecs::Registry& registry = bubble.registry;
    const bool playersBubble = &bubble == m_bubbles.front().get();

    // Rocks tumble. It is the cheapest thing that makes a field read as a
    // place rather than a diagram.
    ecs::Pool<MineableRock>& rocks = registry.storage<MineableRock>();
    ecs::Pool<Transform>& transforms = registry.storage<Transform>();
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const MineableRock& rock = rocks.values()[i];
        Transform& transform = transforms.get(rocks.entityIndices()[i]);
        transform.previousOrientation = transform.orientation;
        transform.orientation =
            normalize(transform.orientation *
                      core::fromAxisAngle(rock.tumbleAxis, rock.tumbleRate * static_cast<float>(dt)));
    }

    // Chunks drift, are gathered, or are lost.
    //
    // ⚑⚑ THE DRIFT IS THE SYSTEM'S AND THE GATHERING IS THE PLAYER'S, AND THAT
    // SEAM IS NEW (Phase 38 stage B). `shipPosition` is a point in the PLAYER's
    // frame; two systems both place their contents around a barycentre origin,
    // so a chunk drifting in a bubble the player is not in would have measured
    // itself against a collector a light-year away and read as being in range.
    // The one guard is `playersBubble`, and it is why the range test below is
    // reachable at all.
    const core::DVec3 shipPosition = shipState().position;
    const double collectRange = static_cast<double>(m_collectorRange);
    float space = m_playerCargoCapacity - playerCargoTotal();
    ecs::Pool<OreChunk>& chunks = registry.storage<OreChunk>();
    std::vector<std::uint32_t> spent;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const std::uint32_t entityIndex = chunks.entityIndices()[i];
        OreChunk& chunk = chunks.values()[i];
        Transform& transform = transforms.get(entityIndex);
        transform.previousPosition = transform.position;
        transform.position = transform.position + chunk.velocity * dt;
        chunk.lifetime -= dt;
        if (chunk.lifetime <= 0.0) {
            spent.push_back(entityIndex);
            continue;
        }
        if (!playersBubble || isDocked() || length(transform.position - shipPosition) > collectRange) {
            continue;
        }
        // A full hold cannot gather: the chunks keep drifting until they are
        // lost, which is the pressure that sends a miner home.
        const float take = std::min(chunk.units, space > 0.0f ? space : 0.0f);
        if (take <= 0.0f) {
            continue;
        }
        if (chunk.commodity < m_playerCargo.size()) {
            m_playerCargo[chunk.commodity] += take;
            space -= take;
            m_collectTicker += take;
            m_collectTickerAge = 0.0;
            m_collectName = m_defs != nullptr && chunk.commodity < m_commodityIds.size()
                                ? commodityDisplayName(*m_defs, m_commodityIds[chunk.commodity])
                                : "ore";
        }
        chunk.units -= take;
        if (chunk.units <= 0.001f) {
            spent.push_back(entityIndex);
        }
    }
    for (const std::uint32_t entityIndex : spent) {
        registry.destroy(registry.entityFromIndex(entityIndex));
    }

    // Reconcile wreck entities with the sim: newly killed ships get a hull to
    // cut, decayed and emptied ones stop being there. Done here rather than
    // in handleShipDestroyed so no pool changes shape mid-iteration.
    bool wrecksChanged = false;
    ecs::Pool<WreckMarker>& markers = registry.storage<WreckMarker>();
    std::vector<std::uint32_t> goneEntities;
    std::vector<std::uint32_t> present;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const std::uint32_t entityIndex = markers.entityIndices()[i];
        const std::uint32_t id = markers.values()[i].id;
        if (m_mining.wreck(id) == nullptr) {
            goneEntities.push_back(entityIndex);
        } else {
            present.push_back(id);
        }
    }
    for (const std::uint32_t entityIndex : goneEntities) {
        registry.destroy(registry.entityFromIndex(entityIndex));
        wrecksChanged = true;
    }
    std::vector<std::uint32_t> here;
    // ⚑ `m_currentSystem` until stage B, which is the player's system rather
    // than this bubble's. A `WreckRecord` has carried its own system since
    // Phase 8f, so the wrong one here would have materialised the player's
    // wrecks in every other bubble - at their own coordinates, in a frame they
    // do not belong to.
    m_mining.wrecksIn(bubble.system, here);
    for (const std::uint32_t id : here) {
        if (std::find(present.begin(), present.end(), id) != present.end()) {
            continue;
        }
        const sim::WreckRecord* wreck = m_mining.wreck(id);
        const ecs::Entity entity = registry.create();
        registry.emplace<Transform>(
            entity, Transform{.position = wreck->position, .previousPosition = wreck->position});
        // ⚑ Phase 19 stage E: a wreck is drawn as THE SHIP THAT DIED, at that
        // hull's own scale, because `WreckRecord::defId` has carried "the
        // victim's ship def" since the record existed and this site threw it
        // away to draw one model for every death. There is still no broken
        // hull mesh - the oversize factor is the whole of the effect - but a
        // freighter now leaves a freighter-sized derelict rather than a
        // shuttle-sized one.
        //
        // ⚑ This is NOT cosmetic: the salvage beam sweeps `modelBaseRadius() *
        // scale`, so a bigger wreck is a bigger thing to hit. It is the one
        // behaviour change in the phase and was called out as such before it
        // was built.
        //
        // A def that no longer exists (a save naming a removed hull) falls
        // back to the `wreck` role at the old size, which is why that role
        // exists at all.
        ModelId wreckModel = roleModel(kRoleWreck);
        float wreckScale = kWreckOversize;
        if (m_defs != nullptr) {
            if (const assets::ShipDef* victim = m_defs->findShip(wreck->defId.c_str())) {
                wreckModel = modelIdFromName(*m_defs, victim->model, "wreck's ship def", kRoleWreck);
                wreckScale = victim->scale * kWreckOversize;
            }
        }
        registry.emplace<RenderShape>(
            entity, RenderShape{.scale = {wreckScale, wreckScale, wreckScale}, .model = wreckModel});
        registry.emplace<WreckMarker>(entity, WreckMarker{.id = id});
        wrecksChanged = true;
    }
    // The nav list is the player's, so only their sky can change it.
    if (wrecksChanged && playersBubble) {
        rebuildDynamicTargets();
    }
}

// Places `count` hulls from a roster around an anchor. Was a lambda inside
// `spawnAmbientPilots` until Phase 30 stage C needed to send a wing at a moment
// that is not "the system just loaded".
//
// ⚑ It goes through `spawnShipAt`, NOT `spawnShipFromDef`. That matters: the
// latter places a ship 150-250 m directly in front of the PLAYER, facing the
// player's own orientation, which is correct for the dev console it was written
// for and is exactly the trap decisions/019 warned this stage about - a
// response wing built on it materialises in the offender's face, which is the
// precise opposite of what a response time is for.
std::span<const std::string>
factionRoster(const GameFaction& faction, assets::RosterCell cell, assets::RosterCell fallback)
{
    const std::vector<std::string>* rosters[assets::kRosterCellCount] = {
        &faction.shipsPatrol, &faction.shipsRaider, &faction.shipsTrader};
    const auto index = static_cast<std::size_t>(cell);
    if (index >= assets::kRosterCellCount) {
        return {};
    }
    // ⚑ THE DECLARATION IS CHECKED BEFORE THE LIST, which is what makes it a
    // declaration rather than a comment. A faction that says it fields none
    // gets none, and no substitute is looked for - the empty answer IS the
    // authored one.
    if (faction.buildsNo[index]) {
        return {};
    }
    if (!rosters[index]->empty()) {
        return *rosters[index];
    }
    // Unspecified, so this site's own substitution applies. `Count` is how a
    // site says it has none, which is what both ambient spawns have always
    // said by not writing a ternary.
    const auto fallbackIndex = static_cast<std::size_t>(fallback);
    if (fallbackIndex >= assets::kRosterCellCount || faction.buildsNo[fallbackIndex]) {
        return {};
    }
    return *rosters[fallbackIndex];
}

const char* factionKindLabel(const GameFaction& faction)
{
    switch (faction.kind) {
    case assets::FactionKind::Pirate:
        return "pirate clan";
    case assets::FactionKind::Shadow:
        return "syndicate";
    case assets::FactionKind::Major:
        break;
    }
    return "major";
}

void SpaceWorld::spawnWing(SystemBubble& bubble,
                           std::uint32_t faction,
                           assets::RosterCell cell,
                           std::span<const std::string> roster,
                           float baselineSecurity,
                           std::uint32_t count,
                           const core::DVec3& anchor,
                           double spread,
                           PilotState state,
                           const core::DVec3* waypoint)
{
    if (roster.empty() || m_defs == nullptr || faction >= m_factionTable.size()) {
        return;
    }
    // ⚑ The classes the pick ranks against, read once for the whole wing
    // rather than per slot - the roster does not change between them, and this
    // is the same scratch-member arrangement `m_rosterCapacities` uses for the
    // hauler pick a few hundred lines down.
    m_rosterClasses.clear();
    for (const std::string& id : roster) {
        const assets::ShipDef* candidate = m_defs->findShip(id.c_str());
        m_rosterClasses.push_back(candidate != nullptr && candidate->hasHullClass
                                      ? static_cast<std::uint32_t>(candidate->hullClass)
                                      : static_cast<std::uint32_t>(assets::kHullClassCount));
    }
    // The job is the cell's, not a caller's opinion of it - and it is the cell
    // ASKED for, so a wing flying a substituted roster is still doing what it
    // was sent to do.
    const PilotRole cellRole = pilotRoleFor(cell);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t pick =
            chooseWingHull(std::span<const std::uint32_t>(m_rosterClasses), baselineSecurity, i, count);
        const assets::ShipDef* def = m_defs->findShip(roster[pick].c_str());
        if (def == nullptr) {
            SOL_LOG_WARN("wing: no ship def '%s'", roster[pick].c_str());
            return;
        }
        // ⚑⚑⚑⚑ `HullRole` GETS ITS FIRST READER HERE, THREE PHASES AFTER IT
        // WAS AUTHORED. Phase 32 ruling 11 made it deliberately unread
        // vocabulary and Phase 36 said so again in writing when it shipped
        // covert COMPONENTS instead; this is the line that spends it.
        //
        // ⚑⚑⚑ AND IT IS AN EXCEPTION TO THE RULE THREE LINES UP, WHICH IS
        // WORTH SAYING RATHER THAN SLIPPING IN. "The job is the cell's" is
        // right for a hull that could be doing any job - an interceptor sent as
        // a raider IS a raider. A covert hull is not that: it is a statement
        // about the job made when the hull was bought, and a clan that put one
        // in its raider roster did not send it out to trade blows. So the HULL
        // wins, and only for this one role.
        //
        // ⚑⚑ THE ALTERNATIVE WAS A FOURTH ROSTER CELL, and it would have been
        // worse: `builds_no` and the four substitution rules would all have had
        // to learn a cell that means "the same job, quietly", and every faction
        // in the game would have grown a column it had nothing to say about.
        const PilotRole role = def->role == assets::HullRole::Covert ? PilotRole::Covert : cellRole;
        const core::DVec3 position =
            anchor + core::DVec3{spread * (1.0 + i), 300.0 + 250.0 * i, -spread * 0.5 * i};
        const ecs::Entity entity =
            spawnShipAt(bubble, *def, *m_defs, position, m_factionTable[faction].name.c_str());
        ShipPilot pilot{.role = role, .factionIndex = faction};
        pilot.state = state;
        if (waypoint != nullptr) {
            pilot.waypoint = *waypoint;
            pilot.respondTimer = kResponseGiveUpSeconds;
        }
        // ⚑⚑⚑ INTO THE SAME REGISTRY THE HULL WENT INTO, WHICH IS NOT WHAT
        // THIS SAID (Phase 38 stage B). `spawnShipAt` was given the bubble and
        // this line was not, so a wing spawned into any bubble but the
        // player's got its hull in one registry and its ShipPilot in another -
        // a ship nothing would ever fly, and an entity index in the player's
        // pilot pool belonging to somebody else's ship. Nothing could have
        // caught it: the compiler is happy either way and both registries have
        // the pool.
        bubble.registry.emplace<ShipPilot>(entity, pilot);
    }
}

core::DVec3 SpaceWorld::nearestPost(const core::DVec3& from) const
{
    if (m_currentSystem >= m_galaxy.systems.size()) {
        return from;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const core::DVec3* best = nullptr;
    double bestDistance = 0.0;
    const auto consider = [&](const core::DVec3& candidate) {
        const core::DVec3 d = candidate - from;
        const double distance = d.x * d.x + d.y * d.y + d.z * d.z; // squared is enough to rank
        if (best == nullptr || distance < bestDistance) {
            best = &candidate;
            bestDistance = distance;
        }
    };
    for (const sim::StationSpec& station : spec.stations) {
        consider(station.position);
    }
    for (const sim::GateSpec& gate : spec.gates) {
        consider(gate.position);
    }
    if (best == nullptr) {
        // No station and no gate. No shipped system is like this - every one
        // has at least two gates - so this is the honest fallback rather than a
        // case: hold station on the primary planet.
        return spec.planets.empty() ? from : spec.planets[spec.primaryPlanet].position;
    }
    return *best;
}

void SpaceWorld::spawnAmbientPilots(SystemBubble& bubble, const sim::SystemSpec& spec)
{
    const std::uint32_t systemIndex = bubble.system;
    if (m_defs == nullptr || m_factionTable.empty()) {
        return;
    }
    const core::DVec3 hub = spec.planets[spec.primaryPlanet].position;

    // Owner presence: patrol wings by region security for majors, resident
    // raider wings for clan systems. Read through the accessor, not off the
    // spec: since Phase 8u the founding claim is not who holds the system,
    // and this is the one site where the stale value would put the wrong
    // navy in the sky above a station that has changed hands.
    const std::uint32_t owner = systemOwnerFaction(systemIndex);
    const core::DVec3 anchor = spec.stations.empty() ? hub : spec.stations[0].position;
    // ⚑⚑ Phase 30 stage B: the two per-region tables that used to sit here are
    // curves on the security BASELINE now - see `patrolsFor` and friends in the
    // header, including why they must not read the live rating. The branch is
    // still on `faction.pirate()` rather than on the sign, because the roster is
    // what actually differs; the sign only decides how many.
    const float baselineSecurity = systemSecurityBaseline(systemIndex);
    if (owner < m_factionTable.size()) {
        const GameFaction& faction = m_factionTable[owner];
        if (faction.pirate()) {
            // ⚑ `RosterCell::Count` as the fallback is this site saying it has
            // never had one, which is what reading `faction.shipsRaider` bare
            // used to say by omission.
            spawnWing(bubble,
                      owner,
                      assets::RosterCell::Raider,
                      factionRoster(faction, assets::RosterCell::Raider, assets::RosterCell::Count),
                      baselineSecurity,
                      raidersFor(baselineSecurity),
                      anchor,
                      900.0);
        } else {
            // ⚑⚑⚑⚑ BOTH POSTURES, AND THEY COST NOTHING (Phase 36 stage B,
            // and the user's ruling). The garrison is FINITE and where it
            // stands is the decision: some of the wing holds the station and
            // the rest is posted to gates. Measured on the shipped galaxy, the
            // three candidate rules were:
            //
            //   one picket at every gate      +214 hulls, worst sky 9
            //   round(baseline * gates)       +102 hulls, worst sky 9
            //   SPLIT the existing wing         +0 hulls, worst sky 4
            //
            // The third watches 40 gates across 39 of the 52 policed systems
            // and does not add a single hull to any sky. A commander with three
            // interceptors choosing to stand one on the lane is a better answer
            // than a commander who is issued a fourth for free - and it is the
            // only one of the three that does not double the ambient hull count
            // the phase's own risk section warns about.
            //
            // ⚑ The station always keeps at least one hull (`wing - 1`), so
            // this can never empty the approach posture to fill the gate one.
            const std::uint32_t wing = patrolsFor(baselineSecurity);
            const std::uint32_t gateCount = static_cast<std::uint32_t>(spec.gates.size());
            const std::uint32_t posted = wing == 0 ? 0u : std::min({wing / 2u, gateCount, wing - 1u});
            spawnWing(bubble,
                      owner,
                      assets::RosterCell::Patrol,
                      factionRoster(faction, assets::RosterCell::Patrol, assets::RosterCell::Count),
                      baselineSecurity,
                      wing - posted,
                      anchor,
                      700.0);
            // ⚑ Posted to the gates that lead somewhere LESS policed, nearest
            // the bottom first. A checkpoint faces the direction trouble comes
            // from, and the alternative - the first N gates in generation order
            // - would be an arbitrary choice dressed up as a rule. This also
            // means a core system watches its frontier lane rather than the
            // lane back to another core system, which is where a smuggler
            // actually arrives from.
            if (posted > 0) {
                m_gatePostOrder.clear();
                for (std::uint32_t g = 0; g < gateCount; ++g) {
                    m_gatePostOrder.push_back(g);
                }
                std::sort(
                    m_gatePostOrder.begin(), m_gatePostOrder.end(), [&](std::uint32_t a, std::uint32_t b) {
                        const float sa = systemSecurityBaseline(spec.gates[a].toSystem);
                        const float sb = systemSecurityBaseline(spec.gates[b].toSystem);
                        if (sa != sb) {
                            return sa < sb;
                        }
                        return spec.gates[a].toSystem < spec.gates[b].toSystem; // stable
                    });
                for (std::uint32_t i = 0; i < posted; ++i) {
                    spawnWing(bubble,
                              owner,
                              assets::RosterCell::Patrol,
                              factionRoster(faction, assets::RosterCell::Patrol, assets::RosterCell::Count),
                              baselineSecurity,
                              1,
                              spec.gates[m_gatePostOrder[i]].position,
                              700.0);
                }
            }

            // Civilian traffic (Phase 13, note 5b). Before this, everything in
            // the sky over a station was military: three interceptors in a core
            // system and nothing else, with the nearest trader body typically
            // ~900,000 km out on a coarse lane. A system did not feel dead
            // because too little was generated - it felt dead because nothing
            // generated near the player was going about its business.
            //
            // ⚑ These are SCENERY, not TraderPuppets. A puppet is a body for a
            // coarse EconomyTrader and carries its cargo, route and attrition
            // hooks; these carry none of that and move no goods, so they cannot
            // touch a steady state 8g tuned from both directions. The day the
            // economy wants intra-system hauls is a different phase.
            //
            // No Lua change: pilot_think's role == "trader" branch already
            // flies a two-leg station circuit, and PilotRole::Trader,
            // PilotState::Travel and shipsTrader all predate this.
            spawnWing(bubble,
                      owner,
                      assets::RosterCell::Trader,
                      factionRoster(faction, assets::RosterCell::Trader, assets::RosterCell::Count),
                      baselineSecurity,
                      civiliansFor(baselineSecurity),
                      anchor,
                      1'500.0);
        }
    }

    // Contested system (Phase 8u): the attacker keeps a standing force here
    // for as long as the claim is live, sized off pressure rather than off a
    // single raid's warmth. This is what makes a border a place the player
    // can fly into and fight over rather than a colour on the map. The
    // owner's patrol wing above is reinforced, not replaced - both sides are
    // present, which is what a contested system means.
    const sim::SystemContest contest = m_factionSim.contestOf(systemIndex);
    const bool contested = m_factionSim.contested(systemIndex);
    if (contested && contest.attacker < m_factionTable.size() && contest.attacker != owner) {
        const GameFaction& attacker = m_factionTable[contest.attacker];
        const std::span<const std::string> roster =
            factionRoster(attacker, assets::RosterCell::Raider, assets::RosterCell::Patrol);
        const std::uint32_t count = std::clamp(static_cast<std::uint32_t>(contest.pressure * 4.0f), 1u, 4u);
        spawnWing(bubble,
                  contest.attacker,
                  assets::RosterCell::Raider,
                  roster,
                  baselineSecurity,
                  count,
                  anchor + core::DVec3{9'000.0, 1'500.0, 6'000.0},
                  1'200.0);
        if (owner < m_factionTable.size()) {
            spawnWing(
                bubble,
                owner,
                assets::RosterCell::Patrol,
                factionRoster(m_factionTable[owner], assets::RosterCell::Patrol, assets::RosterCell::Count),
                baselineSecurity,
                2,
                anchor + core::DVec3{-4'000.0, 800.0, 2'000.0},
                700.0);
        }
    } else {
        // Raid incursion: the last raider keeps ships in-system while the
        // intensity is warm (fresh raids read as an active raiding party).
        const float intensity = m_factionSim.raidIntensity(systemIndex);
        const std::uint32_t raider = m_factionSim.lastRaider(systemIndex);
        if (intensity >= 0.5f && raider < m_factionTable.size() && raider != owner) {
            const std::uint32_t count = std::min(3u, static_cast<std::uint32_t>(intensity + 0.5f));
            spawnWing(
                bubble,
                raider,
                assets::RosterCell::Raider,
                factionRoster(m_factionTable[raider], assets::RosterCell::Raider, assets::RosterCell::Count),
                baselineSecurity,
                count,
                anchor + core::DVec3{9'000.0, 1'500.0, 6'000.0},
                1'200.0);
        }
    }
}

namespace {

// Shortest-arc rotation taking the ship's nose (-z) onto `direction`.
core::Quat lookAlong(const core::DVec3& direction)
{
    const double distance = length(direction);
    if (distance < 1.0e-6) {
        return core::Quat::identity();
    }
    const core::DVec3 unit = direction * (1.0 / distance);
    const core::Vec3 to{static_cast<float>(unit.x), static_cast<float>(unit.y), static_cast<float>(unit.z)};
    const core::Vec3 nose{0.0f, 0.0f, -1.0f};
    const float alignment = core::clamp(core::dot(nose, to), -1.0f, 1.0f);
    if (alignment > 0.9999f) {
        return core::Quat::identity();
    }
    if (alignment < -0.9999f) {
        return core::fromAxisAngle({0.0f, 1.0f, 0.0f}, 3.14159265f); // exactly behind
    }
    return core::fromAxisAngle(core::normalize(core::cross(nose, to)), std::acos(alignment));
}

} // namespace

// Removes a spawned ship without the death path's consequences: no wreck, no
// loot, no kill credit. A puppet leaving the player's system has not died,
// it has stopped being drawn.
void SpaceWorld::despawnShip(SystemBubble& bubble, std::uint32_t entityIndex)
{
    for (std::size_t i = 0; i < bubble.spawnedShips.size(); ++i) {
        if (bubble.spawnedShips[i].entity.index == entityIndex) {
            bubble.spawnedShips.erase(bubble.spawnedShips.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    bubble.registry.destroy(bubble.registry.entityFromIndex(entityIndex));
}

bool SpaceWorld::traderLegSegment(std::uint32_t traderIndex,
                                  std::uint32_t system,
                                  TraderLegPlacement& out) const
{
    return legSegment(m_economy.route(traderIndex), traderIndex, system, out);
}

bool SpaceWorld::captainLegSegment(std::size_t captainIndex,
                                   std::uint32_t system,
                                   TraderLegPlacement& out) const
{
    return legSegment(captainRoute(captainIndex), captainLaneSlot(captainIndex), system, out);
}

bool SpaceWorld::legSegment(const sim::TraderRoute& route,
                            std::uint32_t slot,
                            std::uint32_t system,
                            TraderLegPlacement& out) const
{
    core::DVec3& from = out.from;
    core::DVec3& to = out.to;
    float& progress = out.progress;
    if (route.system != system || route.leg == sim::TraderLeg::None || route.leg == sim::TraderLeg::Jump) {
        return false;
    }
    if (route.fromMarket >= m_economy.markets().size() || route.toMarket >= m_economy.markets().size() ||
        system >= m_galaxy.systems.size()) {
        return false;
    }
    const auto stationOf = [&](std::uint32_t market) {
        const sim::StationMarket& row = m_economy.markets()[market];
        return m_galaxy.systems[row.systemIndex].stations[row.stationIndex].position;
    };
    const core::DVec3 origin = stationOf(route.fromMarket);
    const core::DVec3 destination = stationOf(route.toMarket);
    const sim::SystemSpec& spec = m_galaxy.systems[system];
    const auto hops = [&](std::uint32_t a, std::uint32_t b) {
        return static_cast<std::uint32_t>(m_economy.hopCount(a, b));
    };

    // Its slot in the lane, applied to BOTH ends: offsetting only the spawn
    // would let the whole convoy converge again on the way in and pile up at
    // the destination, which is the same stack one leg later.
    const auto inSlot = [&](const core::DVec3& start, const core::DVec3& end) {
        const core::DVec3 offset = sim::laneSlotOffset(slot, end - start, kTraderLaneSpacing);
        from = start + offset;
        to = end + offset;
    };

    if (route.hops == 0) {
        // No gate in it at all: one straight run, and the two leg windows are
        // its halves (sim::hoplessProgress owns that fold) — so it is quoted
        // at both endpoints' time rather than one.
        inSlot(origin, destination);
        progress = sim::hoplessProgress(route.leg, route.progress);
        out.legSeconds = m_economy.params().traderLegSeconds * 2.0;
        return true;
    }
    out.legSeconds = m_economy.params().traderLegSeconds;

    // The far end of the trip is out of this system, so the leg runs to or
    // from whichever gate starts the shortest path — the same table that
    // quoted the leg its travel time, so the body flies the route the economy
    // actually planned.
    const std::uint32_t otherSystem =
        m_economy.markets()[route.leg == sim::TraderLeg::Depart ? route.toMarket : route.fromMarket]
            .systemIndex;
    const std::uint32_t gate =
        sim::gateTowardSystem(std::span<const sim::GateSpec>(spec.gates), system, otherSystem, hops);
    if (gate == sim::kNoGate) {
        return false; // no lane out of here toward it: draw nothing rather than a guess
    }
    if (route.leg == sim::TraderLeg::Depart) {
        inSlot(origin, spec.gates[gate].position);
    } else {
        inSlot(spec.gates[gate].position, destination);
    }
    progress = route.progress;
    return true;
}

void SpaceWorld::beginPuppetReconcile(double dt)
{
    // Re-asserted from scratch every tick: a hold on a trader's clock is a
    // fact about what is happening to its body right now, and a body that is
    // gone can hold nothing.
    m_puppetPresent.assign(m_economy.traders().size(), 0);
    m_economy.clearDetained();
    // The captains' two, on the same footing and for the same reason: both are
    // indexed by a galaxy-wide id, so both are cleared ONCE above the bubble
    // loop. A captain under fire is a fact about their body right now, and a
    // body that is gone can hold nothing.
    m_captainPresent.assign(m_captains.size(), 0);
    m_captainDetained.assign(m_captains.size(), 0);

    const std::size_t marketCount = m_economy.markets().size();
    if (m_minerHold.size() != marketCount) {
        m_minerHold.assign(marketCount, 0.0);
    }
    for (double& hold : m_minerHold) {
        hold = std::max(0.0, hold - dt);
    }
    m_minerPresent.assign(marketCount, 0);
}

void SpaceWorld::syncTraderPuppets(SystemBubble& bubble)
{
    if (m_defs == nullptr || m_factionTable.empty() || m_economy.markets().empty()) {
        return;
    }
    ecs::Registry& registry = bubble.registry;
    const std::size_t fleet = m_economy.traders().size();

    // Existing bodies first: anything whose record has moved on stops being
    // here, and anything whose leg has changed under it is rebuilt rather than
    // left flying at a destination nobody is going to.
    std::vector<ecs::Entity> doomed;
    ecs::Pool<TraderPuppet>& puppets = registry.storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        TraderPuppet& puppet = puppets.values()[i];
        const ecs::Entity entity = registry.entityFromIndex(puppets.entityIndices()[i]);
        TraderLegPlacement leg;
        if (puppet.traderIndex >= fleet || !traderLegSegment(puppet.traderIndex, bubble.system, leg) ||
            length(leg.to - puppet.destination) > 1.0) {
            doomed.push_back(entity);
            continue;
        }
        m_puppetPresent[puppet.traderIndex] = 1;
        // A puppet's ROUTE is not Lua's to choose — it belongs to the record —
        // but its fight-or-flight is. So Attack and Flee are left alone, while
        // Idle (a fight it walked away from) and Patrol (init.lua's canned
        // trader loop, which would fly it in circles off its own lane) are put
        // back on the leg it is actually here to fly. Done before the pacing
        // below, which declines to touch anything not on its leg.
        ShipPilot* pilot = registry.tryGet<ShipPilot>(entity);
        if (pilot != nullptr && pilot->threatTimer > 0.0f) {
            m_economy.detainTrader(puppet.traderIndex);
        }
        if (pilot != nullptr && (pilot->state == PilotState::Idle || pilot->state == PilotState::Patrol)) {
            pilot->state = PilotState::Travel;
        }
        if (pilot != nullptr && pilot->state == PilotState::Travel) {
            pilot->waypoint = leg.to;
        }
        // Recorded rather than recomputed by prey selection: this is the only
        // place that knows which of the two clocks moved the hauler this tick.
        puppet.paced = keepTraderOnSchedule(registry, entity, leg) ? 1u : 0u;
    }
    for (const ecs::Entity entity : doomed) {
        despawnShip(bubble, entity.index);
    }

    // Then the fleet: every trader flying a leg here that has no body yet gets
    // one, placed where the record says it already is rather than at the start
    // of its leg — the player arriving mid-haul must find traffic in progress.
    for (std::uint32_t t = 0; t < fleet; ++t) {
        if (m_puppetPresent[t] != 0) {
            continue;
        }
        TraderLegPlacement leg;
        if (!traderLegSegment(t, bubble.system, leg)) {
            continue;
        }
        const sim::TraderRoute route = m_economy.route(t);
        // Allegiance follows the ground it is trading between, which is stable
        // for the whole leg. Never left unaffiliated: Lua reads that as
        // unconditionally player-hostile (the pre-8b rule), and a hauler
        // opening fire on sight is the one thing this must not be.
        std::uint32_t faction = systemOwnerFaction(m_economy.markets()[route.fromMarket].systemIndex);
        if (faction >= m_factionTable.size()) {
            faction = systemOwnerFaction(m_economy.markets()[route.toMarket].systemIndex);
        }
        if (faction >= m_factionTable.size()) {
            faction = 0;
        }
        const GameFaction& owner = m_factionTable[faction];
        const std::span<const std::string> roster =
            factionRoster(owner, assets::RosterCell::Trader, assets::RosterCell::Patrol);
        if (roster.empty()) {
            continue;
        }
        // ⚑ The hull carries what the hauler is carrying (Phase 8x stage 6).
        // Stage 2 keyed this on the trader index alone, which put freighters
        // and shuttles on the same lanes at random; reading the load instead
        // makes the sky mean something, because a coarse haul is laden inbound
        // and a deadhead outbound. It stays stable for the whole leg without
        // being pinned to the index: cargo is bought at one end of a haul and
        // sold at the other, so a body cannot change ship under the player.
        const assets::ShipDef* def = nullptr;
        const std::string* defId = nullptr;
        m_rosterCapacities.clear();
        for (const std::string& id : roster) {
            const assets::ShipDef* candidate = m_defs->findShip(id.c_str());
            m_rosterCapacities.push_back(candidate != nullptr ? candidate->cargoCapacity : 0.0f);
        }
        const std::uint32_t hull = sim::chooseTraderHull(
            std::span<const float>(m_rosterCapacities), m_economy.traders()[t].cargo, t);
        if (hull < roster.size()) {
            defId = &roster[hull];
            def = m_defs->findShip(defId->c_str());
        }
        if (def == nullptr) {
            SOL_LOG_WARN("trader puppet: no ship def '%s'", defId != nullptr ? defId->c_str() : "?");
            continue;
        }
        const core::DVec3 position = traderScheduledPoint(leg);
        const ecs::Entity entity = spawnShipAt(bubble, *def, *m_defs, position, owner.name.c_str());
        registry.emplace<ShipPilot>(entity,
                                    ShipPilot{.role = PilotRole::Trader,
                                              .state = PilotState::Travel,
                                              .waypoint = leg.to,
                                              .factionIndex = faction});
        registry.emplace<TraderPuppet>(entity, TraderPuppet{.traderIndex = t, .destination = leg.to});
        // Pointed down its lane on the frame it appears. steerTravel would
        // turn it anyway, but a system full of haulers facing nowhere is what
        // the player would see in the first second after a jump.
        Transform& transform = registry.storage<Transform>().get(entity.index);
        transform.orientation = lookAlong(leg.to - position);
        transform.previousOrientation = transform.orientation;
        // Appearing mid-leg is the normal case, so a new body has to answer
        // the same question the reconcile above answers: is the record flying
        // this one? Skipping it would leave a hauler briefly advertised as
        // huntable while being uncatchable, and hand it its lane speed a tick
        // late into the bargain.
        registry.storage<TraderPuppet>().get(entity.index).paced =
            keepTraderOnSchedule(registry, entity, leg) ? 1u : 0u;
        m_puppetPresent[t] = 1;
    }
}

void SpaceWorld::syncCaptainPuppets(SystemBubble& bubble)
{
    if (m_defs == nullptr || m_factionTable.empty() || m_economy.markets().empty() || m_captains.empty()) {
        return;
    }
    ecs::Registry& registry = bubble.registry;

    // Existing bodies first: a captain whose record has moved on stops being
    // here, and one whose leg changed under them is rebuilt rather than left
    // flying at a destination nobody is going to. `syncTraderPuppets`' opening,
    // against a different record.
    std::vector<ecs::Entity> doomed;
    ecs::Pool<CaptainPuppet>& puppets = registry.storage<CaptainPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        CaptainPuppet& puppet = puppets.values()[i];
        const ecs::Entity entity = registry.entityFromIndex(puppets.entityIndices()[i]);
        // ⚑⚑⚑ A HULL THIS FUNCTION DOES NOT OWN IS NOT ITS TO JUDGE, AND WITHOUT
        // THIS LINE IT DELETES ONE EVERY TICK (stage C). The test below is "is
        // this body still on the leg the record says it is flying", and an order
        // with no leg answers false - so the hull is doomed one tick after the
        // function that owns it spawned it, forever.
        //
        // ⚑⚑⚑⚑ AND STAGE D ADDED A THIRD OWNER WITHOUT WIDENING THE DIVIDER,
        // WHICH IS THIS COMMENT'S OWN WARNING COMING TRUE ONE STAGE AFTER IT WAS
        // WRITTEN. `escorting()` is not `stationary()` - deliberately, because
        // an escort holds no bubble - so an escort's hull fell straight through
        // to the doom below and was destroyed and respawned EVERY TICK. It
        // looked like it worked: a body was always there when anything asked.
        // What it actually was is a brand new hull sixty times a second, with
        // full shields, no memory of the fight it was in, and an order it could
        // never finish standing down from. ⚑ Caught by asserting that the body
        // is the SAME BODY rather than that a body exists - which is Phase 38's
        // "a count, not a state" pointed at an entity id.
        //
        // One component, three owners now, and the ORDER is what divides them.
        if (puppet.captainIndex < m_captains.size() &&
            !itinerant(m_captains[puppet.captainIndex].order.kind)) {
            continue;
        }
        TraderLegPlacement leg;
        if (puppet.captainIndex >= m_captains.size() ||
            !captainLegSegment(puppet.captainIndex, bubble.system, leg) ||
            length(leg.to - puppet.destination) > 1.0) {
            doomed.push_back(entity);
            continue;
        }
        m_captainPresent[puppet.captainIndex] = 1;
        ShipPilot* pilot = registry.tryGet<ShipPilot>(entity);
        // Being shot at holds the haul's clock, exactly as it holds a coarse
        // trader's - and this is the one place that can see it, because a
        // threat is a fact about a BODY and the coarse layer has none.
        if (pilot != nullptr && pilot->threatTimer > 0.0f) {
            m_captainDetained[puppet.captainIndex] = 1;
        }
        // A captain's ROUTE is not Lua's to choose - it belongs to the record -
        // but its fight-or-flight is. Attack and Flee are left alone; Idle and
        // Patrol are put back on the leg the record says they are flying.
        if (pilot != nullptr && (pilot->state == PilotState::Idle || pilot->state == PilotState::Patrol)) {
            pilot->state = PilotState::Travel;
        }
        if (pilot != nullptr && pilot->state == PilotState::Travel) {
            pilot->waypoint = leg.to;
        }
        puppet.paced = keepTraderOnSchedule(registry, entity, leg) ? 1u : 0u;
    }
    for (const ecs::Entity entity : doomed) {
        despawnShip(bubble, entity.index);
    }

    // Then the people: every captain flying a leg here with no body yet gets
    // one, placed where the RECORD says they already are rather than at the
    // start of the leg. Walking in on a haul must find it in progress.
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(m_captains.size()); ++c) {
        if (m_captainPresent[c] != 0) {
            continue;
        }
        if (stationary(m_captains[c].order.kind)) {
            continue; // `tickStationaryCaptains` gives that one its body
        }
        TraderLegPlacement leg;
        if (!captainLegSegment(c, bubble.system, leg)) {
            continue;
        }
        const Captain& captain = m_captains[c];
        if (captain.ship >= m_fleet.size()) {
            continue;
        }
        // THE HULL IS THE PLAYER'S OWN, WEARING THE PLAYER'S FIT, THROUGH THE
        // FUNCTION WHOSE COMMENT ALREADY SAID IT COULD BE ASKED WITHOUT BEING
        // DOCKED. `resolvedShipDef` is "the ship as flown" - since Phase 31
        // stage B its mounts carry what is actually in them - so there is no
        // new hull model in this phase and the freighter the player meets on
        // the lane is the one they outfitted, not a generic one of its class.
        const assets::ShipDef def = resolvedShipDef(m_fleet[captain.ship]);
        // Allegiance follows the ground it is trading between, which is a
        // trader's own rule and is WRONG in a way stage D exists to fix: this
        // hull inherits that faction's wars and reads to prey selection as that
        // faction's hauler. It is never left unaffiliated, because Lua reads
        // that as unconditionally player-hostile and a freighter of yours
        // opening fire on you is the one thing this must not be.
        const sim::TraderRoute route = captainRoute(c);
        std::uint32_t faction = systemOwnerFaction(m_economy.markets()[route.fromMarket].systemIndex);
        if (faction >= m_factionTable.size()) {
            faction = systemOwnerFaction(m_economy.markets()[route.toMarket].systemIndex);
        }
        if (faction >= m_factionTable.size()) {
            faction = 0;
        }
        const core::DVec3 position = traderScheduledPoint(leg);
        // THE NAME IN THE SLOT A FACTION WOULD HAVE TAKEN, and it is the whole
        // payoff of the stage's exit: target the hull and the game says "Bex
        // Torvald", not "Hegemony". A person is what this phase added, and the
        // sky is where the player finds out it worked.
        const ecs::Entity entity = spawnShipAt(bubble, def, *m_defs, position, captain.name.c_str());
        registry.emplace<ShipPilot>(entity,
                                    ShipPilot{.role = PilotRole::Trader,
                                              .state = PilotState::Travel,
                                              .waypoint = leg.to,
                                              .factionIndex = faction});
        registry.emplace<CaptainPuppet>(entity, CaptainPuppet{.captainIndex = c, .destination = leg.to});
        Transform& transform = registry.storage<Transform>().get(entity.index);
        transform.orientation = lookAlong(leg.to - position);
        transform.previousOrientation = transform.orientation;
        registry.storage<CaptainPuppet>().get(entity.index).paced =
            keepTraderOnSchedule(registry, entity, leg) ? 1u : 0u;
        m_captainPresent[c] = 1;
        SOL_LOG_INFO("%s's %s is in %s",
                     captain.name.c_str(),
                     def.name.c_str(),
                     m_galaxy.systems[bubble.system].name.c_str());
    }
}

std::uint32_t
SpaceWorld::chooseCaptainOre(const ecs::Registry& registry, std::uint32_t market, float load) const
{
    if (market >= m_economy.markets().size() || load <= 0.0f) {
        return kNoIndex;
    }
    const ecs::Pool<MineableRock>& rocks = registry.storage<MineableRock>();
    std::uint32_t best = kNoIndex;
    float bestValue = 0.0f;
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const std::uint32_t commodity = rocks.values()[i].commodity;
        if (commodity == best) {
            continue; // the run of one field's rocks; the quote does not change
        }
        const float value = m_economy.quoteSell(market, commodity, load);
        if (best == kNoIndex || value > bestValue) {
            best = commodity;
            bestValue = value;
        }
    }
    return best;
}

bool SpaceWorld::chooseCaptainRock(const ecs::Registry& registry,
                                   const Captain& captain,
                                   CaptainPuppet& puppet,
                                   const core::DVec3& from,
                                   bool sameField) const
{
    // ⚑⚑ `MinerPuppet`'s CHOICE, MADE FOR A DIFFERENT MINER - AND IT IS THE
    // SAME FUNCTION RATHER THAN THE SAME IDEA WRITTEN TWICE. `chooseMinerRock`
    // takes a `MinerPuppet&` because that is the only miner this game had; the
    // three fields it actually reads and writes are a commodity, a field and a
    // rock, and a captain's body has all three. So this is a shim that borrows
    // the record rather than a second rock-picking rule to keep in step - which
    // matters more than it looks, because the rule it would have to keep in
    // step with is "the straight line to the next rock must miss every other
    // rock", and that one was written after watching a miner die.
    MinerPuppet borrowed{.market = captain.order.marketA,
                         .commodity = captain.mine.commodity,
                         .field = captain.mine.field,
                         .rock = puppet.rock,
                         .rockSeconds = 0.0f,
                         .rockStep = captain.mine.rockStep};
    if (!chooseMinerRock(registry, borrowed, from, sameField)) {
        return false;
    }
    puppet.rock = borrowed.rock;
    return true;
}

bool SpaceWorld::settleCaptainMineSale(Captain& captain)
{
    CaptainMine& mine = captain.mine;
    const std::uint32_t market = captain.order.marketA;
    if (mine.units <= 0.0f || market >= m_economy.markets().size()) {
        return false;
    }
    const float aboard = mine.units;
    const sim::TradeResult sold = m_economy.sell(market, mine.commodity, aboard);
    if (sold.units <= 0.0f) {
        // A full warehouse. The load stays aboard and the captain goes back to
        // the rock with it - `Economy`'s own answer at a haul's far end, for its
        // own reason: tipping a hold into space is a galaxy-wide leak of goods.
        // The hold is already at its stop fraction, so nothing more is cut and
        // the next run in is the one that clears.
        //
        // ⚑⚑ AND THE CALLER IS TOLD, BECAUSE THIS IS NOT ALWAYS TEMPORARY. Two
        // hours of measurement says a captain reaches this line and never
        // leaves it: one freighter fills one station's ore capacity in half an
        // hour and the price sits on its floor thereafter. "The next run in is
        // the one that clears" is true of a busy market and false of a full
        // one, and nothing here could tell them apart.
        return false;
    }
    // ⚑⚑⚑⚑ RULING 6 NEEDED NO SPECIAL CASE HERE, AND THAT IS THE STAGE'S
    // CHEAPEST FINDING. "The cut is of the PROFIT, never of the sale" was
    // written against a haul, where the hold's cost has to come off first or a
    // thin margin pays the captain more than the run made. Ore out of the
    // ground has no cost: nobody bought it, so the basis is zero and the profit
    // IS the gross. The same sentence, evaluated against a different outlay,
    // gives the answer a mining captain should get - a straight share of what
    // the ore fetched - without a second rule anywhere.
    const double gross = static_cast<double>(sold.credits);
    const double cut = gross > 0.0 ? gross * static_cast<double>(captain.cut) : 0.0;
    m_playerCredits += gross - cut;
    captain.ledger.earned += gross - cut;
    captain.ledger.paid += cut;
    mine.units = std::max(0.0f, aboard - sold.units);
    const sim::StationMarket& row = m_economy.markets()[market];
    SOL_LOG_INFO("%s sold %.0f %s at %s for %.0f cr (%.0f to them)",
                 captain.name.c_str(),
                 static_cast<double>(sold.units),
                 mine.commodity < m_commodityIds.size() ? m_commodityIds[mine.commodity].c_str() : "ore",
                 m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name.c_str(),
                 gross,
                 cut);
    return true;
}

void SpaceWorld::tickStationaryCaptains(SystemBubble& bubble, double dt)
{
    if (m_defs == nullptr || m_factionTable.empty() || m_economy.markets().empty() || m_captains.empty()) {
        return;
    }
    ecs::Registry& registry = bubble.registry;

    // Bodies first, on `syncCaptainPuppets`' opening and for its reason: a
    // captain whose order has been taken away stops having a hull here.
    std::vector<ecs::Entity> doomed;
    ecs::Pool<CaptainPuppet>& puppets = registry.storage<CaptainPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        CaptainPuppet& puppet = puppets.values()[i];
        const ecs::Entity entity = registry.entityFromIndex(puppets.entityIndices()[i]);
        if (puppet.captainIndex >= m_captains.size()) {
            continue; // `syncCaptainPuppets` owns that one and dooms it there
        }
        Captain& captain = m_captains[puppet.captainIndex];
        if (!stationary(captain.order.kind)) {
            continue;
        }
        if (captainSystem(puppet.captainIndex) != bubble.system || captain.ship >= m_fleet.size() ||
            captain.order.marketA >= m_economy.markets().size()) {
            doomed.push_back(entity);
            continue;
        }
        m_captainPresent[puppet.captainIndex] = 1;
        ShipPilot* pilot = registry.tryGet<ShipPilot>(entity);
        Transform* transform = registry.tryGet<Transform>(entity);
        if (pilot == nullptr || transform == nullptr) {
            continue;
        }

        // ⚑⚑⚑⚑ THE SECOND STATIONARY ORDER FORKS HERE, ABOVE THE THREE LINES
        // BELOW RATHER THAN UNDER THEM - AND PUTTING IT UNDER THEM IS THE BUG A
        // LIVE FLIGHT FOUND. Those lines are the MINING state machine: being
        // shot at stops the work, and a hull that is not in `Travel` is
        // fighting, so the tick leaves it to Lua and the steering. For a patrol
        // both readings invert. Being shot at IS the work; and `Attack` is the
        // state this function itself just set, so `state != Travel -> continue`
        // meant a patrol was skipped on every tick after the one where it
        // spotted something. It locked on across the system and then never ran
        // the code that closes the distance - which is why the fix in
        // `tickPatrolBeat` measured as no fix at all until this moved.
        //
        // ⚑ `stationary()` still decides the REPRESENTATION in one place; this
        // decides the JOB, and the job now owns its whole state machine.
        if (captain.order.kind == OrderKind::Patrol) {
            tickPatrolBeat(bubble, entity, puppet, captain, dt);
            continue;
        }

        // Being shot at stops the work and nothing else does. `syncMinerPuppets`'
        // rule, word for word: Attack and Flee are Lua's, Idle and Patrol go
        // back on the job, and the threat has to have gone cold first or the
        // tick after a fight ends sends the hull straight back to the rock it
        // was being shot off.
        if (pilot->threatTimer > 0.0f) {
            m_captainDetained[puppet.captainIndex] = 1;
            continue;
        }
        if (pilot->state == PilotState::Idle || pilot->state == PilotState::Patrol) {
            pilot->state = PilotState::Travel;
        }
        if (pilot->state != PilotState::Travel) {
            continue;
        }

        CaptainMine& mine = captain.mine;
        const sim::StationMarket& row = m_economy.markets()[captain.order.marketA];
        const core::DVec3 dock = m_galaxy.systems[row.systemIndex].stations[row.stationIndex].position;
        const float capacity = resolvedShipDef(m_fleet[captain.ship]).cargoCapacity;
        const float stopAt = capacity * kCaptainHoldFullFraction;

        if (mine.phase == MinePhase::Selling) {
            puppet.destination = dock;
            pilot->waypoint = dock;
            (void)cruiseCaptainToward(registry, entity, dock, kCaptainDeliverRange * 0.5, dt);
            if (length(transform->position - dock) > kCaptainDeliverRange) {
                continue;
            }
            const bool sold = settleCaptainMineSale(captain);
            // ⚑⚑⚑⚑ THE WAREHOUSE THAT WILL NOT TAKE ANY MORE (stage E, the
            // user's ruling 18). Measured over two hours on the shipped galaxy:
            // one freighter fills one station's ore capacity in about thirty
            // minutes and then earns NOTHING for the rest of the run, standing
            // at the counter with a full hold. Before this, the game said
            // nothing at all about it - the Crew tab went on reading "taking a
            // load in" and the rock count simply stopped moving, so a captain
            // that had permanently stopped working looked exactly like one
            // between trips. That is the "looks identical to a broken one"
            // failure this phase has now hit three times, and here it was not
            // even a false alarm: the captain really had stopped.
            if (sold) {
                mine.stalledSeconds = 0.0;
            } else if (mine.units > 0.0f) {
                // The FIRST tick of a stall is the one that speaks, which is
                // what makes `kMinerStallSeconds` uncritical: the player knows
                // immediately and the timer only decides when the captain gives
                // up, not when they are told.
                if (mine.stalledSeconds <= 0.0) {
                    const std::string& where =
                        m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name;
                    char line[192] = {};
                    std::snprintf(
                        line, sizeof(line), "%s will not take any more - holding the load", where.c_str());
                    say(captain.name, line);
                    SOL_LOG_WARN(
                        "%s cannot sell at %s: the warehouse is full", captain.name.c_str(), where.c_str());
                }
                mine.stalledSeconds += dt;
            }
            if (mine.stalledSeconds >= kMinerStallSeconds) {
                OwnedShip& stalledHull = m_fleet[captain.ship];
                stalledHull.storedSystem = row.systemIndex;
                stalledHull.storedStation = row.stationIndex;
                captain.order = {};
                captain.mine = {};
                SOL_LOG_WARN("%s stands down at %s with nowhere to sell",
                             captain.name.c_str(),
                             m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name.c_str());
                say(captain.name, "nothing more to do here - standing down");
                doomed.push_back(entity);
                continue;
            }
            if (captain.order.stopping) {
                // Stood down at the counter, and the hull is parked where it
                // landed - `captainArrive`'s line, which is what makes "fly to
                // one of them and find them where the screen said they were"
                // answerable through the field every other screen already reads.
                OwnedShip& hull = m_fleet[captain.ship];
                hull.storedSystem = row.systemIndex;
                hull.storedStation = row.stationIndex;
                captain.order = {};
                captain.mine = {};
                SOL_LOG_INFO("%s stands down at %s",
                             captain.name.c_str(),
                             m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name.c_str());
                doomed.push_back(entity);
                continue;
            }
            // ⚑⚑⚑ A HOLD THAT DID NOT CLEAR KEEPS THE HULL AT THE COUNTER, AND
            // THE ALTERNATIVE IS A SHUTTLE THAT NEVER STOPS. `settleCaptainSale`'s
            // own rule is that a market with no room leaves the load aboard, so
            // this can return with the hold exactly as full as it arrived - and
            // sending it back out then means arriving at a rock with no room,
            // turning straight round, and flying the leg again forever. It waits
            // instead, which is what a hauler with nowhere to put a load actually
            // does, and the readout says "taking a load in" the whole time.
            if (mine.units >= stopAt) {
                continue;
            }
            // Back out to the rock. The hold may still hold something - a market
            // that could take only part of the load - and that is fine: the stop
            // fraction is a ceiling, so a part-full hull simply cuts less before
            // its next run in.
            mine.phase = MinePhase::Cutting;
            mine.rockSeconds = 0.0; // pick a rock on the next tick
            continue;
        }

        // Cutting. The rock is settled first, because everything below is about
        // one, and then the ore comes out of the ground at the rate the fit cuts
        // at.
        // ⚑ THE ROCK CLOCK ONLY RUNS AT THE ROCK. A minute a rock is a minute
        // of WORK, and counting it down during a crossing would have a captain
        // arrive at a rock it has already decided to leave - which reads, from
        // outside, as a hull that flies between rocks and never cuts.
        if (length(transform->position - puppet.destination) <= kCaptainCruiseInside) {
            mine.rockSeconds -= dt;
        }
        const MineableRock* rock =
            puppet.rock != kNoIndex ? registry.storage<MineableRock>().tryGet(puppet.rock) : nullptr;
        if (rock == nullptr || mine.rockSeconds <= 0.0) {
            // ⚑⚑⚑ THE ORE IS DECIDED WHEN THE HOLD IS EMPTY AND NOT BEFORE,
            // AND THE DEFAULT WAS A BUG WAITING TO BE ONE. `chooseMinerRock`
            // filters on `MinerPuppet::commodity` because an outpost sells one
            // thing and works the rock holding it - so a captain arriving with
            // an unset commodity would have hunted for commodity ZERO, found
            // none of it, and stood in an asteroid field forever reporting
            // that the field was worked out. Nothing about that reads as a
            // wrong DEFAULT; it reads as mining being broken.
            if (mine.units <= 0.0f) {
                const std::uint32_t ore = chooseCaptainOre(registry, captain.order.marketA, stopAt);
                if (ore == kNoIndex) {
                    mine.phase = MinePhase::Selling;
                    continue;
                }
                mine.commodity = ore;
            }
            const bool sameField = rock != nullptr;
            if (!chooseCaptainRock(registry, captain, puppet, transform->position, sameField)) {
                // ⚑⚑ THE FIELD IS WORKED OUT UNDER THEM, AND THE ANSWER IS TO GO
                // AND SELL RATHER THAN TO STOP. A captain sitting at a dead rock
                // with a half hold is the one failure of this order a player
                // could not tell from a broken one, and there is nothing better
                // for them to do: the order names this system, and the ore grows
                // back (`rockRegenPerSecond`), so the run in IS the wait.
                mine.phase = MinePhase::Selling;
                continue;
            }
            rock = registry.storage<MineableRock>().tryGet(puppet.rock);
            if (rock == nullptr) {
                mine.phase = MinePhase::Selling;
                continue;
            }
            mine.field = rock->field;
            mine.rockSeconds = kMinerRockSeconds;
            ++mine.rockStep;
        }
        const Transform* rockTransform = registry.storage<Transform>().tryGet(puppet.rock);
        const RenderShape* rockShape = registry.storage<RenderShape>().tryGet(puppet.rock);
        if (rockTransform == nullptr || rockShape == nullptr) {
            continue;
        }

        // On the dock's side of the rock, the rule `minerWorkPoint` follows: a
        // ship coming out from the station meets the miner rather than the rock
        // it is hiding behind.
        const core::DVec3 hold = sim::minerHoldPoint(rockTransform->position,
                                                     static_cast<double>(rockShape->scale.x),
                                                     dock - rockTransform->position,
                                                     kMinerRockClearance);
        puppet.destination = hold;
        pilot->waypoint = hold;
        (void)cruiseCaptainToward(registry, entity, hold, kTraderArrivalRange, dt);
        // ⚑⚑⚑ ORE ONLY COMES OUT WHILE THE HULL IS ACTUALLY AT THE ROCK, AND
        // THAT IS WHAT MAKES THE FLYING COST SOMETHING. Without it a captain
        // earns the same whether the field is beside the dock or half a
        // playfield out, every hop between rocks is free, and the body becomes
        // scenery drawn over an accrual - which is the "it is a spreadsheet"
        // failure stage B's promotion exists to avoid, arrived at from the
        // other side.
        if (length(transform->position - hold) > kMinerRockClearance + kTraderArrivalRange) {
            continue;
        }
        const float room = stopAt - mine.units;
        if (room <= 0.0f) {
            mine.phase = MinePhase::Selling;
            continue;
        }
        const float wanted = std::min(room, shipMiningPower(m_fleet[captain.ship]) * static_cast<float>(dt));
        // ⚑⚑⚑⚑ THROUGH `MiningSim::mineRock`, WHICH IS THE WHOLE OF WHY THIS IS
        // NOT AN ACCRUAL WITH A SHIP DRAWN NEXT TO IT. The rock a captain cuts
        // is depleted in the same sparse record the player's own beam writes and
        // an outpost's draw reads, so a captain working a field takes ore out of
        // a mining station's supply and out of the player's next visit. One
        // finite resource, three consumers - this file's rule since Phase 8f,
        // and it needed nothing new to hold.
        const float taken =
            m_mining.mineRock(bubble.system, rock->field, rock->index, rock->totalUnits, wanted);
        if (taken <= 0.0f) {
            mine.rockSeconds = 0.0; // cut to nothing; move on next tick
            continue;
        }
        mine.units += taken;
        if (m_mining.unitsLeft(bubble.system, rock->field, rock->index, rock->totalUnits) <= 0.0f) {
            registry.destroy(registry.entityFromIndex(puppet.rock)); // it broke up
            puppet.rock = kNoIndex;
            mine.rockSeconds = 0.0;
        }
        if (mine.units >= stopAt) {
            mine.phase = MinePhase::Selling;
        }
    }
    for (const ecs::Entity entity : doomed) {
        despawnShip(bubble, entity.index);
    }

    // Then the people: a captain posted here with no hull in the sky yet gets
    // one. Placed at the DOCK rather than out at a rock, because that is where
    // they were standing when the order was given, and the flight out is the
    // first thing the order costs.
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(m_captains.size()); ++c) {
        if (m_captainPresent[c] != 0) {
            continue;
        }
        const Captain& captain = m_captains[c];
        if (!stationary(captain.order.kind) || captainSystem(c) != bubble.system ||
            captain.ship >= m_fleet.size() || captain.order.marketA >= m_economy.markets().size()) {
            continue;
        }
        const assets::ShipDef def = resolvedShipDef(m_fleet[captain.ship]);
        const sim::StationMarket& row = m_economy.markets()[captain.order.marketA];
        const core::DVec3 dock = m_galaxy.systems[row.systemIndex].stations[row.stationIndex].position;
        // Allegiance still follows the ground it works, and it no longer decides
        // anything the player can feel: stage D moved every hostility question
        // onto `playerOwnedHull`, and what is left here is a row for the NPC-vs-
        // NPC readers that have always needed one. Never unaffiliated, because
        // Lua reads that as unconditionally player-hostile.
        std::uint32_t faction = systemOwnerFaction(row.systemIndex);
        if (faction >= m_factionTable.size()) {
            faction = 0;
        }
        // ⚑⚑⚑ AND THE NAME IS THE PERSON'S, WHICH THIS ARM HAD WRONG SINCE
        // STAGE C. The itinerant spawn writes `captain.name` and says in its own
        // comment why - "target the hull and the game says Bex Torvald, not
        // Hegemony" - and this one wrote `m_factionTable[faction].name`, so
        // exactly the hull the player flies two systems to check on was the one
        // that would not tell them whose it was. Found by pointing the contact
        // panel at a mining captain while writing this stage.
        const ecs::Entity entity = spawnShipAt(bubble, def, *m_defs, dock, captain.name.c_str());
        registry.emplace<ShipPilot>(
            entity,
            ShipPilot{.role = captain.order.kind == OrderKind::Patrol ? PilotRole::Patrol : PilotRole::Trader,
                      .state = PilotState::Travel,
                      .waypoint = dock,
                      .factionIndex = faction});
        registry.emplace<CaptainPuppet>(entity, CaptainPuppet{.captainIndex = c, .destination = dock});
        m_captainPresent[c] = 1;
        SOL_LOG_INFO("%s's %s is %s in %s",
                     captain.name.c_str(),
                     def.name.c_str(),
                     captain.order.kind == OrderKind::Patrol ? "on the beat" : "working the rock",
                     m_galaxy.systems[bubble.system].name.c_str());
    }
}

namespace {
// Declared here and defined with the other pilot helpers further down: stage D
// is the first caller ABOVE that definition, because a captain under a combat
// order picks its own target in C++ rather than waiting for `pilot_think`.
sol::sim::PowerPips pipsForPilot(PilotState state);
} // namespace

void SpaceWorld::tickEscortCaptains(SystemBubble& bubble, double dt)
{
    // ⚑⚑⚑⚑ THE PLAYER'S BUBBLE AND NOWHERE ELSE, WHICH IS THE ORDER'S OWN
    // DEFINITION AND NOT A RESTRICTION ON IT (stage D). An escort is defined as
    // being where the player is; the player is in the front bubble; so every
    // other bubble has nothing to do here. ⚑ It is checked rather than assumed
    // because `tick` calls this from inside the per-system loop, which is where
    // the two calls above it belong and where this one has exactly one system
    // to act on.
    if (m_defs == nullptr || m_captains.empty() || &bubble != m_bubbles.front().get()) {
        return;
    }
    ecs::Registry& registry = bubble.registry;

    // Bodies first, `tickStationaryCaptains`' opening and its reason: an escort
    // whose order has been taken away, or who followed you through a gate,
    // stops having a hull HERE. ⚑ The jump is the interesting case and it needs
    // no code of its own: a gate leaves this bubble behind and the arm below
    // gives the captain a fresh body in the new one, which is the same bargain
    // `MinerPuppet` makes about a rock - the record is durable and the body is
    // rebuilt wherever it is needed.
    std::vector<ecs::Entity> doomed;
    ecs::Pool<CaptainPuppet>& puppets = registry.storage<CaptainPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        CaptainPuppet& puppet = puppets.values()[i];
        const ecs::Entity entity = registry.entityFromIndex(puppets.entityIndices()[i]);
        if (puppet.captainIndex >= m_captains.size()) {
            continue; // `syncCaptainPuppets` owns that one and dooms it there
        }
        const Captain& captain = m_captains[puppet.captainIndex];
        if (!escorting(captain.order.kind)) {
            continue;
        }
        if (captain.ship >= m_fleet.size()) {
            doomed.push_back(entity);
            continue;
        }
        m_captainPresent[puppet.captainIndex] = 1;
        ShipPilot* pilot = registry.tryGet<ShipPilot>(entity);
        Transform* transform = registry.tryGet<Transform>(entity);
        if (pilot == nullptr || transform == nullptr) {
            continue;
        }
        // Called off: in to the nearest pad in whatever system you are both
        // standing in, which is the closest an escort has to a home dock -
        // a patrol has the market it was posted from and this order has no
        // place in it at all (`orderEscort`'s own reason for leaving
        // `marketA` unset).
        if (captain.order.stopping) {
            const std::uint32_t pad = nearestStationTo(bubble.system, transform->position);
            if (pad != kNoIndex && standCaptainDownAt(bubble, entity, puppet.captainIndex, pad, dt)) {
                doomed.push_back(entity);
            }
            continue;
        }

        // ⚑⚑ AN ESCORT PICKS FIGHTS AND A HAULER DOES NOT, WHICH IS WHAT
        // `fighting()` NAMES. `tickPatrolBeat` makes the same call against the
        // same reach; what differs is only what it does when the sky is empty,
        // because a patrol has a beat to walk and an escort has you.
        if (pilot->threatTimer <= 0.0f) {
            const std::uint32_t enemy =
                nearestPlayerEnemy(bubble, transform->position, sim::preyReach(m_galaxyParams.gateDistance));
            if (enemy != kNoIndex) {
                pilot->state = PilotState::Attack;
                pilot->targetIndex = enemy;
                pilot->hasTarget = 1;
                if (ShipPower* power = registry.tryGet<ShipPower>(entity)) {
                    power->state.pips = pipsForPilot(pilot->state);
                }
                // Closed on the cruise drive first, for `tickPatrolBeat`'s
                // reason and through the same handover: combat steering takes
                // about a day to cross a system.
                if (const Transform* prey = registry.tryGet<Transform>(registry.entityFromIndex(enemy));
                    prey != nullptr) {
                    (void)cruiseCaptainToward(registry, entity, prey->position, kCaptainEngageRange, dt);
                }
                continue;
            }
            if (pilot->state == PilotState::Attack) {
                pilot->state = PilotState::Travel;
                pilot->hasTarget = 0;
            }
        }
        if (pilot->state == PilotState::Attack || pilot->state == PilotState::Flee) {
            continue; // in a fight; the steering flies it
        }
        // ⚑⚑ STATION IS KEPT OFF THE PLAYER'S SHIP AND THE STANDOFF IS WHY IT
        // IS NOT ZERO. `cruiseCaptainToward` stops at `arrival` and hands the
        // hull to its own steering inside that, so an escort settles a few
        // kilometres off rather than trying to occupy the same point as the
        // ship it is flying with - which is `kCaptainDeliverRange`'s job at a
        // dock, doing the same job against something that moves.
        const core::DVec3 station = shipState().position;
        puppet.destination = station;
        pilot->waypoint = station;
        if (pilot->state == PilotState::Idle || pilot->state == PilotState::Patrol) {
            pilot->state = PilotState::Travel;
        }
        (void)cruiseCaptainToward(registry, entity, station, kCaptainDeliverRange, dt);
    }
    for (const ecs::Entity entity : doomed) {
        despawnShip(bubble, entity.index);
    }

    // Then the people: an escort with no hull in this sky gets one, alongside
    // the player rather than at a dock - they are flying WITH you, and the
    // first thing the order should look like is a second ship on your wing.
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(m_captains.size()); ++c) {
        if (m_captainPresent[c] != 0) {
            continue;
        }
        const Captain& captain = m_captains[c];
        if (!escorting(captain.order.kind) || captain.ship >= m_fleet.size() || isDocked()) {
            continue;
        }
        const assets::ShipDef def = resolvedShipDef(m_fleet[captain.ship]);
        const core::DVec3 at = shipState().position + core::DVec3{kCaptainDeliverRange, 0.0, 0.0};
        std::uint32_t faction = systemOwnerFaction(bubble.system);
        if (faction >= m_factionTable.size()) {
            faction = 0;
        }
        const ecs::Entity entity = spawnShipAt(bubble, def, *m_defs, at, captain.name.c_str());
        registry.emplace<ShipPilot>(entity,
                                    ShipPilot{.role = PilotRole::Patrol,
                                              .state = PilotState::Travel,
                                              .waypoint = shipState().position,
                                              .factionIndex = faction});
        registry.emplace<CaptainPuppet>(
            entity, CaptainPuppet{.captainIndex = c, .destination = shipState().position});
        m_captainPresent[c] = 1;
        SOL_LOG_INFO("%s's %s is on your wing in %s",
                     captain.name.c_str(),
                     def.name.c_str(),
                     m_galaxy.systems[bubble.system].name.c_str());
    }
}

std::uint32_t SpaceWorld::captainBeatLeg(std::size_t captainIndex) const
{
    for (const std::unique_ptr<SystemBubble>& bubble : m_bubbles) {
        const ecs::Registry& registry = bubble->registry;
        const ecs::Pool<ShipPilot>& pilots = registry.storage<ShipPilot>();
        for (std::size_t i = 0; i < pilots.size(); ++i) {
            const std::uint32_t index = pilots.entityIndices()[i];
            const CaptainPuppet* puppet = registry.tryGet<CaptainPuppet>(registry.entityFromIndex(index));
            if (puppet != nullptr && puppet->captainIndex == captainIndex) {
                return puppet->beat;
            }
        }
    }
    return 0;
}

float SpaceWorld::heldBubbleRiskPerSecond(std::uint32_t system) const
{
    // ⚑⚑ THE NUMBER `rollHeldBubbleHazard` ROLLS AGAINST, AS A QUERY. Stage A's
    // rule and Phase 38's silent audio are why it exists at all: a probe that
    // reports a STATE cannot tell "the patrol is helping" from "the patrol is
    // posted", and a test that measures the help by racing two worlds against
    // one shared random stream is measuring a coin flip. This is the rule
    // itself, so a guard that stopped being counted fails immediately rather
    // than half the time.
    if (system >= m_galaxy.systems.size()) {
        return 0.0f;
    }
    const float danger = m_factionSim.danger(system);
    if (danger <= 0.0f) {
        return 0.0f;
    }
    std::uint32_t guards = 0;
    bool exposed = false;
    for (std::size_t i = 0; i < m_captains.size(); ++i) {
        if (!stationary(m_captains[i].order.kind) || captainSystem(i) != system) {
            continue;
        }
        if (m_captains[i].order.kind == OrderKind::Patrol) {
            ++guards;
        }
        exposed = true;
    }
    if (!exposed) {
        return 0.0f;
    }
    float risk = danger * m_factionSim.params().traderLossPerSecond;
    for (std::uint32_t g = 0; g < guards; ++g) {
        risk *= 0.5f;
    }
    return risk;
}

void SpaceWorld::rollHeldBubbleHazard(double dt)
{
    // ⚑⚑⚑⚑ THE GAP THIS CLOSES IS THAT A POSTED CAPTAIN WAS SAFE PRECISELY
    // BECAUSE NOBODY WAS LOOKING, and it took two of this project's own rulings
    // to make it. `rollCaptainAttrition` skips a system that is being simulated
    // - correctly, because rolling a coarse loss against a hull that is also
    // being modelled is the "a captain that is both things" defect the phase's
    // risk register names first - and a stationary captain's system is ALWAYS
    // instantiated, because their order is what holds it open. Meanwhile Phase
    // 38 scoped `pilot_think` to the player's bubble, so nothing in that system
    // ever decides to attack them either. Two correct rules, one hole between.
    //
    // ⚑⚑⚑ SO THE HAZARD IS PRICED WHERE IT LIVES, WHICH IS THE COARSE LAYER
    // (the user's ruling 12, over reopening the fine layer's decisions). The
    // alternative was letting hostility re-target inside every held bubble,
    // which contradicts Phase 38's ruling in its own words and pays per-frame
    // for a fight nobody is watching.
    if (m_captains.empty() || m_factionSim.params().traderLossPerSecond <= 0.0f) {
        return;
    }
    for (std::size_t slot = 0; slot < m_bubbles.size(); ++slot) {
        const SystemBubble& bubble = *m_bubbles[slot];
        // The player's own system is never rolled. It is fully simulated AND
        // watched: a raider there is a raider the player can see, shoot at and
        // lose a captain to honestly, which is `attrition`'s own rule and the
        // reason it has always had one.
        if (slot == 0 || bubble.system >= m_galaxy.systems.size()) {
            continue;
        }
        // ⚑⚑⚑ AND A PATROL POSTED HERE IS WHAT BRINGS IT DOWN, WHICH IS THE
        // ORDER'S WHOLE MEANING WHEN THE PLAYER IS NOT THERE TO WATCH IT WORK.
        // The exit's second half - "the same fight happens in a system you left"
        // - is this roll, and an order that could not change the number would
        // have made "patrol this" a thing you only ever see working in the one
        // system you happen to be standing in.
        // Each guard halves it, and a patrol is exposed to its own roll: a
        // guard that could not be shot at is an invulnerable one, and a system
        // with two of them is safer without being safe. ⚑ THE RULE ITSELF LIVES
        // IN `heldBubbleRiskPerSecond` and is READ here rather than restated,
        // because a probe that computes the number a second way is a probe that
        // can agree with itself while the game does something else.
        const float risk = heldBubbleRiskPerSecond(bubble.system) * static_cast<float>(dt);
        if (risk <= 0.0f) {
            continue;
        }
        std::vector<std::size_t> exposed;
        for (std::size_t i = 0; i < m_captains.size(); ++i) {
            if (stationary(m_captains[i].order.kind) && captainSystem(i) == bubble.system) {
                exposed.push_back(i);
            }
        }
        if (exposed.empty() || m_captainRng.nextFloat01() >= risk) {
            continue;
        }
        // ⚑ THE VICTIM IS THE FIRST EXPOSED CAPTAIN AND NOT A DRAW, because the
        // roll above has already decided that something happened and a second
        // random choice would only make the same event harder to reproduce in a
        // test. Spawn order, which is `choosePrey`'s tie-break for the same
        // reason.
        const std::size_t victim = exposed.front();
        // Through the body when there is one, so a raid in a held bubble leaves
        // the same wreck a raid in the player's own does. `killCaptainPuppet`
        // walks the ordinary death path; the direct call is the fallback for
        // the tick before a body has been spawned.
        if (!killCaptainPuppet(victim)) {
            killCaptain(victim, bubble.system);
        }
        return; // one loss per tick: the roll is per system, the consequence is not
    }
}

std::uint32_t
SpaceWorld::nearestPlayerEnemy(const SystemBubble& bubble, const core::DVec3& from, double reach) const
{
    // ⚑⚑⚑⚑ HOSTILITY DERIVED FROM THE PLAYER'S STANDING, WHICH IS THE WHOLE
    // POINT OF THE STAGE POINTED THE OTHER WAY (stage D). Every other hostility
    // question in this game asks "is A hostile to B" of two faction rows.
    // A captain has no row - that is the phase spec's diagnosis - so the
    // question a hull of the player's has to ask is "is this hostile to the
    // person who hired me", and `playerHostile` has always been able to answer
    // it. The same bit `pilotHuntTrader` now reads about a captain, read back.
    //
    // ⚑ AND IT SKIPS OTHER HULLS OF THE PLAYER'S, which is not defensive: two
    // captains in one system is reachable today (a miner and the patrol posted
    // to cover them), and without this the patrol's first act would be to open
    // fire on the ship it was hired to protect.
    const ecs::Registry& registry = bubble.registry;
    const ecs::Pool<ShipPilot>& pilots = registry.storage<ShipPilot>();
    std::uint32_t best = kNoIndex;
    double bestDistance = 0.0;
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        const std::uint32_t index = pilots.entityIndices()[i];
        if (playerOwnedHull(registry, index) || isPlayerEntity(registry, index)) {
            continue;
        }
        const bool hostile =
            pilot.factionIndex >= m_factionTable.size() || m_factionSim.playerHostile(pilot.factionIndex);
        if (!hostile) {
            continue;
        }
        const Transform* transform = registry.storage<Transform>().tryGet(index);
        const ShipDefense* defense = registry.storage<ShipDefense>().tryGet(index);
        if (transform == nullptr || defense == nullptr || !defense->state.alive()) {
            continue;
        }
        const double distance = length(transform->position - from);
        if (distance > reach) {
            continue;
        }
        // Ties to the earlier entity, which is spawn order - `choosePrey`'s own
        // rule, so two patrols handed the same sky pick the same target.
        if (best == kNoIndex || distance < bestDistance) {
            bestDistance = distance;
            best = index;
        }
    }
    return best;
}

bool SpaceWorld::standCaptainDownAt(
    SystemBubble& bubble, ecs::Entity entity, std::size_t captainIndex, std::uint32_t station, double dt)
{
    // ⚑⚑ ONE PLACE THAT ENDS A COMBAT ORDER, because a patrol and an escort end
    // it identically and the half that is easy to get wrong is the same for
    // both: the hull has to be back on a PAD. `OwnedShip{storedSystem,
    // storedStation}` is the field every other screen in this game reads to say
    // where a ship of yours is, so a captain who stood down anywhere else is a
    // hull the player cannot find, sell, board or hand back.
    Captain& captain = m_captains[captainIndex];
    ecs::Registry& registry = bubble.registry;
    Transform* transform = registry.tryGet<Transform>(entity);
    const sim::SystemSpec& spec = m_galaxy.systems[bubble.system];
    if (transform == nullptr || station >= spec.stations.size()) {
        return false;
    }
    const core::DVec3 pad = spec.stations[station].position;
    if (CaptainPuppet* puppet = registry.tryGet<CaptainPuppet>(entity); puppet != nullptr) {
        puppet->destination = pad;
    }
    if (ShipPilot* pilot = registry.tryGet<ShipPilot>(entity); pilot != nullptr) {
        pilot->waypoint = pad;
        pilot->hasTarget = 0;
        if (pilot->state != PilotState::Travel) {
            pilot->state = PilotState::Travel;
        }
    }
    (void)cruiseCaptainToward(registry, entity, pad, kCaptainDeliverRange * 0.5, dt);
    if (length(transform->position - pad) > kCaptainDeliverRange) {
        return false; // still on the way in
    }
    if (captain.ship < m_fleet.size()) {
        OwnedShip& hull = m_fleet[captain.ship];
        hull.storedSystem = bubble.system;
        hull.storedStation = station;
    }
    captain.order = {};
    captain.mine = {};
    SOL_LOG_INFO("%s stands down at %s", captain.name.c_str(), spec.stations[station].name.c_str());
    return true;
}

std::uint32_t SpaceWorld::nearestStationTo(std::uint32_t system, const core::DVec3& from) const
{
    if (system >= m_galaxy.systems.size()) {
        return kNoIndex;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[system];
    std::uint32_t best = kNoIndex;
    double bestDistance = 0.0;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(spec.stations.size()); ++i) {
        const double distance = length(spec.stations[i].position - from);
        if (best == kNoIndex || distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

void SpaceWorld::tickPatrolBeat(
    SystemBubble& bubble, ecs::Entity entity, CaptainPuppet& puppet, const Captain& captain, double dt)
{
    ecs::Registry& registry = bubble.registry;
    ShipPilot* pilot = registry.tryGet<ShipPilot>(entity);
    Transform* transform = registry.tryGet<Transform>(entity);
    if (pilot == nullptr || transform == nullptr || captain.order.marketA >= m_economy.markets().size()) {
        return;
    }

    // Fleeing is Lua's call and it stands: a hull down to its last hull points
    // running for the dock is a decision, and re-issuing Attack over it every
    // tick would pin it in the fight until it died. `syncCaptainPuppets` makes
    // the same concession about a captain's route for the same reason.
    if (pilot->state == PilotState::Flee) {
        return;
    }

    // COMING IN TO STAND DOWN BEATS EVERYTHING ELSE, including a fight: the
    // player has called them off, and a patrol that stopped to shoot on the way
    // home is one that never gets there while the system stays busy.
    if (captain.order.stopping) {
        const sim::StationMarket& home = m_economy.markets()[captain.order.marketA];
        if (standCaptainDownAt(bubble, entity, puppet.captainIndex, home.stationIndex, dt)) {
            despawnShip(bubble, entity.index);
        }
        return;
    }

    // ⚑⚑⚑ THE DECISION IS MADE IN C++ AND IN EVERY BUBBLE, WHICH IS STAGE C's
    // PRECEDENT RATHER THAN A HOLE IN PHASE 38's. That phase scoped
    // `pilot_think` to the player's bubble so Lua could not reach across
    // registries, and recorded the cost in its own words: "nothing re-targets,
    // breaks off or picks a new beat until the player is back to watch it". A
    // captain is the exception the ruling already carved, because
    // `tickStationaryCaptains` has picked rocks in bubbles the player is not in
    // since stage C - it is C++, it takes the bubble as an argument, and it
    // touches only entities the player owns and what they can see.
    //
    // What that buys is the half of the exit the player WATCHES: post a patrol,
    // fly out, get jumped, and the hull you paid for comes over. The half they
    // do NOT watch is priced coarsely instead - see `rollHeldBubbleHazard` -
    // because a fight nobody is looking at should cost a die roll rather than a
    // frame of collision resolution.
    const std::uint32_t enemy =
        nearestPlayerEnemy(bubble, transform->position, sim::preyReach(m_galaxyParams.gateDistance));
    if (enemy != kNoIndex) {
        pilot->state = PilotState::Attack;
        pilot->targetIndex = enemy;
        pilot->hasTarget = 1;
        if (ShipPower* power = registry.tryGet<ShipPower>(entity)) {
            power->state.pips = pipsForPilot(pilot->state);
        }
        // ⚑⚑⚑⚑ AND IT HAS TO GET THERE, WHICH THE FIRST CUT DID NOT AND A LIVE
        // FLIGHT IS WHAT FOUND IT. `preyReach` is 2 x gateDistance - hundreds of
        // thousands of kilometres - and `PilotState::Attack` steering is
        // COMBAT-scale, which `PilotState`'s own comment says out loud: "Patrol's
        // steering is combat-scale - it closes to 50 m and stops - and a trade leg
        // is hundreds of thousands of kilometres, which is a distance only the
        // cruise envelope crosses in a sane time." So the first version spotted a
        // raider across the system, locked on, and closed 22 km in seventy
        // seconds. Measured in flight: the guard the player had paid for watched
        // from 47,000 km while their own hull burned.
        //
        // ⚑⚑⚑ THE REACH IS RIGHT AND THE APPROACH WAS MISSING, which is the
        // distinction that took a drive to see. `preyReach` documents its own
        // bound as the LOD bubble's diameter, saying no "only to a lock that
        // geometry already made impossible" - correct for a HUNTER, which
        // re-decides at 2 Hz and whose prey pops back onto a schedule, so "a long
        // chase costs it nothing". A guard is the opposite case: the chase IS the
        // job, and a lock it cannot act on is worse than no lock, because every
        // screen reports it as the order working. Third time this phase that a
        // rule borrowed from a neighbour arrived carrying its endpoints'
        // assumptions.
        if (const Transform* prey = registry.tryGet<Transform>(registry.entityFromIndex(enemy));
            prey != nullptr) {
            (void)cruiseCaptainToward(registry, entity, prey->position, kCaptainEngageRange, dt);
        }
        return; // inside the handover the steering flies the fight
    }
    // Nothing to shoot: back on the beat rather than left pointing at wherever
    // the last kill happened. `pilotHuntTrader`'s own rule when its prey is
    // gone, and it exists for the same reason - a hull that keeps its Attack
    // state here flies at a ghost for as long as the system stays quiet.
    if (pilot->state == PilotState::Attack) {
        pilot->state = PilotState::Travel;
        pilot->hasTarget = 0;
    }

    // ⚑⚑ THE BEAT IS THE DOCK AND THE GATES, AND THE GATES ARE THE POINT.
    // Anything hostile that is not already here arrives through one, so a beat
    // that walks them is the difference between a guard and an ornament. The
    // dock is in the loop because it is what the player was standing on when
    // they gave the order, and a patrol that never comes home reads as lost.
    const sim::SystemSpec& spec = m_galaxy.systems[bubble.system];
    const sim::StationMarket& row = m_economy.markets()[captain.order.marketA];
    const std::uint32_t stops = 1u + static_cast<std::uint32_t>(spec.gates.size());
    if (puppet.beat >= stops) {
        puppet.beat = 0;
    }
    const core::DVec3 stop =
        puppet.beat == 0 ? spec.stations[row.stationIndex].position : spec.gates[puppet.beat - 1].position;
    puppet.destination = stop;
    pilot->waypoint = stop;
    if (pilot->state == PilotState::Idle || pilot->state == PilotState::Patrol) {
        pilot->state = PilotState::Travel;
    }
    (void)cruiseCaptainToward(registry, entity, stop, kCaptainDeliverRange, dt);
    if (length(transform->position - stop) <= kCaptainCruiseInside) {
        puppet.beat = (puppet.beat + 1u) % stops;
    }
}

bool SpaceWorld::cruiseCaptainToward(
    ecs::Registry& registry, ecs::Entity entity, const core::DVec3& waypoint, double arrival, double dt) const
{
    Transform* transform = registry.tryGet<Transform>(entity);
    FlightBody* body = registry.tryGet<FlightBody>(entity);
    if (transform == nullptr || body == nullptr) {
        return false;
    }
    const core::DVec3 lane = waypoint - transform->position;
    const double distance = length(lane);
    if (distance <= arrival + kCaptainCruiseInside) {
        return false; // inside a field's own scale; the pilot flies it
    }
    const double step = std::min(kCaptainCruiseSpeed * dt, distance - arrival);
    const core::DVec3 point = transform->position + lane * (step / distance);
    transform->position = point;
    // Both ends of the tick, or the collision sweep reads the pace as a
    // hypersonic charge through everything between the two points -
    // `keepTraderOnSchedule`'s own hazard, and it is the same one here.
    transform->previousPosition = point;
    transform->orientation = lookAlong(lane);
    transform->previousOrientation = transform->orientation;
    // Handed over already moving, so the release is a continuation rather than
    // a freighter stalling at the edge of the window.
    const ShipControl* control = registry.tryGet<ShipControl>(entity);
    const double envelope = control != nullptr ? static_cast<double>(control->tuning.maxSpeed) : 200.0;
    body->velocity = lane * (envelope / distance);
    return true;
}

void SpaceWorld::openStationaryCaptainBubbles()
{
    // ⚑⚑⚑ THE ORDER OPENS THE SYSTEM, AND IT DOES IT OVER THE CAP (the user's
    // ruling 11). The spec named exactly what happens without this line: past
    // six systems `instantiateSystem` returns false and a captain's ship "is
    // not destroyed and not refused - it is never simulated, silently". A hull
    // the player paid for, flown by somebody they hired, is not a thing to drop
    // on the floor because a cooling fight two systems away got there first.
    for (std::size_t i = 0; i < m_captains.size(); ++i) {
        if (!stationary(m_captains[i].order.kind)) {
            continue;
        }
        const std::uint32_t system = captainSystem(i);
        if (system == kNoIndex || systemIsInstantiated(system)) {
            continue;
        }
        if (!instantiateSystem(system, true)) {
            // Only `kMaxBubbles` can refuse now, and that one is the save
            // format's ceiling rather than a policy - reaching it means
            // something else has gone very wrong, so it is said rather than
            // skipped.
            SOL_LOG_WARN("captain '%s' is posted to a system that could not be opened",
                         m_captains[i].name.c_str());
        }
    }
}

void SpaceWorld::captainPuppetInfo(std::vector<CaptainPuppetInfo>& out)
{
    out.clear();
    ecs::Registry& registry = playerRegistry();
    ecs::Pool<CaptainPuppet>& puppets = registry.storage<CaptainPuppet>();
    const core::DVec3 here = shipState().position;
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        const CaptainPuppet& puppet = puppets.values()[i];
        const std::uint32_t entityIndex = puppets.entityIndices()[i];
        if (puppet.captainIndex >= m_captains.size()) {
            continue;
        }
        const Captain& captain = m_captains[puppet.captainIndex];
        const Transform& transform = registry.storage<Transform>().get(entityIndex);
        const FlightBody& body = registry.storage<FlightBody>().get(entityIndex);
        CaptainPuppetInfo info;
        info.captainIndex = puppet.captainIndex;
        info.name = captain.name;
        info.ship = captain.ship < m_fleet.size() ? m_fleet[captain.ship].defId : std::string();
        info.distance = length(transform.position - here);
        info.speed = length(body.velocity);
        info.paced = puppet.paced != 0;
        info.beat = puppet.beat;
        info.entity = entityIndex;
        out.push_back(std::move(info));
    }
}

bool SpaceWorld::chooseMinerRock(const ecs::Registry& registry,
                                 MinerPuppet& miner,
                                 const core::DVec3& from,
                                 bool sameField) const
{
    // 8f's rocks are real entities with real depletion, so the miner works one
    // of those rather than a point in a field: a rock that has been cut to
    // nothing is not spawned, which means a miner can never be found working
    // ground that is already gone.
    const ecs::Pool<MineableRock>& rocks = registry.storage<MineableRock>();
    m_minerRocks.clear();
    m_minerRockEntities.clear();
    std::uint32_t nearest = kNoIndex;
    std::uint32_t nearestField = 0;
    double nearestDistance = 0.0;
    std::uint32_t leaving = sim::kNoRock;
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const MineableRock& rock = rocks.values()[i];
        if (rock.commodity != miner.commodity) {
            continue; // an outpost sells one thing; it works the rock holding it
        }
        const std::uint32_t index = rocks.entityIndices()[i];
        const Transform* transform = registry.storage<Transform>().tryGet(index);
        const RenderShape* shape = registry.storage<RenderShape>().tryGet(index);
        if (transform == nullptr || shape == nullptr) {
            continue;
        }
        if (rock.field == miner.field) {
            if (index == miner.rock) {
                leaving = static_cast<std::uint32_t>(m_minerRocks.size());
            }
            m_minerRocks.push_back(
                {.position = transform->position, .radius = static_cast<double>(shape->scale.x)});
            m_minerRockEntities.push_back(index);
        }
        const double distance = length(transform->position - from);
        if (nearest == kNoIndex || distance < nearestDistance) {
            nearest = index;
            nearestField = rock.field;
            nearestDistance = distance;
        }
    }
    if (sameField && !m_minerRocks.empty()) {
        // Every hop is short and every hop is clear, because a field is full
        // of solid rock and nothing steers around it.
        const std::uint32_t next = sim::chooseWorkRock(
            from, leaving, std::span<const sim::MiningRock>(m_minerRocks), kMinerPathClearance);
        if (next != sim::kNoRock) {
            miner.rock = m_minerRockEntities[next];
            ++miner.rockStep;
            return true;
        }
        // Boxed in: keep working the one it has rather than fly a path that
        // ends in a rock. Answering true is the point — the miner is fine,
        // there is simply nowhere better to be.
        return miner.rock != kNoIndex;
    }
    if (nearest == kNoIndex) {
        return false; // nothing of this commodity in the sky: no body to draw
    }
    miner.field = nearestField;
    miner.rock = nearest;
    ++miner.rockStep;
    return true;
}

bool SpaceWorld::minerWorkPoint(const ecs::Registry& registry,
                                const MinerPuppet& miner,
                                core::DVec3& rock,
                                core::DVec3& hold) const
{
    const Transform* transform = registry.storage<Transform>().tryGet(miner.rock);
    if (transform == nullptr || registry.storage<MineableRock>().tryGet(miner.rock) == nullptr) {
        return false; // cut to nothing, or the system changed under it
    }
    rock = transform->position;
    const RenderShape* shape = registry.storage<RenderShape>().tryGet(miner.rock);
    const double radius = shape != nullptr ? static_cast<double>(shape->scale.x) : 0.0;
    // On the station's side of the rock, so a ship coming out from the dock
    // meets the miner rather than the rock it is hiding behind.
    const sim::StationMarket& market = m_economy.markets()[miner.market];
    const core::DVec3 station = m_galaxy.systems[market.systemIndex].stations[market.stationIndex].position;
    hold = sim::minerHoldPoint(rock, radius, station - rock, kMinerRockClearance);
    return true;
}

void SpaceWorld::syncMinerPuppets(SystemBubble& bubble, double dt)
{
    if (m_defs == nullptr || m_factionTable.empty() || m_economy.markets().empty()) {
        return;
    }
    ecs::Registry& registry = bubble.registry;
    const std::size_t marketCount = m_economy.markets().size();

    // Which outposts here are actually digging. Not "which are extractors":
    // satisfaction is the station's own answer to how much of its nominal
    // output it managed, so a mine whose warehouse is full, whose rock has run
    // out, or whose miner the player just shot reads zero and gets no body.
    // That is the promotion working in the honest direction — the sky follows
    // the books, and a still field means the books have stopped.
    const auto digs = [&](std::uint32_t market) {
        const sim::StationMarket& row = m_economy.markets()[market];
        if (row.systemIndex != bubble.system || row.archetype >= m_economyParams.archetypes.size()) {
            return false;
        }
        return m_economyParams.archetypes[row.archetype].extracts && m_economy.satisfaction(market) > 0.0f &&
               m_minerHold[market] <= 0.0;
    };

    std::vector<ecs::Entity> doomed;
    ecs::Pool<MinerPuppet>& miners = registry.storage<MinerPuppet>();
    for (std::size_t i = 0; i < miners.size(); ++i) {
        MinerPuppet& miner = miners.values()[i];
        const ecs::Entity entity = registry.entityFromIndex(miners.entityIndices()[i]);
        if (miner.market >= marketCount || !digs(miner.market) || m_minerPresent[miner.market] != 0) {
            doomed.push_back(entity);
            continue;
        }
        m_minerPresent[miner.market] = 1;
        ShipPilot* pilot = registry.tryGet<ShipPilot>(entity);
        Transform* transform = registry.tryGet<Transform>(entity);
        if (pilot == nullptr || transform == nullptr) {
            continue;
        }
        // The job is not Lua's to choose, exactly as a hauler's route is not
        // (stage 2): Idle and Patrol go back on the rock, while Attack and
        // Flee are left alone — being shot at is the one thing that should
        // stop a miner working. And it stays stopped for as long as the threat
        // is warm, or the tick after Lua stopped flying the fight would send
        // the ship straight back to the rock it was being shot off.
        if (pilot->threatTimer <= 0.0f &&
            (pilot->state == PilotState::Idle || pilot->state == PilotState::Patrol)) {
            pilot->state = PilotState::Travel;
        }
        if (pilot->state != PilotState::Travel) {
            continue;
        }
        miner.rockSeconds -= static_cast<float>(dt);
        core::DVec3 rock;
        core::DVec3 hold;
        if (miner.rockSeconds <= 0.0f || !minerWorkPoint(registry, miner, rock, hold)) {
            if (!chooseMinerRock(registry, miner, transform->position, true)) {
                doomed.push_back(entity); // the field is worked out under it
                continue;
            }
            miner.rockSeconds = static_cast<float>(kMinerRockSeconds);
            if (!minerWorkPoint(registry, miner, rock, hold)) {
                doomed.push_back(entity);
                continue;
            }
        }
        pilot->waypoint = hold;
    }
    for (const ecs::Entity entity : doomed) {
        despawnShip(bubble, entity.index);
    }

    for (std::uint32_t market = 0; market < marketCount; ++market) {
        if (m_minerPresent[market] != 0 || !digs(market)) {
            continue;
        }
        const sim::StationMarket& row = m_economy.markets()[market];
        const sim::EconomyArchetype& archetype = m_economyParams.archetypes[row.archetype];
        std::uint32_t commodity = kNoIndex;
        for (std::uint32_t c = 0; c < archetype.production.size(); ++c) {
            if (archetype.production[c] > 0.0f) {
                commodity = c;
                break;
            }
        }
        if (commodity == kNoIndex) {
            continue;
        }
        const core::DVec3 station = m_galaxy.systems[row.systemIndex].stations[row.stationIndex].position;
        MinerPuppet miner{.market = market, .commodity = commodity};
        if (!chooseMinerRock(registry, miner, station, false)) {
            continue; // no rock of that kind here; the draw is failing anyway
        }
        core::DVec3 rock;
        core::DVec3 hold;
        if (!minerWorkPoint(registry, miner, rock, hold)) {
            continue;
        }
        miner.rockSeconds = static_cast<float>(kMinerRockSeconds);

        // Whose ship it is: the ground it works, which is the same rule a
        // hauler's allegiance follows. Never unaffiliated — Lua reads that as
        // unconditionally player-hostile, and a mining ship opening fire on
        // sight is exactly what this must not be.
        std::uint32_t faction = systemOwnerFaction(row.systemIndex);
        if (faction >= m_factionTable.size()) {
            faction = 0;
        }
        const GameFaction& owner = m_factionTable[faction];
        const std::span<const std::string> roster =
            factionRoster(owner, assets::RosterCell::Trader, assets::RosterCell::Patrol);
        if (roster.empty()) {
            continue;
        }
        // ⚑ The biggest hull the faction hauls with, and that falls out of the
        // same rule as a freighter rather than needing a roster of its own: a
        // ship that works a rock all day is carrying as much as anything in
        // the sky. Asking chooseTraderHull for more than any hull holds is how
        // "the biggest there is" is spelled.
        m_rosterCapacities.clear();
        for (const std::string& id : roster) {
            const assets::ShipDef* candidate = m_defs->findShip(id.c_str());
            m_rosterCapacities.push_back(candidate != nullptr ? candidate->cargoCapacity : 0.0f);
        }
        const std::uint32_t hull = sim::chooseTraderHull(
            std::span<const float>(m_rosterCapacities), std::numeric_limits<float>::max(), market);
        const assets::ShipDef* def = hull < roster.size() ? m_defs->findShip(roster[hull].c_str()) : nullptr;
        if (def == nullptr) {
            SOL_LOG_WARN("miner puppet: no ship def for market %u", market);
            continue;
        }
        // Placed where it works rather than at the station: an outpost's draw
        // has been running since the galaxy was made, so the player arriving
        // must find the field already being worked, not a ship setting out.
        const ecs::Entity entity = spawnShipAt(bubble, *def, *m_defs, hold, owner.name.c_str());
        registry.emplace<ShipPilot>(entity,
                                    ShipPilot{.role = PilotRole::Trader,
                                              .state = PilotState::Travel,
                                              .waypoint = hold,
                                              .factionIndex = faction});
        registry.emplace<MinerPuppet>(entity, miner);
        Transform& transform = registry.storage<Transform>().get(entity.index);
        transform.orientation = lookAlong(rock - hold); // nose on the rock it is cutting
        transform.previousOrientation = transform.orientation;
        m_minerPresent[market] = 1;
    }
}

void SpaceWorld::minerPuppetInfo(std::vector<MinerPuppetInfo>& out)
{
    out.clear();
    const core::DVec3 eye = playerRegistry().storage<Transform>().get(playerEntityIndex()).position;
    ecs::Pool<MinerPuppet>& miners = playerRegistry().storage<MinerPuppet>();
    for (std::size_t i = 0; i < miners.size(); ++i) {
        const std::uint32_t entityIndex = miners.entityIndices()[i];
        const Transform* transform = playerRegistry().storage<Transform>().tryGet(entityIndex);
        if (transform == nullptr) {
            continue;
        }
        const MinerPuppet& miner = miners.values()[i];
        MinerPuppetInfo info;
        info.market = miner.market;
        info.distance = length(transform->position - eye);
        const FlightBody* body = playerRegistry().storage<FlightBody>().tryGet(entityIndex);
        info.speed = body != nullptr ? length(body->velocity) : 0.0;
        if (const Transform* rock = playerRegistry().storage<Transform>().tryGet(miner.rock)) {
            info.working = true;
            info.rockDistance = length(rock->position - transform->position);
        }
        if (miner.market < m_economy.markets().size()) {
            const sim::StationMarket& row = m_economy.markets()[miner.market];
            info.station = m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name;
        }
        for (const SpawnedShip& spawned : playerShips()) {
            if (spawned.entity.index == entityIndex) {
                info.name = spawned.name;
                break;
            }
        }
        out.push_back(std::move(info));
    }
}

core::DVec3 SpaceWorld::traderScheduledPoint(const TraderLegPlacement& leg) const
{
    const core::DVec3 lane = leg.to - leg.from;
    const double legLength = length(lane);
    const double remaining = (1.0 - static_cast<double>(leg.progress)) * leg.legSeconds;
    const double distance = sim::scheduledLaneDistance(
        remaining, leg.legSeconds, legLength, kTraderApproachDistance, kTraderApproachSeconds);
    if (legLength <= 0.0) {
        return leg.to;
    }
    return leg.to - lane * (distance / legLength);
}

// Holds a puppet to the record's pace through the middle of its leg, and lets
// go at both ends.
//
// The ship cannot fly the middle: a freighter needs about 260 s for a leg the
// economy quotes at 90, so flying it freely means never arriving anywhere.
// Through the middle the record therefore owns the position — invisibly, at
// tens of thousands of km and speeds where a hull is far below a pixel — and
// inside the approach window the ship owns it, which is the part with
// anything to look at. That handover IS the Simulation-LOD promotion: coarse
// where nobody is watching, full fidelity where they are.
bool SpaceWorld::keepTraderOnSchedule(ecs::Registry& registry,
                                      ecs::Entity entity,
                                      const TraderLegPlacement& leg)
{
    ShipPilot* pilot = registry.tryGet<ShipPilot>(entity);
    if (pilot != nullptr && pilot->state != PilotState::Travel) {
        return false; // fighting or running: its own business, and Lua's
    }
    // ⚑ And nor is a hauler someone is shooting at, whatever state it is in.
    // A drive watched a raider close to 2 km, open fire, and then find its
    // prey 13,000 km away: the hauler had stopped fleeing for one think, gone
    // back on its lane, and the record — which moves it faster than any hull
    // flies — carried it out of the fight. The schedule owns a hauler's
    // position only while nothing is happening to it. Six seconds of quiet
    // (kThreatMemorySeconds) is what "the fight is over" means here.
    if (pilot != nullptr && pilot->threatTimer > 0.0f) {
        return false;
    }
    const double elapsed = static_cast<double>(leg.progress) * leg.legSeconds;
    const double remaining = leg.legSeconds - elapsed;
    const double window = std::min(kTraderApproachSeconds, leg.legSeconds * 0.4);
    if (elapsed <= window || remaining <= window) {
        return false; // near an endpoint: it flies itself
    }
    Transform* transform = registry.tryGet<Transform>(entity);
    FlightBody* body = registry.tryGet<FlightBody>(entity);
    if (transform == nullptr || body == nullptr) {
        return false;
    }
    const core::DVec3 point = traderScheduledPoint(leg);
    transform->position = point;
    // Both ends of the tick, or the collision sweep reads the schedule's jump
    // as a hypersonic charge through everything between the two points.
    transform->previousPosition = point;
    const core::DVec3 lane = leg.to - point;
    const double distance = length(lane);
    if (distance > 1.0) {
        transform->orientation = lookAlong(lane);
        transform->previousOrientation = transform->orientation;
        // Handed over already moving at the speed steerTravel's own profile
        // wants at the approach distance, so the release is a continuation
        // rather than a hauler stalling at the edge of the window.
        const ShipControl* control = registry.tryGet<ShipControl>(entity);
        const double envelope =
            control != nullptr ? static_cast<double>(control->tuning.maxSpeed) * 2.0 : 200.0;
        body->velocity = lane * (envelope / distance);
    }
    return true;
}

void SpaceWorld::traderPuppetInfo(std::vector<TraderPuppetInfo>& out)
{
    static constexpr const char* kStateNames[] = {"idle", "patrol", "attack", "flee", "travel", "inspect"};
    out.clear();
    const core::DVec3 eye = playerRegistry().storage<Transform>().get(playerEntityIndex()).position;
    ecs::Pool<TraderPuppet>& puppets = playerRegistry().storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        const std::uint32_t entityIndex = puppets.entityIndices()[i];
        const Transform* transform = playerRegistry().storage<Transform>().tryGet(entityIndex);
        if (transform == nullptr) {
            continue;
        }
        TraderPuppetInfo info;
        info.traderIndex = puppets.values()[i].traderIndex;
        info.distance = length(transform->position - eye);
        const FlightBody* body = playerRegistry().storage<FlightBody>().tryGet(entityIndex);
        info.speed = body != nullptr ? length(body->velocity) : 0.0;
        const ShipPilot* pilot = playerRegistry().storage<ShipPilot>().tryGet(entityIndex);
        info.state = pilot != nullptr
                         ? kStateNames[static_cast<std::uint32_t>(pilot->state) % std::size(kStateNames)]
                         : "none";
        for (const SpawnedShip& spawned : playerShips()) {
            if (spawned.entity.index == entityIndex) {
                info.name = spawned.name;
                break;
            }
        }
        out.push_back(std::move(info));
    }
}

void SpaceWorld::patrolPosts(std::vector<PatrolPost>& out) const
{
    out.clear();
    if (m_currentSystem >= m_galaxy.systems.size()) {
        return;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const ecs::Pool<ShipPilot>& pilots = playerRegistry().storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        if (pilot.role != PilotRole::Patrol) {
            continue;
        }
        const std::uint32_t index = pilots.entityIndices()[i];
        const Transform* transform = playerRegistry().storage<Transform>().tryGet(index);
        if (transform == nullptr) {
            continue;
        }
        PatrolPost info;
        info.pilotIndex = index;
        info.position = transform->position;
        info.state = pilot.state;
        info.distanceToPlayer = length(
            transform->position - playerRegistry().storage<Transform>().get(playerEntityIndex()).position);
        info.post = nearestPost(transform->position);
        info.distanceToPost = length(info.post - transform->position);
        info.waypointDistanceToPost = length(info.post - pilot.waypoint);
        info.factionIndex = pilot.factionIndex;
        // Which KIND of post it is. Compared against the gate list rather than
        // stored on the pilot, so this stays a fact about where the ship is
        // rather than a label that could drift away from it.
        for (const sim::GateSpec& gate : spec.gates) {
            if (length(gate.position - info.post) < 1.0) {
                info.atGate = true;
                break;
            }
        }
        out.push_back(info);
    }
}

void SpaceWorld::responderInfo(std::vector<ResponderInfo>& out) const
{
    out.clear();
    const ecs::Pool<ShipPilot>& pilots = playerRegistry().storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        if (pilot.respondTimer <= 0.0f) {
            continue;
        }
        const std::uint32_t index = pilots.entityIndices()[i];
        const Transform* transform = playerRegistry().storage<Transform>().tryGet(index);
        if (transform == nullptr) {
            continue;
        }
        ResponderInfo info;
        info.name = playerRegistry().storage<ShipPilot>().tryGet(index) != nullptr &&
                            pilot.factionIndex < m_factionTable.size()
                        ? m_factionTable[pilot.factionIndex].name
                        : std::string("unaffiliated");
        info.distanceToIncident = length(pilot.waypoint - transform->position);
        info.secondsLeft = static_cast<double>(pilot.respondTimer);
        info.state = pilot.state;
        info.position = transform->position;
        info.pirate =
            pilot.factionIndex < m_factionTable.size() && m_factionTable[pilot.factionIndex].pirate();
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const ResponderInfo& a, const ResponderInfo& b) {
        return a.distanceToIncident < b.distanceToIncident;
    });
}

PilotState SpaceWorld::pilotStateOf(ecs::Entity entity) const
{
    const ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    return pilot != nullptr ? pilot->state : PilotState::Idle;
}

bool SpaceWorld::enterSystem(std::uint32_t systemIndex)
{
    if (systemIndex >= m_galaxy.systems.size()) {
        return false;
    }
    m_jump.clear();
    loadSystem(systemIndex, kNoIndex);
    return true;
}

void SpaceWorld::hunterInfo(std::vector<HunterInfo>& out)
{
    static constexpr const char* kStateNames[] = {"idle", "patrol", "attack", "flee", "travel", "inspect"};
    out.clear();
    const ecs::Pool<ShipPilot>& pilots = playerRegistry().storage<ShipPilot>();
    const ecs::Pool<TraderPuppet>& puppets = playerRegistry().storage<TraderPuppet>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        // ⚑ Every fighter, not only the ones that found prey. A probe that
        // listed hunts alone answered "0 hunts" while a raider was visibly
        // crossing the system at cruise speed, which told me nothing about
        // which half had failed - picking prey, or reporting it. What a hunt
        // fails at is worth as much as that it happened.
        if (pilot.role != PilotRole::Fighter) {
            continue;
        }
        const std::uint32_t entityIndex = pilots.entityIndices()[i];
        const Transform* from = playerRegistry().storage<Transform>().tryGet(entityIndex);
        if (from == nullptr) {
            continue;
        }
        HunterInfo info;
        info.state = kStateNames[static_cast<std::uint32_t>(pilot.state) % std::size(kStateNames)];
        const TraderPuppet* prey = pilot.hasTarget != 0 ? puppets.tryGet(pilot.targetIndex) : nullptr;
        if (prey != nullptr) {
            info.traderIndex = prey->traderIndex;
            info.hunting = true;
        }
        const Transform* to =
            pilot.hasTarget != 0 ? playerRegistry().storage<Transform>().tryGet(pilot.targetIndex) : nullptr;
        info.distance = to != nullptr ? length(to->position - from->position) : 0.0;
        for (const SpawnedShip& spawned : playerShips()) {
            if (spawned.entity.index == entityIndex) {
                info.name = spawned.name;
            } else if (pilot.hasTarget != 0 && spawned.entity.index == pilot.targetIndex) {
                info.prey = spawned.name;
            }
        }
        if (info.prey.empty() && pilot.hasTarget != 0 &&
            isPlayerEntity(playerRegistry(), pilot.targetIndex)) {
            info.prey = "the player";
        }
        out.push_back(std::move(info));
    }
}

bool SpaceWorld::bubbleHoldsLiveFight(const SystemBubble& bubble)
{
    // A bolt in the air is a fight on its own: it has a shooter, a victim it
    // is closing on, and it will land whether or not anybody is still steering
    // toward anybody.
    if (!bubble.registry.storage<Projectile>().empty()) {
        return true;
    }
    const ecs::Pool<ShipPilot>& pilots = bubble.registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        // ⚑ ANY fight, not only one the player was in - the user's ruling, and
        // the spec's own wording. A raider running down a hauler two jumps
        // away is a fight, and a system that stops mid-way through one so that
        // the same hauler can be freshly alive next time you look is the exact
        // reset this phase exists to remove. It also means the cap gets
        // exercised in ordinary play rather than only in a fighting retreat.
        if (pilot.state == PilotState::Attack || pilot.state == PilotState::Flee ||
            pilot.threatTimer > 0.0f) {
            return true;
        }
    }
    return false;
}

bool SpaceWorld::bubbleHoldsPlayerAsset(const SystemBubble& bubble) const
{
    // ⚑⚑⚑⚑ `decisions/015`'s SECOND SET, WHICH WAS PROVABLY EMPTY UNTIL THIS
    // STAGE. "The player's system, plus every system holding a player asset" -
    // and an owned ship you are not flying was an `OwnedShip{storedSystem,
    // storedStation}` row parked at a station, which is not in the sky at all.
    // A captain on a STATIONARY order is the first thing that puts one there.
    //
    // ⚑⚑⚑ ASKED OF THE RECORD AND NEVER OF THE REGISTRY, AND THAT IS THE ONE
    // THING THIS FUNCTION MUST GET RIGHT. The obvious implementation walks the
    // bubble's `CaptainPuppet` pool - and it would be a circle: the body exists
    // because the bubble is held, and the bubble would be held because the body
    // exists. One tick where the hull has not been spawned yet, or a tick after
    // it was despawned for a rebuild, and the bubble is released out from under
    // the captain standing in it. The ORDER is what holds a system open,
    // because the order is what the player gave and what the player can take
    // back.
    for (std::size_t i = 0; i < m_captains.size(); ++i) {
        if (!stationary(m_captains[i].order.kind)) {
            continue;
        }
        if (captainSystem(i) == bubble.system) {
            return true;
        }
    }
    return false;
}

double SpaceWorld::bubbleRetentionSeconds(const SystemBubble& bubble) const
{
    // ⚑⚑⚑⚑ PHASE 39'S CLAUSE, AND IT IS NOT THE ONE PHASE 38 DRAFTED FOR IT.
    // The line left here read
    //
    //     if (bubbleHoldsPlayerAsset(bubble)) { return kHeldIndefinitely; }
    //
    // with a comment arguing that this is "why this returns SECONDS rather than
    // a bool: a parked asset is not held for two minutes, it is held for as
    // long as it is parked, and a predicate has nowhere to put that
    // difference". The argument was right about the requirement and wrong about
    // where it goes, and there is no `kHeldIndefinitely` in this file as a
    // result. A sentinel large enough to mean "forever" is a number that
    // `enforceBubbleCap` then compares against real hold times, that gets
    // written into a save and read back, and that has to be recognised again
    // everywhere it is decremented. What "held for as long as it is parked"
    // actually needs is for the countdown to be RESTARTED while the asset is
    // there, and the only function that can do that is the one holding the
    // clock - `releaseCooledBubbles`, below.
    //
    // So this stays what its name says: the seconds a bubble is worth keeping
    // at the moment the player walks out of it. Both callers of it are asking
    // the same question and the answer to both is the same two minutes; what
    // differs is that one of them is renewed and the other is not.
    if (bubbleHoldsLiveFight(bubble) || bubbleHoldsPlayerAsset(bubble)) {
        return kCoolingSeconds;
    }
    return 0.0;
}

void SpaceWorld::forgetDepartedPlayer(SystemBubble& bubble, std::uint32_t departedIndex)
{
    ecs::Registry& registry = bubble.registry;
    ecs::Pool<ShipPilot>& pilots = registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        ShipPilot& pilot = pilots.values()[i];
        if (pilot.hasTarget != 0 && pilot.targetIndex == departedIndex) {
            pilot.hasTarget = 0;
            pilot.targetIndex = 0;
            // The three states that are ABOUT a target. `Patrol` and `Travel`
            // fly to a waypoint and do not care, and `Idle` is where the other
            // three were going to land anyway - the Attack case in `tickSystem`
            // drops to Idle the moment the target stops resolving, one tick
            // later. Doing it here rather than letting that happen is what
            // makes the difference between "the target is gone" and "the
            // target is now whoever took its slot".
            if (pilot.state == PilotState::Attack || pilot.state == PilotState::Flee ||
                pilot.state == PilotState::Inspect) {
                pilot.state = PilotState::Idle;
            }
        }
        // ⚑⚑ THE GRUDGE GOES WITH THE TARGET, AND THIS HALF IS STRUCTURE
        // RATHER THAN A FIX - SAID PLAINLY BECAUSE THE MUTATION TEST SAYS SO.
        // Deleting these three lines breaks nothing any guard can see, and the
        // reason is that all three readers of `threatIndex` resolve it against
        // `playerRegistry()` - `pilotEngageThreat` and the two think-pass
        // sites - so a stale one in a bubble the player is not in is never
        // dereferenced at all, and `threatTimer` ages to nothing in six
        // seconds either way. It is here so that "nothing left behind names
        // you" is true of the whole component set rather than true of two
        // fields out of three, which is the difference between a rule and a
        // coincidence. What actually matters to the player is the DAMAGE, and
        // that is deliberately untouched.
        if (pilot.threatIndex == departedIndex) {
            pilot.threatIndex = 0;
            pilot.threatTimer = 0.0f;
        }
    }
    // Bolts the player fired and left behind keep flying - they are part of the
    // frame and something is about to be hit by them - but they are ownerless
    // now. `kNoIndex` is the value the damage path already understands: it
    // skips the threat write, and `isPlayerEntity` is bounds-checked and
    // answers false, so a kill by an abandoned bolt is credited to nobody
    // rather than to whoever inherits the slot.
    ecs::Pool<Projectile>& projectiles = registry.storage<Projectile>();
    for (std::size_t i = 0; i < projectiles.size(); ++i) {
        Projectile& projectile = projectiles.values()[i];
        if (projectile.shooterIndex == departedIndex) {
            projectile.shooterIndex = kNoIndex;
        }
    }
}

void SpaceWorld::enforceBubbleCap()
{
    while (m_bubbles.size() > kMaxInstantiatedSystems) {
        // From slot 1: the player's bubble is the front one and is never a
        // candidate, whatever the cap says.
        //
        // ⚑⚑⚑⚑ AND NEITHER IS A CAPTAIN'S, WHICH IS THE STAGE C CHANGE AND THE
        // DATA-LOSS BUG THE PHASE'S RISK REGISTER NAMED IN ADVANCE. The comment
        // this function used to carry justified the drop like this: "Nothing
        // dangles either: every reference that outlives a bubble is coarse-layer
        // state keyed by SYSTEM (wrecks, rock depletion, trader routes,
        // standing)." That was exactly right when it was written and it is
        // exactly wrong about a bubble a captain is working in. An `OwnedShip`
        // row is not coarse-layer state keyed by a system - it is the player's
        // property, and the thing it names is an entity inside the registry
        // about to be freed. Where evicting an ambient bubble is a degradation
        // (the system is generated fresh next time), evicting this one is a
        // freighter the player paid sixty thousand credits for, and the ore in
        // its hold, ceasing to exist with no wreck, no death path and no line
        // anywhere that a person could find afterwards.
        std::size_t coldest = m_bubbles.size();
        for (std::size_t i = 1; i < m_bubbles.size(); ++i) {
            if (bubbleHoldsPlayerAsset(*m_bubbles[i])) {
                continue;
            }
            if (coldest == m_bubbles.size() || m_bubbles[i]->holdSeconds < m_bubbles[coldest]->holdSeconds) {
                coldest = i;
            }
        }
        // ⚑⚑⚑ SO THE CAP IS SOFT NOW, AND IT IS SOFT ON PURPOSE (the user's
        // ruling 11, taken before a line was written). When every retained
        // bubble is a captain's there is nothing this function is allowed to
        // choose, and the honest thing to do is stop rather than pick the least
        // bad victim. The alternatives were both worse: refusing the order at
        // the Crew tab makes a button's availability depend on how many fights
        // the player happens to have walked out of in the last two minutes, and
        // demoting the seventh captain to a coarse record builds a second
        // representation of mining for a case that needs seven simultaneous
        // captains to reach.
        //
        // What bounds it instead is the fleet: a held bubble costs a hull the
        // player had to buy and a captain they had to hire, so the ceiling is
        // what they can afford rather than a constant. `kMaxInstantiatedSystems`
        // keeps its full authority over the bubbles nobody paid for - a cooling
        // fight, a console `instantiate`, an ambient retention - which is every
        // bubble that can appear without the player deciding to make one.
        if (coldest == m_bubbles.size()) {
            break;
        }
        // ⚑⚑ DROPPING A BUBBLE RUNS NO DEATH PATH, AND THAT IS BOTH WHY IT IS
        // SAFE AND WHY IT IS LOSSY. Nothing is killed, so nothing is recorded:
        // no wreck, no kill credit, no standing change. The system simply
        // reverts to being generated fresh on the next arrival, which is the
        // pre-Phase-38 behaviour - a degradation, never a corruption. Nothing
        // dangles, and that clause is now TRUE BECAUSE OF THE SKIP ABOVE rather
        // than true of everything: of the bubbles this loop can still choose
        // from, every reference that outlives one is coarse-layer state keyed by
        // SYSTEM (wrecks, rock depletion, trader routes, standing), and the one
        // world-scoped list of entity indices, `m_collisionShipIndices`, is
        // per-tick scratch refilled inside the per-bubble loop.
        m_bubbles.erase(m_bubbles.begin() + static_cast<std::ptrdiff_t>(coldest));
    }
}

void SpaceWorld::releaseCooledBubbles(double dt)
{
    for (std::size_t i = m_bubbles.size(); i > 1; --i) {
        SystemBubble& bubble = *m_bubbles[i - 1];
        // ⚑⚑⚑⚑ THE DOOR `kCoolingSeconds` FORBIDS, OPENED FOR EXACTLY ONE
        // REASON, WHICH IS THE WHOLE OF WHAT STAGE C ADDS TO THIS FUNCTION.
        // That constant's own words: "Counted DOWN only, never refreshed - a
        // refresh is how 'a stalemate pins a bubble forever' gets back in
        // through the door the ceiling closes." Every word of that is still
        // true of the condition it was written about. What makes a refresh
        // dangerous is that a LIVE FIGHT is a condition the world can hold true
        // by accident and forever: two ships that cannot kill each other keep
        // `threatTimer` warm between them with nobody deciding anything, so a
        // per-tick re-ask would be the sim renewing its own lease.
        //
        // A standing order is the opposite kind of condition on every axis that
        // matters here. The PLAYER set it, the player can see it on the Crew
        // tab, and one button takes it away - so a bubble held this way is held
        // deliberately, by a decision that is visible in a screen and bounded
        // by how many hulls the player owns. That is why it is safe to renew
        // and why the fight case still is not, and it is the only reason this
        // branch exists.
        //
        // ⚑⚑ IT WRITES THE ORDINARY COOLING TIME RATHER THAN A SENTINEL, AND
        // THAT IS WHAT MAKES STANDING DOWN CORRECT FOR FREE. The tick after an
        // order is cancelled or a captain is recalled, this stops renewing and
        // the bubble is on `kCoolingSeconds` - the same two minutes any system
        // gets - rather than on however much of a huge number is left. A player
        // who calls a captain home does not want the system they were working
        // pinned open for the rest of the session.
        if (bubbleHoldsPlayerAsset(bubble)) {
            bubble.holdSeconds = kCoolingSeconds;
            continue;
        }
        bubble.holdSeconds -= dt;
        if (bubble.holdSeconds <= 0.0) {
            m_bubbles.erase(m_bubbles.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
    }
}

bool SpaceWorld::leaveSystemFor(std::uint32_t destination)
{
    // ⚑⚑⚑ THIS USED TO BE A FILTERED TEARDOWN AND IS A CROSSING PLUS A DROP
    // NOW (Phase 38 stage A). `despawnSystem` walked the Transform pool, kept
    // whatever matched the player's index and destroyed the rest - a filter
    // over one shared registry, and one that had to be right. A bubble IS the
    // system's contents, so the player leaves and the bubble is released, and
    // nothing has to decide what belonged to it.
    //
    // ⚑⚑⚑⚑ THE PLAYER LANDS IN A BUBBLE THAT IS FRESH, ALWAYS, AND THAT IS
    // NOT A MISSED REUSE. `loadSystem` is about to build the destination's sky
    // from its spec, so a bubble the destination already had would have that
    // sky built ON TOP of it. The case is real rather than theoretical:
    // `enterSystem` on the system you are already standing in is how the
    // console and the tests re-roll a sky, and the unconditional teardown this
    // replaced made that safe for free. Caught by
    // `posting_a_picket_adds_no_hulls_to_the_sky`, which counted a doubled
    // garrison - the first thing the plural registry broke, and it broke it in
    // the one direction a "drop the old one" reading does not cover.
    // ⚑⚑⚑⚑ THE DESTINATION MAY ALREADY BE OPEN, AND THAT IS THE PHASE'S EXIT
    // (Phase 38 stage C). A bubble retained when the player last left it is
    // taken back as it stands - the same hulls, on the same damage, where the
    // tick has flown them to since - rather than generated again. Searched
    // from slot 1 because you cannot arrive in the bubble you are LEAVING:
    // the front one is the system the player is standing in, and re-entering
    // it is a re-roll of that sky (stage A's `enterSystem(here)` case), not a
    // reunion with it.
    std::unique_ptr<SystemBubble> arrival;
    for (std::size_t i = 1; i < m_bubbles.size(); ++i) {
        if (m_bubbles[i]->system == destination) {
            arrival = std::move(m_bubbles[i]);
            m_bubbles.erase(m_bubbles.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    const bool fresh = arrival == nullptr;
    if (fresh) {
        arrival = std::make_unique<SystemBubble>();
        arrival->system = destination;
        furnishBubble(*arrival);
    } else {
        // ⚑ NOT re-furnished. `furnishBubble` reseeds the system's chunk
        // stream, so running it over a bubble that has been drawing from that
        // stream for two minutes would rewind it - the determinism risk the
        // spec names, arriving through the one door that looks like tidying up.
        // The statics it would write are already there and already correct.
        //
        // The clock stops because the player is in it: a bubble the player is
        // standing in is held open by that and not by a countdown, and leaving
        // it again is what sets a new window.
        arrival->holdSeconds = 0.0;
    }
    // ⚑⚑ ASKED WITH THE PLAYER STILL IN THE BUBBLE, WHICH IS THE ONLY MOMENT
    // IT CAN BE ASKED. One tick later every pilot that was attacking the
    // player has dropped to Idle for want of a target, so this exact call
    // moved below the migrate would answer "no fight here" in precisely the
    // case the retention exists for. `kCoolingSeconds` carries the reasoning.
    const double hold = bubbleRetentionSeconds(*m_bubbles.front());
    ecs::Registry& leaving = m_bubbles.front()->registry;
    const std::uint32_t departedIndex = playerEntityIndex();
    // ⚑⚑ THE SCHEMA IS WHAT MOVES WITH YOU, AND IT IS THE SAME LIST THE SAVE
    // WRITES. A component the world does not persist is one it rebuilds, and a
    // jump is exactly when that rebuild happens - so "what survives a save" and
    // "what survives a jump" being one list is a statement about the world
    // rather than a convenience. The player's entity INDEX is not preserved and
    // cannot be; `playerEntityIndex()` recomputes from the pool, which is why
    // nothing had to learn about this.
    (void)makeSnapshotSchema().migrate(leaving, arrival->registry, leaving.entityFromIndex(departedIndex));
    // ⚑⚑⚑⚑ AND NOTHING LEFT BEHIND MAY STILL NAME THE PLAYER. The slot they
    // just vacated is the next one `Registry::create` hands out in that
    // registry, and the puppet reconciles spawn into every instantiated bubble
    // on every tick — so a raider left holding `targetIndex` would transfer
    // the fight, at full aggression, to whichever hauler inherits it. This was
    // free while the bubble was dropped on the same line; it is a live bug the
    // moment one is kept.
    forgetDepartedPlayer(*m_bubbles.front(), departedIndex);
    // ⚑⚑⚑⚑ THIS WAS `m_bubbles.clear()`, AND STAGE C IS THE STAGE THE SPEC
    // SAID WOULD COME HERE AND CHANGE IT. The system the player has just left
    // keeps running for `kCoolingSeconds` when a fight was live in it at the
    // moment of departure, and is released now when it was not. Retained
    // bubbles the player did not just leave keep their own clocks and their
    // own slots; the player's is always the front one, because that is what
    // `playerRegistry()` means and every player-scoped reader in this file
    // depends on it.
    std::vector<std::unique_ptr<SystemBubble>> kept;
    kept.reserve(m_bubbles.size() + 1);
    kept.push_back(std::move(arrival));
    if (hold > 0.0) {
        m_bubbles.front()->holdSeconds = hold;
        kept.push_back(std::move(m_bubbles.front()));
    }
    for (std::size_t i = 1; i < m_bubbles.size(); ++i) {
        kept.push_back(std::move(m_bubbles[i]));
    }
    m_bubbles = std::move(kept);
    enforceBubbleCap();
    // ⚑⚑⚑⚑ THE SHIP LIST IS NOT CLEARED HERE ANY MORE, AND THAT LINE WAS A
    // BUG THE MOMENT AN ARRIVAL COULD BE A BUBBLE THAT ALREADY HAD ONE (Phase
    // 38 stage C). `playerShips()` is `m_bubbles.front()->spawnedShips` - the
    // ARRIVAL's list - and it is what the targeting cycle, the hail and the
    // death path look a hull up in. While every arrival was a fresh bubble the
    // clear was a no-op on an empty vector, which is exactly why it survived
    // stage B unnoticed; on a RETAINED bubble it throws away the names of
    // ships that are still alive and still flying in it, so a player who backs
    // out of a fight and comes straight back finds the raider on their screen
    // and cannot target it, cannot hail it, and gets no name for it.
    //
    // ⚑ Nothing replaces it: the list belongs to the bubble, so a bubble that
    // is dropped takes its list with it and a bubble that is kept keeps one
    // that is still true. The identical line in `loadFrom` is left alone - a
    // loaded bubble is freshly constructed and its list is genuinely empty.
    m_combatEffects.clear();
    m_thrusters.clear();
    if (m_audio != nullptr) {
        // Every voice in flight was positioned in the system being left, and a
        // one-shot does not track its emitter - so leaving them running would
        // play the old system's explosions in the new one's coordinates.
        //
        // ⚑⚑ AND IT IS NOT A TEARDOWN ANY MORE, WHICH IS WHY IT IS ONLY HALF OF
        // AN ANSWER (Phase 38 stage C, paid in stage D). The system behind the
        // player is still running and still firing, so silencing the voices
        // already in flight says nothing about its NEXT shot. That half is the
        // ear's frame: `GameAudio` holds the listener's system now and refuses
        // a positional cue from any other, so this line has gone back to
        // meaning exactly what it says - the voices in flight belong to the
        // system being left, and a one-shot does not track its emitter.
        // `decisions/015` says rendering and UI are unaffected in kind, and
        // never noticed that audio is a third output path.
        m_audio->stopAll();
    }
    return fresh;
}

void SpaceWorld::instantiateSystemEntities(ecs::Registry& registry, const sim::SystemSpec& spec)
{
    auto addStatic = [&](core::DVec3 position,
                         core::Vec3 scale,
                         ModelId model,
                         core::Quat orientation = core::Quat::identity()) {
        const ecs::Entity e = registry.create();
        registry.emplace<Transform>(e,
                                    Transform{.position = position,
                                              .previousPosition = position,
                                              .orientation = orientation,
                                              .previousOrientation = orientation});
        registry.emplace<RenderShape>(e, RenderShape{.scale = scale, .model = model});
    };
    // ⚑ Phase 9 stage H: a station's model comes from its ARCHETYPE's def row
    // rather than from a name compiled in here. `StationSpec::archetype` indexes
    // `stationRules`, which `generateUniverse` builds from `defs.stations()` in
    // def order, so the archetype is a direct index into the same list.
    //
    // Resolved once into a table rather than per station, because a name lookup
    // is a string compare and a system can hold a dozen of these. An archetype
    // naming a model that does not exist falls back to the `station` ROLE with
    // a warning from `modelIdFromName` - the same treatment a ship def gets,
    // and since Phase 19 the fallback is data rather than a literal here.
    std::vector<ModelId> stationModels;
    if (m_defs != nullptr) {
        stationModels.reserve(m_defs->stations().size());
        for (const assets::StationDef& archetype : m_defs->stations()) {
            stationModels.push_back(modelIdFromName(*m_defs, archetype.model, "station def", kRoleStation));
        }
    }
    const ModelId defaultStationModel = roleModel(kRoleStation);
    for (const sim::StationSpec& station : spec.stations) {
        const ModelId model =
            station.archetype < stationModels.size() ? stationModels[station.archetype] : defaultStationModel;
        addStatic(station.position, {1.0f, 1.0f, 1.0f}, model);
    }
    // Gates FACE THEIR LANE (Phase 8w) rather than all presenting the same
    // arbitrary world-Z side: the player has to fly through the opening now, so
    // which way through stopped being cosmetic.
    //
    // ⚑ Since Phase 9 stage D the gate is a real aperture authored in
    // assets/meshes/gate.forge, drawn at scale 1 because it is modelled at its
    // own size. It used to be the unit cube stretched to (70, 70, 10) - and
    // that stand-in was drawn +/-35 m wide while kGateRadiusMeters accepted a
    // crossing anywhere inside 70 m, so the game tested for a hole twice the
    // size of the one it drew. The ring's inner radius is 70 m exactly, which
    // closes that without touching the mechanic.
    const ModelId gateModel = roleModel(kRoleGate);
    // Phase 12: the membrane is a second instance at the identical transform
    // rather than a part of the gate mesh, because it draws under a different
    // pipeline. It is non-solid in models.toml, so it joins neither the
    // collision nor the avoidance set and cannot affect the crossing.
    const ModelId membraneModel = roleModel(kRoleGateMembrane);
    const core::DVec3 hub = spec.planets[spec.primaryPlanet].position;
    for (const sim::GateSpec& gate : spec.gates) {
        const core::DVec3 outward = gate.position - hub;
        const double reach = length(outward);
        const core::DVec3 axis = reach > 0.0 ? outward * (1.0 / reach) : core::DVec3{0.0, 0.0, 1.0};
        const core::Quat facing = facingRotation(axis);
        addStatic(gate.position, {1.0f, 1.0f, 1.0f}, gateModel, facing);
        addStatic(gate.position, {1.0f, 1.0f, 1.0f}, membraneModel, facing);
    }
}

void SpaceWorld::rebuildSystemSideData(const sim::SystemSpec& spec)
{
    // ⚑ The star and the planets are the BUBBLE's since stage B - they are
    // read by the per-system tick, so they belong to the frame rather than to
    // the view - and `furnishBubble` has already written them from this same
    // spec. What is left here is everything that exists to be SHOWN to
    // somebody: gates, the target cycle, signals, fields, the survey fog.
    m_gates.clear();
    // The hub the gates are measured from, which is what gives each one its
    // facing (Phase 8w). generateGalaxy places a gate at hub + bearing *
    // gateDistance, so the outward direction IS the lane it serves.
    const core::DVec3 gateHub = spec.planets[spec.primaryPlanet].position;
    for (const sim::GateSpec& gate : spec.gates) {
        const core::DVec3 outward = gate.position - gateHub;
        const double reach = length(outward);
        m_gates.push_back({.name = "Gate: " + m_galaxy.systems[gate.toSystem].name,
                           .toSystem = gate.toSystem,
                           .axis = reach > 0.0 ? outward * (1.0 / reach) : core::DVec3{0.0, 0.0, 1.0},
                           .position = gate.position});
    }

    // Target cycle: stations, gates, planets, star. Lua's stationPosition()
    // anchor is m_targets[0], so stations must stay first.
    m_targets.clear();
    for (const sim::StationSpec& station : spec.stations) {
        m_targets.push_back({.name = station.name, .position = station.position, .surfaceRadius = 0.0});
    }
    for (const GateInstance& gate : m_gates) {
        m_targets.push_back({.name = gate.name, .position = gate.position, .surfaceRadius = 0.0});
    }
    for (const CelestialBody& planet : planets()) {
        m_targets.push_back(
            {.name = planet.name, .position = planet.position, .surfaceRadius = planet.radius});
    }
    m_targets.push_back({.name = sun().name, .position = sun().position, .surfaceRadius = sun().radius});
    m_planetTargetBase = spec.stations.size() + m_gates.size();
    m_starTargetIndex = m_targets.size() - 1;
    m_signalTargetBase = m_targets.size();
    m_dynamicTargets.clear();
    m_targetIndex = 0;
    // Phase 8z: mask the names of anything not identified yet. This lives here
    // rather than in loadSystem because loadSave does NOT go through loadSystem
    // — it calls this function directly (8r's lesson), and a loaded game must
    // show the same fog the saved one did.
    refreshStaticTargetNames();
    // ⚑ And the selection has to move off slot 0, which used to be the first
    // station and is now very often hidden. Arriving with a hidden station
    // selected would print its masked name in the HUD's target readout and
    // hand it to Autopilot — leaking the one thing the fog is for.
    snapSelectionToVisible();

    // Scannable sites (Phase 8e): content regenerates from the system seed;
    // which ones the player has found comes out of SurveySim.
    m_signals.clear();
    std::vector<sim::SignalSpec> signalSpecs;
    m_survey.signalsFor(m_galaxy, m_currentSystem, signalSpecs);
    for (std::uint32_t i = 0; i < signalSpecs.size(); ++i) {
        m_signals.push_back({.index = i,
                             .kind = signalSpecs[i].kind,
                             .position = signalSpecs[i].position,
                             .seed = signalSpecs[i].seed});
    }
    // Asteroid fields (Phase 8f) regenerate from the seed the same way, and
    // are known on sight — a field is a visible thing, not a contact.
    m_mining.fieldsFor(m_galaxy, m_currentSystem, m_fields);
    rebuildDynamicTargets();

    // The avoidance set is no longer built here (Phase 8y): it is rebuilt
    // every tick beside the collision bodies, because rocks are spawned after
    // this runs, wrecks come and go, and ships move. A list assembled once per
    // system load could only ever describe part of what a ship can hit.
    m_avoidance.clear();
    m_avoidStatics = 0;
}

void SpaceWorld::guardManualCruise(double dt)
{
    // ⚑ The player's own cruise, warned and then cut (Phase 8y §D). Cruise
    // exists only in assist mode, so this is the assist system doing its
    // stated job; flying with assist off is still raw Newtonian with no help
    // at all, and sub-cruise flight is unprotected on purpose.
    m_cruiseWarningTimer = std::max(0.0, m_cruiseWarningTimer - dt);
    const std::uint32_t playerIndex = playerEntityIndex();
    const sim::ShipState state = shipState();
    const double speed = length(state.velocity);
    const sim::ShipTuning& tuning = shipTuning();
    const double brake =
        0.5 * static_cast<double>(tuning.reverseAccel) * static_cast<double>(tuning.cruiseAccelScale);
    if (speed < static_cast<double>(tuning.maxSpeed) || brake <= 0.0) {
        return; // not actually travelling yet; nothing to be saved from
    }
    // How much room stopping needs from here, and how much there is. Looking
    // along the VELOCITY rather than the nose: at cruise the ship goes where
    // it is pointed a moment ago, and it is the momentum that hits things.
    //
    // ⚑ The floor is not decoration, and a drive found out why. Stopping
    // distance falls with the SQUARE of speed, so a look-ahead that is only a
    // multiple of it shrinks to nothing exactly as the guard succeeds: at
    // cruise it watched 28 km ahead, and by 220 m/s it was watching 40 cm. The
    // guard blinded itself, stopped warning, let the throttle build the speed
    // back, and oscillated its way into the station it had just saved the ship
    // from. The floor is the distance this hull needs to stop from its own
    // normal envelope — derived from the tuning, not chosen — so the query
    // always reaches far enough to be worth asking.
    const double envelopeSpeed = static_cast<double>(tuning.maxSpeed) * 2.0;
    const double normalBrake = 0.5 * static_cast<double>(tuning.reverseAccel);
    const double floorDistance = envelopeSpeed * envelopeSpeed / (2.0 * normalBrake);
    const double stopping = speed * speed / (2.0 * brake);
    const double lookahead = std::max(kCruiseLookaheadStops * stopping, floorDistance);
    const core::DVec3 ahead = state.position + state.velocity * (lookahead / (speed > 0.0 ? speed : 1.0));
    const double blocked =
        sim::pathBlockedAt(state.position, ahead, sim::kPathClearance, m_avoidance, playerIndex);
    if (blocked < 0.0) {
        return;
    }
    // ⚑ Warned in DISTANCE rather than in seconds, and that is not a liberty
    // taken with the design: at 5.5e6 m/s one second is 5,500 km, so a fixed
    // grace second is either meaningless or already fatal depending on how
    // fast you are going. Stopping distance is the honest currency — the
    // warning lands with room to spare and the cut lands while there is still
    // twice what the brakes need.
    if (m_cruiseWarningTimer <= 0.0) {
        say("Proximity", "Obstruction ahead - cut the drive.");
        if (m_audio != nullptr) {
            m_audio->play2D(m_audio->cues().alarm); // 8t's alarm, finally used
                                                    // for something the player
                                                    // can act on
        }
        m_cruiseWarningTimer = kCruiseWarningRepeatSeconds;
    }
    if (blocked < std::max(stopping * kCruiseCutStops, floorDistance * 0.5)) {
        m_appliedInput.cruise = false; // the flight model's own interruptible
                                       // cruise braking takes it from here
    }
}

const assets::ModelDef* SpaceWorld::modelDef(ModelId model) const
{
    if (m_defs == nullptr) {
        return nullptr;
    }
    const std::uint32_t index = modelIndex(model);
    return index < m_defs->models().size() ? &m_defs->models()[index] : nullptr;
}

double SpaceWorld::modelBaseRadius(ModelId model) const
{
    const assets::ModelDef* def = modelDef(model);
    // The fallback is the ship's own 8 m, which is what the pre-Phase-9 switch
    // returned for anything it did not name. Larger than what you can hit is
    // safe; smaller never is.
    return def != nullptr ? static_cast<double>(def->radius) : 8.0;
}

double SpaceWorld::hullRadius(std::uint32_t entityIndex) const
{
    const RenderShape* shape =
        playerRegistry().tryGet<RenderShape>(playerRegistry().entityFromIndex(entityIndex));
    // Nothing drawn is nothing measured; the 8 m fallback above is the same
    // answer `modelBaseRadius` gives a model it does not know, and it is the
    // reference hull, so a camera keyed on it is exactly unchanged.
    return shape == nullptr ? modelBaseRadius(kNoModel)
                            : modelBaseRadius(shape->model) * static_cast<double>(shape->scale.x);
}

double SpaceWorld::modelAvoidRadius(ModelId model) const
{
    const assets::ModelDef* def = modelDef(model);
    return def != nullptr ? static_cast<double>(def->avoidRadius) : 8.0;
}

bool SpaceWorld::modelIsSolid(ModelId model) const
{
    const assets::ModelDef* def = modelDef(model);
    return def == nullptr || def->solid;
}

ModelId SpaceWorld::roleModel(const char* role) const
{
    if (m_defs == nullptr) {
        return kNoModel;
    }
    const std::uint32_t index = m_defs->roleModelIndex(role);
    if (index == assets::DefDatabase::kNoModel) {
        // Unreachable through the game, which refuses to load defs that do not
        // fill every role (`validateRoles`). Reachable from a test or a tool
        // holding a partial database, so it degrades rather than asserting.
        SOL_LOG_WARN("no [[role]] row fills '%s'", role);
        return kNoModel;
    }
    return static_cast<ModelId>(index);
}

void SpaceWorld::rebuildAvoidance(const SystemBubble& bubble)
{
    // ⚑ Built from the same pools and the same exclusions as the collision
    // bodies below, so "what I avoid" and "what I can hit" are one statement
    // (Phase 8y). Statics first, movers after, and m_avoidStatics is the seam:
    // a ship that must not dodge other ships takes the front of the list.
    //
    // ⚑⚑ ONE SCRATCH BUFFER, REFILLED PER BUBBLE (Phase 38 stage B). The list
    // describes one system and is consumed inside the same iteration that
    // builds it, so nesting the tick costs no per-system copy of it - which is
    // the same bargain `m_collisionBodies` takes below.
    m_avoidance.clear();
    const ecs::Registry& registry = bubble.registry;
    const ecs::Pool<FlightBody>& bodies = registry.storage<FlightBody>();
    const ecs::Pool<RenderShape>& shapes = registry.storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = registry.storage<Transform>();
    const ecs::Pool<Projectile>& projectiles = registry.storage<Projectile>();
    const ecs::Pool<OreChunk>& oreChunks = registry.storage<OreChunk>();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (bodies.contains(entityIndex) || projectiles.contains(entityIndex) ||
            oreChunks.contains(entityIndex)) {
            continue; // ships come after; bolts and ore block nothing
        }
        const RenderShape& shape = shapes.values()[i];
        if (!modelIsSolid(shape.model)) {
            continue; // a gate is a doorway (Phase 8w), and you fly through it
        }
        // A station keeps the wider figure it has carried since Phase 6: its
        // berths ring at 200 m and the approach was tuned against this sphere,
        // so shrinking it to the collision radius would move 8r's docking.
        // Larger than what you can hit is always safe; smaller never is.
        // That is `avoid_radius` in models.toml now, and it is a property of
        // every model rather than a branch naming one of them.
        const double radius = modelAvoidRadius(shape.model) * static_cast<double>(shape.scale.x);
        m_avoidance.push_back(
            {.position = transforms.get(entityIndex).position, .radius = radius, .handle = entityIndex});
    }
    m_avoidance.push_back({.position = bubble.star.position, .radius = bubble.star.radius});
    for (const CelestialBody& planet : bubble.planets) {
        m_avoidance.push_back({.position = planet.position, .radius = planet.radius});
    }
    m_avoidStatics = m_avoidance.size();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (!bodies.contains(entityIndex)) {
            continue;
        }
        const RenderShape& shape = shapes.values()[i];
        m_avoidance.push_back({.position = transforms.get(entityIndex).position,
                               .radius = modelBaseRadius(shape.model) * static_cast<double>(shape.scale.x),
                               .handle = entityIndex});
    }
}

void SpaceWorld::setAudio(GameAudio* audio)
{
    m_audio = audio;
    // The ear arrives after the galaxy does - `content.initialize` runs the
    // generator, and `main.cpp` hands the device over afterwards - so the frame
    // is pushed here as well as from `enterFrame`. Without it the listener sits
    // in system 0 until the player's first jump, and every positional cue
    // before then is refused as foreign.
    if (m_audio != nullptr) {
        m_audio->setListenerSystem(m_currentSystem);
    }
}

void SpaceWorld::enterFrame(std::uint32_t systemIndex)
{
    m_currentSystem = systemIndex;
    m_combatEffects.setFrame(systemIndex);
    if (m_audio != nullptr) {
        m_audio->setListenerSystem(systemIndex);
    }
}

void SpaceWorld::loadSystem(std::uint32_t systemIndex, std::uint32_t fromSystem)
{
    // ⚑ Jumping out from under a hold is running, and it is recorded as such
    // BEFORE the patrol that opened it stops existing (Phase 36 stage C). This
    // is the site rather than `leaveSystemFor` for the reason Phase 35 stage D
    // recorded: state is dropped where the thing that invalidates it HAPPENS.
    // Nothing is said - the ship that would say it is a system away.
    endInspection(InspectionOutcome::Ran, nullptr);
    // ⚑⚑ THE PLAYER CROSSES FIRST, AND THAT CROSSING IS WHAT A JUMP IS (Phase
    // 38 stage A). Everything below this line runs against the DESTINATION's
    // bubble, `instantiateSystemEntities` included - which is why the entities
    // it spawns land in the right frame without knowing there is such a thing
    // as a frame.
    const bool freshSky = leaveSystemFor(systemIndex);
    enterFrame(systemIndex);
    // Knowledge (Phase 8e): being here is what makes a system known, and a
    // gate names where it leads — the map grows along the lanes you fly.
    m_survey.notifyArrival(m_galaxy, systemIndex);
    m_dockedStation = kNoIndex;
    m_dockedBerth = kNoIndex;
    // A clearance belongs to a station in the system you just left (Phase 8r),
    // for the same reason autopilot is dropped on the line below: the target
    // list is about to change under it.
    m_clearance = DockClearance{};
    m_pendingDockRequest = kNoIndex;
    m_berthRefusalTimer = 0.0;
    // Who you have talked to is about to stop existing (Phase 8s): every pilot
    // in the table belongs to the system being left, and entity indices are
    // reused, so keeping it would let a new pilot inherit a dead one's words.
    m_hails.clear();
    m_pendingHail = HailRequest{};
    m_answeringHail = HailMemory{};
    clearCommand(); // the target list is about to change under it
    const sim::SystemSpec& spec = m_galaxy.systems[systemIndex];
    // ⚑ ONE DEFINITION OF WHAT A SYSTEM'S SKY IS (Phase 38 stage B). Statics,
    // rocks and ambient traffic were three calls spelled out here, and stage B
    // needed the same three for a bubble the player is NOT arriving in - so
    // they became `fillSystemSky` rather than a second copy that would drift.
    // The side data below is the half that stays here, because all of it -
    // gates, the target cycle, the survey fog - exists to be shown to a player.
    // ⚑⚑⚑⚑ ONLY WHEN THE BUBBLE IS NEW (Phase 38 stage C). A retained bubble
    // arrives with its sky already in it - the same hulls, still carrying the
    // damage they were left on - and filling one over the top is the doubled
    // garrison stage A shipped, through a third door. This single condition is
    // the difference between the phase's exit happening and the player being
    // handed a freshly generated system that merely looks like the one they
    // fought in.
    //
    // ⚑ The side data below is NOT conditional and must not become so: gates,
    // the target cycle and the survey fog are the player's view of wherever
    // they now are, they were built for the system being left, and they are
    // rebuilt on every arrival however the sky got there.
    if (freshSky) {
        fillSystemSky(playerBubble());
    }
    rebuildSystemSideData(spec);

    // Arrival point: off the gate we came through, facing the playfield; on
    // a fresh start, just off the first station (or the hub failing that).
    const core::DVec3 hub = spec.planets[spec.primaryPlanet].position;
    core::DVec3 arrival = hub + core::DVec3{0.0, 0.0, 2.0e5};
    // What the nose points at once you are there (Phase 10). The comment above
    // has promised "facing the playfield" since Phase 7 and this function never
    // wrote an orientation at all, so a crossing kept the heading it crossed
    // with: the ring ended up dead ahead filling the view, and one press of W
    // flew the player straight back through it, arriving in the same state.
    // Note it is the HUB rather than "away from the gate" - at a gate the two
    // are the same direction, and the hub is the one the player wants.
    core::DVec3 lookAt = hub;
    if (fromSystem != kNoIndex) {
        for (const sim::GateSpec& gate : spec.gates) {
            if (gate.toSystem == fromSystem) {
                arrival = gate.position + normalize(hub - gate.position) * 500.0;
                break;
            }
        }
    } else if (!spec.stations.empty()) {
        arrival = spec.stations[0].position + core::DVec3{0.0, 0.0, 800.0};
        lookAt = spec.stations[0].position; // a new pilot faces their home port
    }
    m_playerSpawn = arrival;

    const std::uint32_t playerIndex = playerEntityIndex();
    Transform& transform = playerRegistry().storage<Transform>().get(playerIndex);
    transform.position = arrival;
    transform.previousPosition = arrival;
    // lookAlong, not facingRotation: the latter takes the model's +Z onto an
    // axis, which is what turns the gate SLAB to its lane. A ship's nose is -Z,
    // so facingRotation here would arrive with the tail toward the playfield.
    transform.orientation = lookAlong(lookAt - arrival);
    // Both ends of the tick. The render nlerps previous->current, so writing
    // only one of the two swings the ship through the whole turn on screen.
    transform.previousOrientation = transform.orientation;
    playerRegistry().storage<FlightBody>().get(playerIndex) = FlightBody{};
    m_playerDamageTimer = 0.0f;

    // You always know what you are touching (Phase 8z §B).
    identifyTouchedObjects(fromSystem);
}

void SpaceWorld::identifyTouchedObjects(std::uint32_t fromSystem)
{
    bool changed = false;
    if (fromSystem != kNoIndex) {
        // The gate you just flew through: you know what it is and where it
        // goes, because you have just come from there. Without this the first
        // jump strands the player at an anonymous object they flew through, and
        // the way home is a contact they have to scan.
        for (std::uint32_t i = 0; i < m_gates.size(); ++i) {
            if (m_gates[i].toSystem == fromSystem) {
                changed = m_survey.notifyGateIdentified(m_galaxy, m_currentSystem, i) || changed;
                break;
            }
        }
    }
    if (m_dockedStation != kNoIndex) {
        changed = m_survey.notifyStationIdentified(m_galaxy, m_currentSystem, m_dockedStation) || changed;
    }
    if (changed) {
        refreshStaticTargetNames();
    }
}

bool SpaceWorld::jumpNearestGate(double activationRange)
{
    if (isDocked()) {
        return false; // undock first
    }
    const core::DVec3 playerPosition = shipState().position;
    const GateInstance* nearest = nullptr;
    double nearestDistance = activationRange;
    for (const GateInstance& gate : m_gates) {
        const double distance = length(gate.position - playerPosition);
        if (distance <= nearestDistance) {
            nearestDistance = distance;
            nearest = &gate;
        }
    }
    if (nearest == nullptr) {
        return false;
    }
    const std::uint32_t destination = nearest->toSystem;
    if (!m_jump.begin(destination)) {
        return false; // already in the lane; you cannot jump out of a jump
    }
    SOL_LOG_INFO("jumping: %s -> %s", currentSystemName(), m_galaxy.systems[destination].name.c_str());
    return true;
}

void SpaceWorld::tickGateCrossing()
{
    if (isDocked() || m_jump.active() || m_gates.empty()) {
        return;
    }
    const Transform& transform = playerRegistry().storage<Transform>().get(playerEntityIndex());
    for (const GateInstance& gate : m_gates) {
        // Did this tick's motion carry the ship through the opening (Phase 8w)?
        // Swept by construction — the test is about a segment, so a ship under
        // boost cannot step over the plane between ticks — and directional by
        // construction, so flying past the gate can no longer take you.
        if (!sim::crossedAperture(transform.previousPosition,
                                  transform.position,
                                  gate.position,
                                  gate.axis,
                                  kGateRadiusMeters)) {
            continue;
        }
        if (m_jump.begin(gate.toSystem)) {
            SOL_LOG_INFO(
                "jumping: %s -> %s", currentSystemName(), m_galaxy.systems[gate.toSystem].name.c_str());
        }
        return;
    }
}

void SpaceWorld::advanceJumpTransition(double deltaSeconds)
{
    if (!m_jump.active()) {
        return;
    }
    // Coast (Phase 8w). The sim is suspended for the length of the transition,
    // so without this the ship stops dead the instant the jump arms and the
    // gate it just flew through stays nailed in the middle of the view for the
    // whole tunnel — which is what made the effect read as a screen filter
    // rather than as travel. Transform only: no forces, no collision, no tick.
    // The gate recedes behind the player because they are still moving.
    if (m_jump.phase() == sim::JumpPhase::Tunnel) {
        const std::uint32_t playerIndex = playerEntityIndex();
        Transform& transform = playerRegistry().storage<Transform>().get(playerIndex);
        const core::DVec3 velocity = playerRegistry().storage<FlightBody>().get(playerIndex).velocity;
        transform.previousPosition = transform.position;
        transform.position += velocity * deltaSeconds;
    }
    m_jump.advance(deltaSeconds);
    if (m_jump.swapDue()) {
        // Full stretch: there is nothing legible on screen to pop. The load
        // itself is unchanged from the instant version — it is only covered.
        loadSystem(m_jump.destination(), m_currentSystem);
        m_jump.noteSwapped();
        SOL_LOG_INFO("Arrived in '%s'", currentSystemName());
    }
}

sol::core::DVec3 SpaceWorld::dockPoint(std::uint32_t stationIndex) const
{
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const core::DVec3& station = spec.stations[stationIndex].position;
    // Since Phase 8r a ship that flew in on a clearance is parked in the berth
    // it was assigned, and this is the one function that answers "where does a
    // ship parked at this station sit" — tick() pins the docked ship here every
    // frame, undock releases relative to it, and the death rule respawns at it.
    if (m_dockedBerth != kNoIndex) {
        return sim::berthPoint(station, m_dockedBerth);
    }
    // No berth: the pre-8r point, 250 m above the station. Still reached by the
    // dev shortcut and by the death respawn, neither of which asks anyone.
    return station + core::DVec3{0.0, 250.0, 0.0};
}

void SpaceWorld::completeDock(std::uint32_t station, std::uint32_t berth)
{
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    m_dockedStation = station;
    m_dockedBerth = berth;
    m_lastDockSystem = m_currentSystem;
    m_lastDockStation = station;
    // A clearance is consumed by being used; nothing below may see one.
    m_clearance = DockClearance{};
    m_pendingDockRequest = kNoIndex;
    // Docking identifies the port (Phase 8z §B): you are inside it. The dev
    // dock shortcut routes through here too, so it cannot leave the player
    // parked in a station the map still calls a contact.
    if (m_survey.notifyStationIdentified(m_galaxy, m_currentSystem, station)) {
        refreshStaticTargetNames();
    }

    // Park at the pad, kill relative motion, refresh the spawn anchor (the
    // death rule respawns at the last dock).
    const std::uint32_t playerIndex = playerEntityIndex();
    Transform& transform = playerRegistry().storage<Transform>().get(playerIndex);
    const core::DVec3 pad = dockPoint(station);
    transform.position = pad;
    transform.previousPosition = pad;
    playerRegistry().storage<FlightBody>().get(playerIndex) = FlightBody{};
    m_playerSpawn = pad;
    clearCommand();
    if (m_audio != nullptr) {
        // Docking kills relative motion, so the engine has to go quiet with
        // it - otherwise the hum runs on through every station screen.
        m_audio->play2D(m_audio->cues().docking);
        m_audio->setEngineThrottle(0.0f);
    }
    SOL_LOG_INFO("docked at '%s'", spec.stations[station].name.c_str());
    // Missions (Phase 8c): Dock objectives first, so a following Deliver at
    // this station can hand in on the same visit; the dock event tells
    // GameContent to re-open the board.
    m_missions.notifyDock(m_currentSystem, station);
    processMissionDeliveries();
    // Market intel (Phase 8g): standing on the pad is the one price reading
    // you never have to pay for, and it is what seeds the "elsewhere" column
    // on every other station's Trade tab.
    recordDockedMarket();
    m_dockEventPending = true;
    rebuildDynamicTargets(); // the berth slot goes away with the clearance
}

std::uint32_t SpaceWorld::nearestStationWithin(double range, double* outDistance) const
{
    if (m_currentSystem >= m_galaxy.systems.size()) {
        return kNoIndex;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const core::DVec3 playerPosition = shipState().position;
    std::uint32_t nearest = kNoIndex;
    double nearestDistance = range;
    for (std::uint32_t i = 0; i < spec.stations.size(); ++i) {
        const double distance = length(spec.stations[i].position - playerPosition);
        if (distance <= nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }
    if (nearest != kNoIndex && outDistance != nullptr) {
        *outDistance = nearestDistance;
    }
    return nearest;
}

bool SpaceWorld::tryDockNearestStation(double range)
{
    if (isDocked()) {
        return false;
    }
    const std::uint32_t nearest = nearestStationWithin(range, nullptr);
    if (nearest == kNoIndex) {
        return false;
    }
    // Docking rights (Phase 8b): a hostile owner refuses. Death respawn
    // bypasses this path on purpose — dock stays the safe room.
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner < m_factionTable.size() && m_factionSim.playerHostile(owner)) {
        SOL_LOG_WARN("docking denied at '%s': %s is hostile",
                     spec.stations[nearest].name.c_str(),
                     m_factionTable[owner].name.c_str());
        return false;
    }
    completeDock(nearest, kNoIndex);
    return true;
}

bool SpaceWorld::undock()
{
    if (!isDocked()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const std::uint32_t playerIndex = playerEntityIndex();
    Transform& transform = playerRegistry().storage<Transform>().get(playerIndex);
    const core::DVec3& station = spec.stations[m_dockedStation].position;
    // Released where you docked: pushed 100 m straight out from the berth if
    // you flew in on a clearance (Phase 8r), or 500 m above the station if you
    // arrived by the shortcut or woke here after dying. Either way the station
    // sphere is comfortably clear.
    core::DVec3 release = station + core::DVec3{0.0, 500.0, 0.0};
    if (m_dockedBerth != kNoIndex) {
        const core::DVec3 berth = sim::berthPoint(station, m_dockedBerth);
        const core::DVec3 outward = berth - station;
        const double reach = length(outward);
        release = reach > 0.0 ? berth + outward * (100.0 / reach) : berth;
    }
    transform.position = release;
    transform.previousPosition = release;
    playerRegistry().storage<FlightBody>().get(playerIndex) = FlightBody{};
    SOL_LOG_INFO("undocked from '%s'", spec.stations[m_dockedStation].name.c_str());
    m_dockedStation = kNoIndex;
    m_dockedBerth = kNoIndex;
    return true;
}

const char* SpaceWorld::dockedStationName() const
{
    if (!isDocked()) {
        return "";
    }
    return m_galaxy.systems[m_currentSystem].stations[m_dockedStation].name.c_str();
}

// --- Comms and docking clearance (Phase 8r) ---------------------------------

void SpaceWorld::say(const std::string& from, const std::string& text)
{
    m_comms.push_back({.from = from, .text = text, .secondsLeft = kCommsMessageSeconds});
    if (m_comms.size() > kCommsLines) {
        m_comms.erase(m_comms.begin(), m_comms.begin() + (m_comms.size() - kCommsLines));
    }
    SOL_LOG_INFO("comms: %s: %s", from.c_str(), text.c_str());
}

void SpaceWorld::drainContestResolutions()
{
    // Announce the contest over the player's own head, once. A system change
    // re-arms this, so flying back into a war you already knew about tells
    // you again - which is right, because you have just arrived.
    const sim::SystemContest here = m_factionSim.contestOf(m_currentSystem);
    const bool live = m_factionSim.contested(m_currentSystem);
    if (!live) {
        m_announcedContestSystem = kNoIndex;
        m_announcedContestAttacker = kNoIndex;
    } else if (m_currentSystem != m_announcedContestSystem || here.attacker != m_announcedContestAttacker) {
        m_announcedContestSystem = m_currentSystem;
        m_announcedContestAttacker = here.attacker;
        if (here.attacker < m_factionTable.size() && !isDocked()) {
            say(kFleetcom, m_factionTable[here.attacker].name + " is pressing a claim here.");
        }
    }

    m_contestResolutions.clear();
    m_factionSim.takeResolutions(m_contestResolutions);
    for (const sim::ContestResolution& resolution : m_contestResolutions) {
        // A Hold objective settles on the resolution regardless of where the
        // player is standing: the contract was about the system, not about
        // being there to watch.
        m_missions.notifyContestResolved(resolution.system, resolution.winner);

        const char* winnerName = resolution.winner < m_factionTable.size()
                                     ? m_factionTable[resolution.winner].name.c_str()
                                     : "Nobody";
        const char* loserName = resolution.loser < m_factionTable.size()
                                    ? m_factionTable[resolution.loser].name.c_str()
                                    : "nobody";
        if (resolution.system < m_galaxy.systems.size()) {
            SOL_LOG_INFO("[territory] %s: %s %s %s",
                         m_galaxy.systems[resolution.system].name.c_str(),
                         winnerName,
                         resolution.flipped ? "takes the system from" : "holds against",
                         loserName);
        }
        if (resolution.system != m_currentSystem) {
            continue; // elsewhere: the map is where you find out
        }
        m_announcedContestSystem = kNoIndex;
        m_announcedContestAttacker = kNoIndex;
        // ⚑ Kept SHORT on purpose. The comms panel is clamped against the
        // target panel (8r) and its cell clips rather than overruns (8s), so
        // a long line is silently cut at the right edge - which a drive found
        // here, on a sentence that read fine in the log. The longest faction
        // name in a generated galaxy is ~17 characters; budget for that.
        say(kFleetcom,
            resolution.flipped ? std::string(winnerName) + " holds this system now."
                               : std::string(loserName) + " driven off. System holds.");
    }
}

void SpaceWorld::drainTraderLosses()
{
    m_traderLossEvents.clear();
    m_factionSim.takeTraderLosses(m_traderLossEvents);
    for (const sim::TraderLoss& loss : m_traderLossEvents) {
        ++m_traderLossCount;
        // An escort contract ends here, and how it ends depends on who fired.
        // The flag is set by handleShipDestroyed, which is the only place that
        // knows: the coarse record is told a hauler died and never by whom.
        const bool betrayed =
            std::find(m_playerKilledTraders.begin(), m_playerKilledTraders.end(), loss.trader) !=
            m_playerKilledTraders.end();
        m_missions.notifyTraderLost(loss.trader, betrayed);
        // One place logs, whichever road the loss came down: attrition rolling
        // in a system nobody is watching, a raider finishing one off, or the
        // player shooting a hauler off their own bow.
        SOL_LOG_INFO("[attrition] trader %u lost in %s",
                     loss.trader,
                     loss.system < m_galaxy.systems.size() ? m_galaxy.systems[loss.system].name.c_str()
                                                           : "transit");
    }
    // Cleared only once the losses it describes have been drained, so it does
    // not matter whether combat ran before or after this call in the frame.
    m_playerKilledTraders.clear();
}

bool SpaceWorld::killCoarseTrader(std::uint32_t traderIndex)
{
    if (traderIndex >= m_economy.traders().size()) {
        return false;
    }
    // ⚑ A dev lever must reach only states the sim can reach (8u's rule, from
    // a lever that cleared a contest without queueing its resolution). So a
    // trader with a body in front of the player dies the way a real one does —
    // explosion, wreck, loot, and the coarse loss falling out of the same path
    // below — rather than being struck off the books while its hull flies on.
    for (std::size_t slot = 0; slot < m_bubbles.size(); ++slot) {
        SystemBubble& bubble = *m_bubbles[slot];
        const ecs::Pool<TraderPuppet>& puppets = bubble.registry.storage<TraderPuppet>();
        for (std::size_t i = 0; i < puppets.size(); ++i) {
            if (puppets.values()[i].traderIndex == traderIndex) {
                handleShipDestroyed(bubble, puppets.entityIndices()[i]);
                return true;
            }
        }
    }
    const sim::TraderRoute route = m_economy.route(traderIndex);
    if (!m_economy.loseTrader(traderIndex)) {
        return false;
    }
    m_factionSim.recordTraderLoss(route.system, traderIndex);
    return true;
}

bool SpaceWorld::killMinerPuppet(std::uint32_t market)
{
    // ⚑ 8u's rule again, and here it is the whole implementation: a miner has
    // no coarse record of its own, so there is nothing to strike off and no
    // second road to write. Either the ship is in the sky and dies the way a
    // raider's shot kills it — explosion, wreck, loot, the outpost's draw
    // stopping — or the lever answers false and the drive has to go somewhere
    // there is a mine.
    for (std::size_t slot = 0; slot < m_bubbles.size(); ++slot) {
        SystemBubble& bubble = *m_bubbles[slot];
        const ecs::Pool<MinerPuppet>& miners = bubble.registry.storage<MinerPuppet>();
        for (std::size_t i = 0; i < miners.size(); ++i) {
            if (miners.values()[i].market == market) {
                handleShipDestroyed(bubble, miners.entityIndices()[i]);
                return true;
            }
        }
    }
    return false;
}

void SpaceWorld::killCaptain(std::size_t captainIndex, std::uint32_t system)
{
    // ⚑⚑⚑⚑ THE HULL IS GONE AND SO IS THE PERSON (the user's ruling 14, taken
    // before a line was written). The two softer answers were both on the table
    // and both were refused for the same reason stage C's own comment gives
    // about the interim it left: a death the player can shrug off is a positive
    // statement that the danger is free, and this phase's whole economic point
    // is that a captain is a business decision. Replacing the hull at the last
    // dock would have mirrored the player's own death exactly and cost almost
    // nothing; keeping the captain alive to be re-assigned would have made an
    // 8-20% cut a bet with no downside. The order you gave is what killed them.
    //
    // ⚑⚑⚑ AND IT IS THE ONE CONSEQUENCE IN THIS GAME THAT CANNOT BE UNDONE BY
    // FLYING BACK. Everything else the player loses is recoverable - a hold, a
    // deductible, a standing, even their own ship. A captain is a name that was
    // drawn once and is not in any def, so there is nothing to restore them
    // FROM: `CastSeat`'s "a regular's name exists in no def at all", which
    // stage A read as a save-format convenience, is a design fact here.
    if (captainIndex >= m_captains.size()) {
        return;
    }
    const Captain& captain = m_captains[captainIndex];
    const std::uint32_t fleetIndex = captain.ship;
    const char* where =
        system < m_galaxy.systems.size() ? m_galaxy.systems[system].name.c_str() : "deep space";

    // INSURANCE, AND IT IS THE SAME FIVE PER CENT THAT CHANGES HANDS WHEN THE
    // PLAYER DIES - the other way. `kInsuranceRate` is the deductible you pay to
    // wake at the last dock in a hull you keep; a hull flown by somebody else is
    // not covered like that, and what comes back is a token against a total
    // loss. Borrowed rather than invented on this project's own rule: a new
    // constant here would be a second number meaning "what insurance is worth"
    // with nothing to keep the two in step.
    double payout = 0.0;
    if (fleetIndex < m_fleet.size()) {
        payout = kInsuranceRate * shipValue(m_fleet[fleetIndex]);
        m_playerCredits += payout;
    }

    // THE LINE THE PLAYER CAN FIND, and it is said through `say` rather than
    // only logged because a death two systems away has no other channel: the
    // console is where the player's own ship reports what happened to it, and
    // this is the same event happening to a ship of theirs somebody else was
    // flying. The wreck is what they find when they fly back; this is how they
    // know to.
    char line[192] = {};
    if (payout > 0.0) {
        std::snprintf(
            line, sizeof(line), "lost with all hands in %s - insurance pays %.0f cr", where, payout);
    } else {
        std::snprintf(line, sizeof(line), "lost with all hands in %s", where);
    }
    say(captain.name, line);
    SOL_LOG_WARN("%s was killed in %s (insurance %.0f cr)", captain.name.c_str(), where, payout);

    // ⚑⚑ THE STANDING CONSEQUENCE IS THAT THERE IS NONE, AND SAYING SO IS THE
    // POINT. The spec's list reads "wreck, insurance, standing, and a log line";
    // the standing half turned out to be a defect to close rather than an event
    // to add - see the guard in `handleShipDestroyed`, where killing your own
    // hull used to move your reputation with whichever government's colours it
    // happened to be wearing. Being the VICTIM moves nothing: a faction that
    // burns your freighter has not changed its opinion of you, it has acted on
    // the one it already had, and inventing a reputation hit here would make
    // being attacked lower your standing with your attacker.

    // ⚑⚑⚑ THE KEY OUTLIVES THE PERSON, AND IT HAS TO, because the hall that
    // hired them is derived from the seed and would offer them again the moment
    // they left `m_captains` - see the filter in `captainCandidates`. Recorded
    // HERE rather than in `removeCaptain`, which is also the erase a DISMISSAL
    // goes through: the two calls look alike and mean opposite things.
    if (std::find(m_lostCaptains.begin(), m_lostCaptains.end(), captain.who) == m_lostCaptains.end()) {
        m_lostCaptains.push_back(captain.who);
    }
    removeCaptain(captainIndex);
    if (fleetIndex < m_fleet.size()) {
        removeFleetShip(fleetIndex);
    }
}

void SpaceWorld::removeCaptain(std::size_t captainIndex)
{
    // ⚑⚑⚑ A CAPTAIN INDEX IS HELD IN FOUR PLACES AND THREE OF THEM ARE NOT
    // `m_captains`. `sellShip`'s comment is the precedent - "an erase renumbers
    // the tail; without this a captain silently inherits the hull that moved
    // into the slot" - and this is that hazard with one more table on it,
    // because a `CaptainPuppet` in a bubble the player is not standing in holds
    // an index too and nothing else would ever renumber it.
    if (captainIndex >= m_captains.size()) {
        return;
    }
    const auto index = static_cast<std::uint32_t>(captainIndex);
    m_captains.erase(m_captains.begin() + static_cast<std::ptrdiff_t>(captainIndex));
    if (captainIndex < m_captainPresent.size()) {
        m_captainPresent.erase(m_captainPresent.begin() + static_cast<std::ptrdiff_t>(captainIndex));
    }
    if (captainIndex < m_captainDetained.size()) {
        m_captainDetained.erase(m_captainDetained.begin() + static_cast<std::ptrdiff_t>(captainIndex));
    }
    // EVERY BUBBLE, NOT ONLY THE PLAYER'S. A captain can die in a system the
    // player has left, and the hull of a DIFFERENT captain two systems further
    // out is what would inherit the dead one's index.
    for (const std::unique_ptr<SystemBubble>& bubble : m_bubbles) {
        ecs::Pool<CaptainPuppet>& puppets = bubble->registry.storage<CaptainPuppet>();
        for (std::size_t i = 0; i < puppets.size(); ++i) {
            CaptainPuppet& puppet = puppets.values()[i];
            if (puppet.captainIndex > index) {
                --puppet.captainIndex;
            } else if (puppet.captainIndex == index) {
                // The dead captain's own body, on the tick it is being walked
                // out. Pointed past the end so `syncCaptainPuppets` dooms it
                // rather than re-reading a slot that now holds somebody else.
                puppet.captainIndex = kNoIndex;
            }
        }
    }
}

void SpaceWorld::removeFleetShip(std::size_t fleetIndex)
{
    // `sellShip`'s tail, lifted out whole because stage D is its second caller
    // and a second copy of an index shift is how the two stop agreeing. ⚑ The
    // equal case IS written here and is not in `sellShip`: that one refuses a
    // hull somebody is flying, so it cannot reach it; this one is called
    // BECAUSE somebody was flying it, and `removeCaptain` has already gone.
    if (fleetIndex >= m_fleet.size()) {
        return;
    }
    m_fleet.erase(m_fleet.begin() + static_cast<std::ptrdiff_t>(fleetIndex));
    if (m_activeShip > fleetIndex) {
        --m_activeShip;
    }
    for (Captain& captain : m_captains) {
        if (captain.ship == kNoIndex) {
            continue;
        }
        if (captain.ship > fleetIndex) {
            --captain.ship;
        } else if (captain.ship == fleetIndex) {
            captain.ship = kNoIndex; // unreachable today; not left dangling if it ever is
        }
    }
}

bool SpaceWorld::killCaptainPuppet(std::size_t captainIndex, bool byPlayer)
{
    // ⚑ `killMinerPuppet`'s lever against a different body, and it exists for
    // the same reason that one does: the consequence of a captain's hull dying
    // is a thing a test and a drive have to be able to CAUSE, because waiting
    // for a raider to find one is waiting on a die roll. There is no coarse
    // record to strike off either - a stationary captain's body IS the record -
    // so this is the whole implementation.
    for (std::size_t slot = 0; slot < m_bubbles.size(); ++slot) {
        SystemBubble& bubble = *m_bubbles[slot];
        const ecs::Pool<CaptainPuppet>& puppets = bubble.registry.storage<CaptainPuppet>();
        for (std::size_t i = 0; i < puppets.size(); ++i) {
            if (puppets.values()[i].captainIndex == captainIndex) {
                handleShipDestroyed(
                    bubble, puppets.entityIndices()[i], byPlayer ? playerEntityIndex() : kNoIndex);
                return true;
            }
        }
    }
    return false;
}

bool SpaceWorld::killAnyNpcByPlayer(std::uint32_t* outFaction)
{
    SystemBubble& bubble = playerBubble();
    ecs::Registry& registry = bubble.registry;
    const ecs::Pool<ShipPilot>& pilots = registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const std::uint32_t index = pilots.entityIndices()[i];
        if (index == playerEntityIndex() || playerOwnedHull(registry, index)) {
            continue;
        }
        if (pilots.values()[i].factionIndex >= m_factionTable.size()) {
            continue; // an unaffiliated console spawn has no standing to move
        }
        if (outFaction != nullptr) {
            *outFaction = pilots.values()[i].factionIndex;
        }
        handleShipDestroyed(bubble, index, playerEntityIndex());
        return true;
    }
    return false;
}

bool SpaceWorld::bubbleHoldsPlayerAssetIn(std::uint32_t system) const
{
    // Asked of the RECORD, which is `bubbleHoldsPlayerAsset`'s own rule and the
    // one thing that function must get right: the order is what holds a system
    // open, so this can answer for a system whose bubble is already gone.
    for (std::size_t i = 0; i < m_captains.size(); ++i) {
        if (stationary(m_captains[i].order.kind) && captainSystem(i) == system) {
            return true;
        }
    }
    return false;
}

sol::core::DVec3 SpaceWorld::clearedBerthPoint() const
{
    if (!hasClearance() || m_currentSystem >= m_galaxy.systems.size()) {
        return {};
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (m_clearance.station >= spec.stations.size()) {
        return {};
    }
    return sim::berthPoint(spec.stations[m_clearance.station].position, m_clearance.berth);
}

void SpaceWorld::clearClearance(const char* reason)
{
    if (!hasClearance()) {
        return;
    }
    if (reason != nullptr && m_currentSystem < m_galaxy.systems.size() &&
        m_clearance.station < m_galaxy.systems[m_currentSystem].stations.size()) {
        say(m_galaxy.systems[m_currentSystem].stations[m_clearance.station].name, reason);
    }
    m_clearance = DockClearance{};
    // The berth's nav slot goes with it, and the compaction inside is what
    // disengages an autopilot that was still flying to it (Phase 8i's rule).
    rebuildDynamicTargets();
}

// --- The transponder (Phase 36 stage A, decisions/017) ---------------------

bool SpaceWorld::setTransponder(bool on)
{
    if (m_transponderOn == on) {
        return false;
    }
    m_transponderOn = on;
    if (on) {
        say(kFleetcom, "Transponder active. Broadcasting " + broadcastIdentity() + ".");
        return true;
    }
    say(kFleetcom, "Transponder off. You are running dark.");
    // ⚑⚑ THE GRANT WAS MADE TO SOMEBODY WHO WAS IDENTIFYING THEMSELVES, AND
    // THEY HAVE STOPPED. Phase 8r built the clearance as a timed, REVOCABLE
    // grant and nothing until now has ever revoked one for a reason other than
    // running out of seconds; this is the first. Doing it here rather than in
    // the approach check is stage 35-D's lesson applied ahead of the bug:
    // state is dropped where the thing that invalidates it HAPPENS, not where
    // somebody later notices.
    clearClearance("Transponder lost. Clearance rescinded.");
    return true;
}

std::string SpaceWorld::broadcastIdentity() const
{
    // The hull half. Falls back to a generic rather than an empty string: an
    // identity that is blank is indistinguishable from running dark, and those
    // are the two states this whole phase exists to tell apart.
    std::string hull = "Ship";
    if (m_activeShip < m_fleet.size() && m_defs != nullptr) {
        if (const assets::ShipDef* def = m_defs->findShip(m_fleet[m_activeShip].defId.c_str())) {
            hull = radioName(def->name);
        }
    }
    // The registration half. Drawn from the universe seed alone, so it is the
    // same for the whole playthrough, different in the next one, and costs
    // nothing on disk. Two letters and three digits is a plate, not a hash:
    // it has to be readable aloud in a comms line.
    core::Rng rng(m_universeSeed, 0x7261'6e64'6f6d'0036ull);
    const std::uint32_t a = rng.nextU32();
    const std::uint32_t b = rng.nextU32();
    char plate[8] = {};
    plate[0] = static_cast<char>('A' + (a % 26u));
    plate[1] = static_cast<char>('A' + ((a / 26u) % 26u));
    plate[2] = '-';
    plate[3] = static_cast<char>('0' + (b % 10u));
    plate[4] = static_cast<char>('0' + ((b / 10u) % 10u));
    plate[5] = static_cast<char>('0' + ((b / 100u) % 10u));
    return hull + " " + plate;
}

bool SpaceWorld::requestDocking()
{
    if (isDocked()) {
        return false;
    }
    if (hasClearance()) {
        const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
        say(spec.stations[m_clearance.station].name,
            "You are already cleared for berth " + std::to_string(m_clearance.berth + 1) + ".");
        return false;
    }
    double distance = 0.0;
    const std::uint32_t station = nearestStationWithin(kDockRequestRange, &distance);
    if (station == kNoIndex) {
        say("Comms", "No station in range to hail.");
        return false;
    }
    // The answer is not decided here. GameContent drains this, asks the
    // dock_request hook, and calls grantDocking/denyDocking — the same shape
    // signal_loot and mission_board use, so a refusal can be authored rather
    // than hardcoded.
    m_pendingDockRequest = station;
    // Seeded per (universe, system, station, how many times you have asked), so
    // the answer is deterministic for a run but a second hail at the same
    // station can put you somewhere else — a dispatcher assigning the same
    // berth forever is the tell that nobody is really on the other end.
    core::Rng rng(m_universeSeed ^ (static_cast<std::uint64_t>(m_currentSystem) << 32u),
                  (static_cast<std::uint64_t>(station) << 20u) | ++m_dockRequestCount);
    m_dockRequestRoll = static_cast<double>(rng.nextU32()) * 0x1.0p-32;
    // Deliberately does not name the station: the comms panel's sender column
    // already does, and a line that repeats it is the line that clips.
    say("You", "Requesting docking clearance.");
    return true;
}

bool SpaceWorld::takeDockRequest(std::uint32_t& outStation, double& outRoll)
{
    if (m_pendingDockRequest == kNoIndex) {
        return false;
    }
    outStation = m_pendingDockRequest;
    outRoll = m_dockRequestRoll;
    m_pendingDockRequest = kNoIndex;
    return true;
}

bool SpaceWorld::grantDocking(std::uint32_t station, std::uint32_t berth, const std::string& message)
{
    if (isDocked() || m_currentSystem >= m_galaxy.systems.size()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (station >= spec.stations.size() || berth >= sim::kBerthCount) {
        SOL_LOG_WARN("grant_docking: station %u berth %u is out of range", station, berth);
        return false;
    }
    m_clearance = {.station = station, .berth = berth, .secondsLeft = kClearanceSeconds};
    say(spec.stations[station].name, message);
    rebuildDynamicTargets();
    // Selected outright rather than cycled to, for the reason 8i gave about
    // the mission objective: the player just asked for this, so it is the one
    // thing they certainly want the ship pointed at.
    const std::size_t slot = berthTargetIndex();
    if (slot != kNoTarget) {
        (void)selectTarget(slot);
    }
    return true;
}

void SpaceWorld::denyDocking(std::uint32_t station, const std::string& message)
{
    if (m_currentSystem >= m_galaxy.systems.size()) {
        return;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    say(station < spec.stations.size() ? spec.stations[station].name : std::string("Comms"), message);
}

bool SpaceWorld::hailTarget()
{
    const std::size_t total = m_targets.size() + playerShips().size();
    // The same wrap currentTargetInfo() applies, because a targeted ship can
    // die out from under m_targetIndex and the hail must ask about whatever
    // the HUD is currently showing rather than about a stale slot.
    const std::size_t index = total > 0 ? m_targetIndex % total : 0;
    if (total == 0 || index < m_targets.size()) {
        say("Comms", "No ship selected to hail.");
        return false;
    }
    const SpawnedShip& ship = playerShips()[index - m_targets.size()];
    const core::DVec3 position = playerRegistry().storage<Transform>().get(ship.entity.index).position;
    const double distance = length(position - shipState().position);
    if (distance > kHailRange) {
        // Naming the ship is right here, unlike a station's own lines: the
        // sender column says "Comms", so nothing repeats.
        say("Comms", ship.name + " is out of comms range.");
        return false;
    }
    // Already spoken to: they repeat themselves. This is what stops a hail
    // being a slot machine you re-roll for a better tip.
    for (const HailMemory& memory : m_hails) {
        if (memory.pilot == ship.entity) {
            say(memory.from, memory.text);
            return true;
        }
    }
    const ShipPilot* pilot = playerRegistry().tryGet<ShipPilot>(ship.entity);
    if (pilot == nullptr) {
        say("Comms", ship.name + " does not answer."); // an inert console spawn
        return false;
    }

    // ⚑⚑⚑ HAILING SOMEBODY WHO WORKS FOR YOU IS NOT A FIRST CONTACT (stage D).
    // The panel below opens negotiations with a stranger - their faction, your
    // standing with it, whether they will trade a tip - and every one of those
    // is the wrong question to ask an employee. Worse, it answered them off the
    // colours the hull was wearing, so your own captain reported a faction you
    // have no relationship with and an attitude toward yourself. A captain
    // answers as themselves, and that is the whole exchange.
    if (playerOwnedHull(playerRegistry(), ship.entity.index)) {
        const CaptainPuppet* puppet = playerRegistry().tryGet<CaptainPuppet>(ship.entity);
        const std::size_t who = puppet != nullptr ? puppet->captainIndex : m_captains.size();
        if (who < m_captains.size()) {
            say(m_captains[who].name, captainHailLine(who));
            return true;
        }
    }

    m_pendingHail = HailRequest{};
    m_pendingHail.pilot = ship.entity;
    m_pendingHail.name = ship.name;
    m_pendingHail.role = pilotRoleName(pilot->role);
    if (pilot->factionIndex < m_factionTable.size()) {
        m_pendingHail.factionName = m_factionTable[pilot->factionIndex].name;
        m_pendingHail.attitude = playerAttitudeName(pilot->factionIndex);
        m_pendingHail.standing = static_cast<double>(m_factionSim.standing(pilot->factionIndex));
        m_pendingHail.hostile = m_factionSim.playerHostile(pilot->factionIndex);
    } else {
        // The pre-8b rule an unaffiliated console spawn already lives under:
        // no faction means no reason to be friendly.
        m_pendingHail.attitude = "none";
        m_pendingHail.hostile = true;
    }
    // Whether there is anything of each kind left to say. The hook picks which
    // KIND of tip to offer and how it sounds; the engine picks which market or
    // which site, because a tip is a claim about the galaxy.
    std::vector<std::uint8_t> hops;
    hopsFrom(m_galaxy, m_currentSystem, kIntelJumpRadius, hops);
    std::uint32_t market = 0;
    m_pendingHail.canTipMarket =
        sim::chooseMarketTip(m_economy.markets(), hops, kIntelJumpRadius, m_survey, m_worldSeconds, &market);
    std::vector<sim::SignalSpec> scratch;
    sim::TipSite site;
    m_pendingHail.canTipPlace =
        sim::choosePlaceTip(m_galaxy, m_survey, hops, kIntelJumpRadius, scratch, &site);
    // Seeded per (universe, system, pilot, how many hails you have made), so
    // the answer is deterministic for a run while two pilots met in a row do
    // not read off the same script.
    core::Rng rng(m_universeSeed ^ (static_cast<std::uint64_t>(m_currentSystem) << 32u),
                  (static_cast<std::uint64_t>(ship.entity.index) << 20u) | ++m_hailCount);
    m_pendingHail.roll = static_cast<double>(rng.nextU32()) * 0x1.0p-32;
    say("You", "Hailing " + ship.name + ".");
    return true;
}

bool SpaceWorld::takeHailRequest(HailRequest& out)
{
    if (isNull(m_pendingHail.pilot)) {
        return false;
    }
    out = m_pendingHail;
    // The hook still gets the full name — it may want the faction to decide
    // what to say. Only the panel's sender column takes the callsign.
    m_answeringHail = HailMemory{.pilot = m_pendingHail.pilot, .from = radioName(m_pendingHail.name)};
    m_pendingHail = HailRequest{};
    return true;
}

void SpaceWorld::finishHail()
{
    m_answeringHail = HailMemory{};
}

bool SpaceWorld::replyHail(const std::string& message)
{
    if (!answeringHail()) {
        SOL_LOG_WARN("hail reply: only valid inside pilot_hail");
        return false;
    }
    m_answeringHail.text = message;
    say(m_answeringHail.from, message);
    // Recorded so a second hail repeats it, and cleared so a hook that calls
    // two builders only gets the first - the same "answer with exactly one"
    // rule dock_request holds its dispatcher to.
    m_hails.push_back(m_answeringHail);
    m_answeringHail = HailMemory{};
    return true;
}

bool SpaceWorld::tipMarket(const std::string& message)
{
    if (!answeringHail()) {
        SOL_LOG_WARN("hail tip: only valid inside pilot_hail");
        return false;
    }
    std::vector<std::uint8_t> hops;
    hopsFrom(m_galaxy, m_currentSystem, kIntelJumpRadius, hops);
    std::uint32_t market = 0;
    if (!sim::chooseMarketTip(
            m_economy.markets(), hops, kIntelJumpRadius, m_survey, m_worldSeconds, &market)) {
        // The hook was told canTipMarket was false and offered one anyway. Its
        // words were written on the premise of a fact, so they are dropped
        // rather than left pointing at nothing.
        return replyHail("Nothing out here you don't already know.");
    }
    const sim::StationMarket& record = m_economy.markets()[market];
    std::vector<float> prices(m_commodityIds.size(), 0.0f);
    for (std::uint32_t c = 0; c < prices.size(); ++c) {
        prices[c] = m_economy.price(market, c);
    }
    m_survey.recordMarket(market, prices, m_worldSeconds);
    // Where it is, appended by C++ for the reason above: the hook writes the
    // sentiment, the engine writes the fact. Kept terse because 8r's comms
    // panel clips, and the sender column is already spending a column.
    const sim::SystemSpec& spec = m_galaxy.systems[record.systemIndex];
    const std::string where = record.stationIndex < spec.stations.size()
                                  ? spec.stations[record.stationIndex].name + ", " + spec.name
                                  : spec.name;
    SOL_LOG_INFO("pilot tip: market %u (%s)", market, where.c_str());
    return replyHail(message + " " + where + ".");
}

bool SpaceWorld::tipPlace(const std::string& message)
{
    if (!answeringHail()) {
        SOL_LOG_WARN("hail tip: only valid inside pilot_hail");
        return false;
    }
    std::vector<std::uint8_t> hops;
    hopsFrom(m_galaxy, m_currentSystem, kIntelJumpRadius, hops);
    std::vector<sim::SignalSpec> scratch;
    sim::TipSite site;
    if (!sim::choosePlaceTip(m_galaxy, m_survey, hops, kIntelJumpRadius, scratch, &site)) {
        return replyHail("Nothing out here worth your time.");
    }
    // A short name on purpose: it lands in the nav cycle and the map's name
    // column, which 8i established does not clip. Who said it rides on the
    // comms line instead, where there is room for it.
    if (m_survey.addBookmark(site.system, site.position, "Rumour", sim::kTipLabel, m_worldSeconds) == 0) {
        return replyHail("You've got nowhere left to write that down."); // at the cap
    }
    if (site.system == m_currentSystem) {
        rebuildDynamicTargets(); // the radar blip, nav slot and marker, at once
    }
    const std::string where = m_galaxy.systems[site.system].name;
    SOL_LOG_INFO("pilot tip: site %u in %s", site.signal, where.c_str());
    return replyHail(message + " " + where + ".");
}

std::size_t SpaceWorld::berthTargetIndex() const
{
    for (std::size_t slot = 0; slot < m_dynamicTargets.size(); ++slot) {
        if (m_dynamicTargets[slot].kind == NavKind::Berth) {
            return m_signalTargetBase + slot;
        }
    }
    return kNoTarget;
}

void SpaceWorld::tickDocking(double dt)
{
    // Comms lines fade whatever else is happening — they are a readout, not
    // state, the same rule the collection ticker follows.
    for (CommsMessage& message : m_comms) {
        message.secondsLeft -= dt;
    }
    while (!m_comms.empty() && m_comms.front().secondsLeft <= 0.0) {
        m_comms.erase(m_comms.begin());
    }
    if (m_berthRefusalTimer > 0.0) {
        m_berthRefusalTimer -= dt;
    }
    if (isDocked() || m_currentSystem >= m_galaxy.systems.size()) {
        return;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const core::DVec3 position = shipState().position;
    const double speed = length(shipState().velocity);

    if (hasClearance()) {
        // Revoked the moment the owner turns on you. Derived from standing
        // rather than from a new event: firing on a patrol is what moves
        // standing, and standing crossing hostile is what a dispatcher would
        // actually react to.
        const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
        if (owner < m_factionTable.size() && m_factionSim.playerHostile(owner)) {
            clearClearance("Clearance revoked. Leave the approach lane.");
            return;
        }
        m_clearance.secondsLeft -= dt;
        if (m_clearance.secondsLeft <= 0.0) {
            clearClearance("Clearance expired. Hail us again.");
            return;
        }
        if (sim::inBerth(position, speed, clearedBerthPoint())) {
            const std::uint32_t station = m_clearance.station;
            const std::uint32_t berth = m_clearance.berth;
            completeDock(station, berth);
            say(spec.stations[station].name, "Docking clamps engaged. Welcome aboard.");
        }
        return;
    }

    // No clearance: sitting in somebody's berth is refused in words rather
    // than silently doing nothing, which is the whole complaint this item
    // started from (a refusal that only ever reached the console log).
    if (m_berthRefusalTimer > 0.0) {
        return;
    }
    for (std::uint32_t i = 0; i < spec.stations.size(); ++i) {
        for (std::uint32_t berth = 0; berth < sim::kBerthCount; ++berth) {
            if (!sim::inBerth(position, speed, sim::berthPoint(spec.stations[i].position, berth))) {
                continue;
            }
            say(spec.stations[i].name,
                "Berth " + std::to_string(berth + 1) + " is not yours. Hail us or stand off.");
            m_berthRefusalTimer = kCommsMessageSeconds;
            return;
        }
    }
}

std::uint32_t SpaceWorld::commodityIndex(const char* id) const
{
    for (std::uint32_t i = 0; i < m_commodityIds.size(); ++i) {
        if (m_commodityIds[i] == id) {
            return i;
        }
    }
    return kNoIndex;
}

float SpaceWorld::playerCargoTotal() const
{
    float total = 0.0f;
    for (const float units : m_playerCargo) {
        total += units;
    }
    return total;
}

std::uint32_t SpaceWorld::dockedMarket() const
{
    return isDocked() ? m_economy.marketFor(m_currentSystem, m_dockedStation) : kNoIndex;
}

sim::TradeResult SpaceWorld::playerBuy(std::uint32_t commodity, float units)
{
    const std::uint32_t market = dockedMarket();
    if (market >= m_economy.markets().size() || commodity >= m_playerCargo.size() || units <= 0.0f) {
        return {};
    }
    units = std::min(units, m_playerCargoCapacity - playerCargoTotal());
    // ⚑⚑⚑⚑ THE PURSE CLAMP ASKS THE ECONOMY NOW, AND FROM PHASE 8 UNTIL PHASE 37
    // IT DID NOT. It read `m_playerCredits / m_economy.price(...)` - the
    // pre-trade MARGINAL price, with no spread and no allowance for the price
    // rising as the trade fills - and then paid `Economy::buy`'s real total,
    // unchecked. Both errors point the same way, so the count was always too
    // high and `m_playerCredits` could land BELOW ZERO.
    //
    // ⚑⚑⚑⚑ AND IT WAS NEVER CONTRABAND-SPECIFIC, WHICH IS WHAT THE GUARD FOUND
    // AND THE FIRST TELLING OF THIS GOT WRONG. It was found buying stims and the
    // obvious story - "the first cargo dear enough to reach the boundary" - is
    // FALSE. With the hold empty and the 100 button pressed, a new pilot's 1000
    // cr overdraws on **Refined Metal at 40 cr** (-63), Machinery (-58),
    // Structural Alloy (-61), Hull Plate (-57) and Hull Section (-52) as well as
    // on stims (-69). Every one of those is reachable at the first dock of a new
    // game.
    //
    // ⚑⚑⚑ SO WHAT KEPT IT HIDDEN FOR TWENTY-NINE PHASES WAS NOT THE PRICES, IT
    // WAS THAT NOBODY LOOKED. `playerBuy` had no test in `game/test` at all, and
    // a player pressing 100 on a cheap good with a nearly-full purse sees a
    // number go slightly negative and reads it as their own arithmetic.
    // Contraband made it CONSPICUOUS - dear enough that even the 10 button does
    // it - rather than possible. *An error found at the edge of new content is
    // not necessarily an error that new content created.*
    //
    // ⚑⚑ THE CLAMP CANNOT BE DONE HERE. The cost of `u` units is quadratic in
    // `u` because the price moves as the trade fills, so only the layer that
    // owns the curve can invert it - which is why this is `unitsWithin` in
    // `sol::sim` rather than a better division in this file.
    units = m_economy.unitsWithin(market, commodity, units, m_playerCredits);
    const sim::TradeResult result = m_economy.buy(market, commodity, units);
    m_playerCredits -= result.credits;
    m_playerCargo[commodity] += result.units;
    recordPlayerTrade(commodity, result.credits);
    return result;
}

sim::TradeResult SpaceWorld::playerSell(std::uint32_t commodity, float units)
{
    const std::uint32_t market = dockedMarket();
    if (market >= m_economy.markets().size() || commodity >= m_playerCargo.size() || units <= 0.0f) {
        return {};
    }
    units = std::min(units, m_playerCargo[commodity]);
    const sim::TradeResult result = m_economy.sell(market, commodity, units);
    m_playerCredits += result.credits;
    m_playerCargo[commodity] -= result.units;
    recordPlayerTrade(commodity, result.credits);
    return result;
}

double SpaceWorld::nearestStationDistance() const
{
    if (m_currentSystem >= m_galaxy.systems.size()) {
        return -1.0;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (spec.stations.empty()) {
        return -1.0;
    }
    const core::DVec3 playerPosition =
        playerRegistry().storage<Transform>().get(playerEntityIndex()).position;
    double nearest = 1.0e30;
    for (const sim::StationSpec& station : spec.stations) {
        nearest = std::min(nearest, length(station.position - playerPosition));
    }
    return nearest;
}

bool SpaceWorld::jumpToSystem(const char* destinationName)
{
    if (isDocked()) {
        return false;
    }
    for (const GateInstance& gate : m_gates) {
        if (m_galaxy.systems[gate.toSystem].name == destinationName) {
            SOL_LOG_INFO("teleport: %s -> %s (no transition)", currentSystemName(), destinationName);
            // A teleport abandons any transition in flight rather than landing
            // on top of it, so the two paths can never both own a destination.
            m_jump.clear();
            loadSystem(gate.toSystem, m_currentSystem);
            return true;
        }
    }
    return false;
}

double SpaceWorld::nearestGateDistance() const
{
    const GateInstance* gate = nearestGate();
    if (gate == nullptr) {
        return -1.0;
    }
    const core::DVec3 playerPosition =
        playerRegistry().storage<Transform>().get(playerEntityIndex()).position;
    return length(gate->position - playerPosition);
}

const GateInstance* SpaceWorld::nearestGate() const
{
    if (m_gates.empty()) {
        return nullptr;
    }
    const core::DVec3 playerPosition =
        playerRegistry().storage<Transform>().get(playerEntityIndex()).position;
    const GateInstance* nearest = nullptr;
    double nearestDistance = 1.0e30;
    for (const GateInstance& gate : m_gates) {
        const double distance = length(gate.position - playerPosition);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = &gate;
        }
    }
    return nearest;
}

void SpaceWorld::playerAddPip(sim::PowerSystem system)
{
    ShipPower& power = playerRegistry().storage<ShipPower>().get(playerEntityIndex());
    sim::addPip(power.state.pips, system, power.tuning);
}

void SpaceWorld::playerBalancePips()
{
    ShipPower& power = playerRegistry().storage<ShipPower>().get(playerEntityIndex());
    sim::balancePips(power.state.pips, power.tuning);
}

void SpaceWorld::applyDefs(const assets::DefDatabase& defs)
{
    m_defs = &defs;
    if (!m_fleet.empty()) {
        applyActiveLoadout();
    } else if (const assets::ShipDef* playerDef = defs.findShip(kPlayerShipDefId)) {
        // Pre-universe (fleet not initialized yet): raw starter def.
        applyShipDef(playerRegistry(), playerEntityIndex(), *playerDef, defs);
        applyCockpitOf(*playerDef);
    } else {
        SOL_LOG_WARN("player ship def '%s' missing; keeping current tuning", kPlayerShipDefId);
    }

    for (const SpawnedShip& spawned : playerShips()) {
        if (const assets::ShipDef* def = defs.findShip(spawned.defId.c_str())) {
            applyShipDef(playerRegistry(), spawned.entity.index, *def, defs);
        }
    }
}

// --- Outfitting & fleet (Phase 8a) ---

namespace {

// The saved fit resolved against the hull's mounts (Phase 31 stage B). The
// MOUNT decides which table an id is looked up in - `mountTakesWeapon` - so a
// component id and a weapon id can never be confused for one another even if
// somebody ships both under one name.
//
// A fitting naming a mount this hull does not have is DROPPED with a warning
// rather than carried: the hull def changed under a save (a mod uninstalled,
// an author renamed a mount), and there is nowhere to put it. Carrying it
// would fail validation forever and make the ship unrefittable.
[[nodiscard]] std::vector<assets::FittedMount>
fitMounts(const assets::DefDatabase& defs, const assets::ShipDef& base, const OwnedShip& ship)
{
    std::vector<assets::FittedMount> fittings;
    fittings.reserve(ship.fittings.size());
    for (const ShipFitting& fitted : ship.fittings) {
        const assets::ShipMount* mount = base.findMount(fitted.mountId);
        if (mount == nullptr) {
            SOL_LOG_WARN("fit: '%s' has no mount '%s'; dropping '%s'",
                         base.id.c_str(),
                         fitted.mountId.c_str(),
                         fitted.defId.c_str());
            continue;
        }
        assets::FittedMount entry{.mountId = fitted.mountId};
        if (assets::mountTakesWeapon(mount->kind)) {
            entry.weapon = defs.findWeapon(fitted.defId.c_str());
        } else {
            entry.component = defs.findComponent(fitted.defId.c_str());
        }
        if (entry.empty()) {
            SOL_LOG_WARN("fit: def '%s' missing; mount '%s' reads empty",
                         fitted.defId.c_str(),
                         fitted.mountId.c_str());
        }
        fittings.push_back(entry);
    }
    return fittings;
}

// What a hull comes with: every mount whose def names a `fit`.
[[nodiscard]] std::vector<ShipFitting> defaultFit(const assets::ShipDef& def)
{
    std::vector<ShipFitting> fittings;
    for (const assets::ShipMount& mount : def.mounts) {
        if (!mount.fit.empty()) {
            fittings.push_back({.mountId = mount.id, .defId = mount.fit});
        }
    }
    return fittings;
}

[[nodiscard]] std::vector<const assets::CrewDef*> fitCrew(const assets::DefDatabase& defs,
                                                          const OwnedShip& ship)
{
    std::vector<const assets::CrewDef*> crew;
    crew.reserve(ship.crewIds.size());
    for (const std::string& id : ship.crewIds) {
        const assets::CrewDef* member = defs.findCrew(id.c_str());
        if (member == nullptr) {
            SOL_LOG_WARN("fit: crew def '%s' missing; ignoring", id.c_str());
        }
        crew.push_back(member);
    }
    return crew;
}

} // namespace

void SpaceWorld::resetFleetToStarter()
{
    m_fleet.clear();
    m_activeShip = 0;
    OwnedShip starter{.defId = kPlayerShipDefId};
    if (m_defs != nullptr) {
        if (const assets::ShipDef* def = m_defs->findShip(kPlayerShipDefId)) {
            starter.fittings = defaultFit(*def);
        }
    }
    m_fleet.push_back(std::move(starter));
}

assets::ShipDef SpaceWorld::resolvedShipDef(const OwnedShip& ship) const
{
    if (m_defs == nullptr) {
        return assets::ShipDef{.id = ship.defId};
    }
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        SOL_LOG_WARN("fleet: ship def '%s' missing; using defaults", ship.defId.c_str());
        return assets::ShipDef{.id = ship.defId};
    }
    const std::vector<assets::FittedMount> fittings = fitMounts(*m_defs, *base, ship);
    const std::vector<const assets::CrewDef*> crew = fitCrew(*m_defs, ship);
    return assets::resolveLoadout(*base, fittings, crew);
}

void SpaceWorld::applyActiveLoadout()
{
    if (m_defs == nullptr || m_fleet.empty()) {
        return;
    }
    const assets::ShipDef def = resolvedShipDef(activeShip());
    applyShipDef(playerRegistry(), playerEntityIndex(), def, *m_defs);
    // ⚑ ONLY THE PLAYER'S SHIP GETS THIS CALL, and this is the only place it
    // is made (Phase 31 stage C3). The resolved def carries WHICH gun is in
    // WHICH mount; the saved fit carries which trigger the pilot wired it to,
    // and it is deliberately not routed through the def - a mount is a place on
    // a hull, and a hull has no opinion about triggers.
    applyPilotFireGroups(playerEntityIndex(), def, activeShip());
    applyCockpitOf(def);
}

void SpaceWorld::applyPilotFireGroups(std::uint32_t entityIndex,
                                      const assets::ShipDef& def,
                                      const OwnedShip& ship)
{
    ShipArmament* armament = playerRegistry().storage<ShipArmament>().tryGet(entityIndex);
    if (armament == nullptr) {
        return;
    }
    for (std::uint32_t i = 0; i < armament->count; ++i) {
        ShipWeapon& weapon = armament->weapons[i];
        if (weapon.mount >= def.mounts.size()) {
            continue;
        }
        const ShipFitting* fitting = ship.fittingAt(def.mounts[weapon.mount].id);
        if (fitting == nullptr || fitting->group < 1 || fitting->group > kFireGroupCount) {
            continue;
        }
        weapon.group = fitting->group;
    }
    // A hull whose every gun sits in group 2 is a perfectly ordinary thing to
    // fly, and a selection left at 1 would be a trigger wired to nothing.
    normalizeFireGroup(*armament);
}

std::uint32_t SpaceWorld::playerFireGroup() const
{
    const ShipArmament* armament = playerRegistry().storage<ShipArmament>().tryGet(playerEntityIndex());
    return armament != nullptr ? armament->selectedGroup : 1;
}

std::uint32_t SpaceWorld::playerFireGroupsInUse() const
{
    const ShipArmament* armament = playerRegistry().storage<ShipArmament>().tryGet(playerEntityIndex());
    return armament != nullptr ? fireGroupsInUse(*armament) : 0;
}

// ⚑ THE CYCLE VISITS ONLY GROUPS THAT HAVE A GUN IN THEM, which is what keeps
// one key usable on a hull carrying two guns in groups 1 and 4: the player
// steps between the two things they set up rather than through two empty
// positions that do nothing and say nothing. It is also why there is no
// "select group N" binding - four more rows in the Controls screen to reach
// four positions a single key already reaches in order.
std::uint32_t SpaceWorld::cycleFireGroup()
{
    ShipArmament* armament = playerRegistry().storage<ShipArmament>().tryGet(playerEntityIndex());
    if (armament == nullptr) {
        return 1;
    }
    const std::uint32_t mask = fireGroupsInUse(*armament);
    if (mask == 0) {
        return armament->selectedGroup;
    }
    normalizeFireGroup(*armament);
    for (std::uint32_t step = 1; step <= kFireGroupCount; ++step) {
        const std::uint32_t candidate = ((armament->selectedGroup - 1 + step) % kFireGroupCount) + 1;
        if ((mask & (1u << (candidate - 1))) != 0) {
            armament->selectedGroup = candidate;
            break;
        }
    }
    return armament->selectedGroup;
}

// ⚑ BOTH COPIES, AND NEITHER IS OPTIONAL. The saved fit is what survives a
// refit, a ship swap and a reload; the live gun is what the firing pass reads
// this tick. Doing it by rebuilding the armament from the def - the obvious
// one-line version - would run `applyShipDef`, which resets the DEFENCES to
// full and clears every cooldown: a free heal and a free salvo, every time the
// player changed which trigger a gun answers to.
bool SpaceWorld::setFireGroup(const char* mountId, std::uint32_t group, std::string* outError)
{
    if (m_fleet.empty() || m_defs == nullptr) {
        return refuse("no active ship", outError);
    }
    if (mountId == nullptr || mountId[0] == '\0') {
        return refuse("no mount named", outError);
    }
    if (group < 1 || group > kFireGroupCount) {
        return refuse("fire group must be 1.." + std::to_string(kFireGroupCount), outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        return refuse("active ship def '" + ship.defId + "' missing", outError);
    }
    if (base->findMount(mountId) == nullptr) {
        return refuse("'" + base->name + "' has no mount '" + mountId + "'", outError);
    }
    ShipArmament* armament = playerRegistry().storage<ShipArmament>().tryGet(playerEntityIndex());
    ShipWeapon* gun = nullptr;
    if (armament != nullptr) {
        for (std::uint32_t i = 0; i < armament->count; ++i) {
            ShipWeapon& weapon = armament->weapons[i];
            if (weapon.mount < base->mounts.size() && base->mounts[weapon.mount].id == mountId) {
                gun = &weapon;
                break;
            }
        }
    }
    if (gun == nullptr) {
        // Said in terms of the gun rather than the mount: a mount holding a
        // cargo pod is not a mistake, it is simply not something a trigger can
        // be wired to, and "mount is empty" would be wrong about half of them.
        return refuse(std::string("mount '") + mountId + "' carries no gun", outError);
    }
    gun->group = group;
    for (ShipFitting& fitting : ship.fittings) {
        if (fitting.mountId == mountId) {
            fitting.group = group;
        }
    }
    normalizeFireGroup(*armament);
    return true;
}

// Phase 19: the seat belongs to the ship, so it is resolved wherever a def is
// applied to the PLAYER's entity - which is two places, not one. Missing the
// second is a defect a test found: `applyDefs` has a pre-universe branch for
// an empty fleet that applies the starter def directly, and on a fresh boot
// that is the only one of the two that runs. Not under the unit-radius
// contract: a cockpit is authored at its real size and drawn at scale 1.
void SpaceWorld::applyCockpitOf(const assets::ShipDef& def)
{
    if (m_defs == nullptr) {
        return;
    }
    m_cockpitModel = modelOverrideOr(*m_defs, def.cockpit, "ship def", kRoleCockpit, false);
}

ModelId SpaceWorld::cockpitModel() const
{
    // Before any loadout has been applied - a fresh world, or a test holding
    // no fleet - the role is still the right answer.
    return m_cockpitModel == kNoModel ? roleModel(kRoleCockpit) : m_cockpitModel;
}

bool SpaceWorld::refuse(const std::string& reason, std::string* outError) const
{
    SOL_LOG_WARN("outfitting: %s", reason.c_str());
    if (outError != nullptr) {
        *outError = reason;
    }
    return false;
}

double SpaceWorld::shipValue(const OwnedShip& ship) const
{
    if (m_defs == nullptr) {
        return 0.0;
    }
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        return 0.0;
    }
    double value = base->price;
    for (const assets::FittedMount& fitting : fitMounts(*m_defs, *base, ship)) {
        if (fitting.component != nullptr) {
            value += fitting.component->price;
        } else if (fitting.weapon != nullptr) {
            value += fitting.weapon->price;
        }
    }
    return value;
}

// --- Fitting, and the one place the mount rules are enforced --------------

const ShipFitting* OwnedShip::fittingAt(std::string_view mountId) const
{
    for (const ShipFitting& fitting : fittings) {
        if (fitting.mountId == mountId) {
            return &fitting;
        }
    }
    return nullptr;
}

namespace {

// A def id resolved to whichever table holds it, with its mount vocabulary.
// Components are searched first, and the ambiguity is documented rather than
// designed away: nothing shipped collides, and the FIT path never comes
// through here - it asks the mount which table to look in.
struct CatalogItem
{
    const assets::ComponentDef* component = nullptr;
    const assets::WeaponDef* weapon = nullptr;

    [[nodiscard]] bool found() const { return component != nullptr || weapon != nullptr; }

    [[nodiscard]] assets::MountKind mount() const
    {
        return component != nullptr ? component->mount : weapon->mount;
    }

    [[nodiscard]] assets::MountSize size() const
    {
        return component != nullptr ? component->size : weapon->size;
    }

    [[nodiscard]] const std::string& name() const
    {
        return component != nullptr ? component->name : weapon->name;
    }

    [[nodiscard]] const std::string& id() const { return component != nullptr ? component->id : weapon->id; }

    [[nodiscard]] float price() const { return component != nullptr ? component->price : weapon->price; }

    [[nodiscard]] const assets::CatalogGate& gate() const
    {
        return component != nullptr ? component->gate : weapon->gate;
    }
};

[[nodiscard]] CatalogItem findFitting(const assets::DefDatabase& defs, const char* defId)
{
    CatalogItem item;
    item.component = defs.findComponent(defId);
    if (item.component == nullptr) {
        item.weapon = defs.findWeapon(defId);
    }
    return item;
}

} // namespace

std::string SpaceWorld::firstFreeMountFor(const char* defId) const
{
    if (m_defs == nullptr || m_fleet.empty() || defId == nullptr) {
        return {};
    }
    const OwnedShip& ship = activeShip();
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    const CatalogItem item = findFitting(*m_defs, defId);
    if (base == nullptr || !item.found()) {
        return {};
    }
    for (const assets::ShipMount& mount : base->mounts) {
        if (ship.fittingAt(mount.id) == nullptr && assets::mountAccepts(mount, item.mount(), item.size())) {
            return mount.id;
        }
    }
    return {};
}

bool SpaceWorld::buyFitting(const char* defId, const char* mountId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr || m_fleet.empty()) {
        return refuse("must be docked to refit", outError);
    }
    if (defId == nullptr) {
        return refuse("no fitting named", outError);
    }
    const CatalogItem item = findFitting(*m_defs, defId);
    if (!item.found()) {
        return refuse(std::string("no component or weapon def '") + defId + "'", outError);
    }
    if (!stationSells(item.gate())) {
        return refuse("'" + item.name() + "' is not sold here (faction catalog)", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        return refuse("active ship def '" + ship.defId + "' missing", outError);
    }

    // The mount is chosen before anything is charged, and an empty `mountId`
    // is a REQUEST rather than a place: "put it wherever it goes" is what a
    // catalog Buy button means, and a hull with no free place for it has to
    // say so in different words than a named mount that is merely full.
    const bool named = mountId != nullptr && mountId[0] != '\0';
    const std::string target = named ? std::string(mountId) : firstFreeMountFor(defId);
    if (target.empty()) {
        return refuse("no free " + std::string(assets::mountKindName(item.mount())) + " mount on the " +
                          base->name + " takes '" + item.name() + "'",
                      outError);
    }
    if (base->findMount(target) == nullptr) {
        return refuse("'" + base->name + "' has no mount '" + target + "'", outError);
    }

    // Swapping sells the old fitting back in the same transaction, which is
    // what the one weapon mount always did and is now every mount's rule.
    double resale = 0.0;
    OwnedShip candidate = ship;
    if (const ShipFitting* occupied = candidate.fittingAt(target); occupied != nullptr) {
        if (occupied->defId == item.id()) {
            return refuse("'" + item.name() + "' is already in mount '" + target + "'", outError);
        }
        const CatalogItem old = findFitting(*m_defs, occupied->defId.c_str());
        if (old.found()) {
            resale = kResaleRate * static_cast<double>(old.price());
        }
        candidate.fittings.erase(candidate.fittings.begin() + (occupied - candidate.fittings.data()));
    }
    candidate.fittings.push_back({.mountId = target, .defId = item.id()});

    std::string reason;
    if (!assets::validateLoadout(
            *base, fitMounts(*m_defs, *base, candidate), fitCrew(*m_defs, candidate), &reason)) {
        return refuse(reason, outError);
    }
    if (m_playerCredits + resale < static_cast<double>(item.price())) {
        return refuse("insufficient credits", outError);
    }
    // A swap that shrinks the hold must not strand cargo - the guard removing
    // a component always had, which a swap can now trip too.
    if (resolvedShipDef(candidate).cargoCapacity < playerCargoTotal()) {
        return refuse("cargo hold would overflow; sell cargo first", outError);
    }

    m_playerCredits += resale - static_cast<double>(item.price());
    ship = std::move(candidate);
    applyActiveLoadout();
    SOL_LOG_INFO("fitted '%s' to '%s' (net %.0f cr)",
                 item.name().c_str(),
                 target.c_str(),
                 resale - static_cast<double>(item.price()));
    return true;
}

bool SpaceWorld::sellFitting(const char* mountId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr || m_fleet.empty()) {
        return refuse("must be docked to refit", outError);
    }
    if (mountId == nullptr) {
        return refuse("no mount named", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const ShipFitting* fitted = ship.fittingAt(mountId);
    if (fitted == nullptr) {
        return refuse(std::string("mount '") + mountId + "' is empty", outError);
    }
    const std::string removedId = fitted->defId;
    OwnedShip candidate = ship;
    candidate.fittings.erase(candidate.fittings.begin() + (fitted - ship.fittings.data()));
    if (resolvedShipDef(candidate).cargoCapacity < playerCargoTotal()) {
        return refuse("cargo hold would overflow; sell cargo first", outError);
    }
    double refund = 0.0;
    if (const CatalogItem item = findFitting(*m_defs, removedId.c_str()); item.found()) {
        refund = kResaleRate * static_cast<double>(item.price());
    }
    ship = std::move(candidate);
    m_playerCredits += refund;
    applyActiveLoadout();
    SOL_LOG_INFO("removed '%s' from '%s' (+%.0f cr)", removedId.c_str(), mountId, refund);
    return true;
}

bool SpaceWorld::buyShip(const char* shipDefId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr) {
        return refuse("must be docked to buy ships", outError);
    }
    const assets::ShipDef* def = m_defs->findShip(shipDefId);
    if (def == nullptr) {
        return refuse(std::string("no ship def '") + shipDefId + "'", outError);
    }
    if (!stationSells(def->gate)) {
        return refuse("'" + def->name + "' is not sold here (faction catalog)", outError);
    }
    if (m_playerCredits < def->price) {
        return refuse("insufficient credits", outError);
    }
    m_playerCredits -= def->price;
    m_fleet.push_back(OwnedShip{.defId = def->id,
                                .fittings = defaultFit(*def),
                                .storedSystem = m_currentSystem,
                                .storedStation = m_dockedStation});
    SOL_LOG_INFO("bought '%s' (-%.0f cr); stored at %s",
                 def->name.c_str(),
                 static_cast<double>(def->price),
                 dockedStationName());
    return true;
}

bool SpaceWorld::sellShip(std::size_t fleetIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to sell ships", outError);
    }
    if (fleetIndex >= m_fleet.size() || fleetIndex == m_activeShip) {
        return refuse("can only sell a stored ship", outError);
    }
    const OwnedShip& ship = m_fleet[fleetIndex];
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("that ship is stored elsewhere", outError);
    }
    // ⚑⚑⚑⚑ A HULL SOMEBODY IS FLYING IS NOT YOURS TO SELL (Phase 39
    // stage A). In this stage it is only tidiness; from stage B on the hull is
    // in another system on a route, and selling it out from under its captain
    // would delete a ship that is mid-haul with no death path and no wreck -
    // the same shape as the eviction hazard the phase spec's risk register
    // names. Refused with a reason rather than silently unassigning.
    if (const Captain* holder = captainOf(fleetIndex); holder != nullptr) {
        return refuse("'" + holder->name + "' is flying that ship - recall them first", outError);
    }
    const double refund = kResaleRate * shipValue(ship);
    SOL_LOG_INFO("sold '%s' (+%.0f cr)", ship.defId.c_str(), refund);
    m_playerCredits += refund;
    // ⚑⚑⚑ AND EVERY CAPTAIN'S INDEX SHIFTS WITH IT, exactly as `m_activeShip`
    // does. `Captain::ship` is a fleet index and an erase renumbers the tail;
    // without this a captain silently inherits the hull that moved into the
    // slot. ⚑ The shift moved into `removeFleetShip` at stage D, which gave it
    // a second caller: a captain's hull can now be deleted by DYING as well as
    // by being sold, and two copies of an index shift is how the two stop
    // agreeing about the tail.
    removeFleetShip(fleetIndex);
    return true;
}

bool SpaceWorld::switchShip(std::size_t fleetIndex, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr) {
        return refuse("must be docked to switch ships", outError);
    }
    if (fleetIndex >= m_fleet.size() || fleetIndex == m_activeShip) {
        return refuse("pick a stored ship", outError);
    }
    OwnedShip& target = m_fleet[fleetIndex];
    if (target.storedSystem != m_currentSystem || target.storedStation != m_dockedStation) {
        return refuse("that ship is stored elsewhere", outError);
    }
    if (resolvedShipDef(target).cargoCapacity < playerCargoTotal()) {
        return refuse("cargo would not fit that ship's hold", outError);
    }
    // ⚑⚑⚑⚑ AND NOR IS A HULL SOMEBODY IS FLYING YOURS TO CLIMB INTO - THE HOLE
    // STAGE A LEFT, FOUND BY WRITING STAGE B'S LOADER. `sellShip` grew this
    // guard and `switchShip` beside it did not, and the two are not the same
    // severity: taking the active seat in a captain's hull sets
    // `captain.ship == m_activeShip`, which is a state the v39 LOADER REFUSES
    // OUTRIGHT (`captain.ship == activeIndex` returns false). So the door was
    // not "a captain and the player share a hull" - it was a save file the game
    // writes happily and then declines to open, with nothing at either end
    // saying why. Stage A's own words, applied where they were missed: one
    // action does one thing, and recalling them first is that action.
    if (const Captain* holder = captainOf(fleetIndex); holder != nullptr) {
        return refuse("'" + holder->name + "' is flying that ship - recall them first", outError);
    }
    OwnedShip& current = m_fleet[m_activeShip];
    current.storedSystem = m_currentSystem;
    current.storedStation = m_dockedStation;
    target.storedSystem = kNoIndex;
    target.storedStation = kNoIndex;
    m_activeShip = fleetIndex;
    applyActiveLoadout();
    SOL_LOG_INFO("now flying '%s'", m_fleet[m_activeShip].defId.c_str());
    return true;
}

bool SpaceWorld::hireCrew(const char* crewId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr || m_fleet.empty()) {
        return refuse("must be docked to hire crew", outError);
    }
    const assets::CrewDef* member = m_defs->findCrew(crewId);
    if (member == nullptr) {
        return refuse(std::string("no crew def '") + crewId + "'", outError);
    }
    if (!stationSells(member->gate)) {
        return refuse("'" + member->name + "' is not for hire here (faction catalog)", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        return refuse("active ship def '" + ship.defId + "' missing", outError);
    }
    std::vector<const assets::CrewDef*> crew = fitCrew(*m_defs, ship);
    crew.push_back(member);
    std::string reason;
    if (!assets::validateLoadout(*base, fitMounts(*m_defs, *base, ship), crew, &reason)) {
        return refuse(reason, outError);
    }
    if (m_playerCredits < member->price) {
        return refuse("insufficient credits", outError);
    }
    m_playerCredits -= member->price;
    ship.crewIds.push_back(member->id);
    applyActiveLoadout();
    SOL_LOG_INFO("hired %s '%s' (-%.0f cr)",
                 member->role.c_str(),
                 member->name.c_str(),
                 static_cast<double>(member->price));
    return true;
}

bool SpaceWorld::fireCrew(const char* crewId, std::string* outError)
{
    if (!isDocked() || m_fleet.empty()) {
        return refuse("must be docked to dismiss crew", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const auto it = std::find(ship.crewIds.begin(), ship.crewIds.end(), crewId);
    if (it == ship.crewIds.end()) {
        return refuse(std::string("crew '") + crewId + "' is not aboard", outError);
    }
    ship.crewIds.erase(it); // hires are one-time fees: no refund
    applyActiveLoadout();
    SOL_LOG_INFO("dismissed '%s'", crewId);
    return true;
}

// --- Captains (Phase 39 stage A) --------------------------------------------

const Captain* SpaceWorld::captainOf(std::size_t fleetIndex) const
{
    for (const Captain& captain : m_captains) {
        if (captain.ship == static_cast<std::uint32_t>(fleetIndex)) {
            return &captain;
        }
    }
    return nullptr;
}

void SpaceWorld::captainCandidates(std::vector<CaptainCandidate>& out) const
{
    out.clear();
    if (!isDocked()) {
        return;
    }
    const std::uint32_t crewBit = 1u << static_cast<std::uint32_t>(assets::StationScreen::Crew);
    if ((dockedStationScreens() & crewBit) == 0) {
        return; // no crew hall on this dock: nobody is looking for a berth here
    }

    // Derived from the seed and never saved, exactly as the cast's seating is.
    // The system and the station are folded into the SEED rather than drawn
    // from one galaxy-wide stream, so a hall's roster is a pure function of the
    // dock you are standing on - which is what lets a test ask a specific hall
    // the same question twice and lets a player remember where the cheap
    // captain was.
    core::Rng rng(m_universeSeed ^ (static_cast<std::uint64_t>(m_currentSystem) << 32u) ^
                      static_cast<std::uint64_t>(m_dockedStation),
                  kCaptainStream);

    char id[64] = {};
    for (std::uint32_t slot = 0; slot < kCaptainsPerHall; ++slot) {
        // ⚑⚑⚑ ALL FOUR DRAWS ARE TAKEN BEFORE ANYTHING IS SKIPPED, and it
        // is `assignCast`'s rule restated: drawing only for slots that survive
        // the filter would make the stream depend on WHO the player has
        // already hired, so hiring the first captain in a hall would re-roll
        // the two standing beside him. Two wasted draws buy a roster whose
        // contents are a function of the dock and nothing else.
        const std::uint32_t given = rng.range(static_cast<std::uint32_t>(std::size(kGivenNames)));
        const std::uint32_t family = rng.range(static_cast<std::uint32_t>(std::size(kFamilyNames)));
        const std::uint32_t trade = rng.range(static_cast<std::uint32_t>(std::size(kCaptainTrades)));
        const float cut = rng.rangeFloat(kCaptainCutMin, kCaptainCutMax);

        std::snprintf(id, sizeof(id), "cap:%u:%u:%u", m_currentSystem, m_dockedStation, slot);
        const std::uint64_t who = castKeyForCharacter(id);
        // Already in your employ: they are not standing in the hall. A captain
        // you DISMISS falls out of `m_captains` and is therefore on offer
        // again, which is honest and costs no storage.
        const bool hired = std::any_of(
            m_captains.begin(), m_captains.end(), [who](const Captain& c) { return c.who == who; });
        if (hired) {
            continue;
        }
        // ⚑⚑⚑⚑ AND DEAD IS NOT THE SAME AS DISMISSED, WHICH THE PHASE EXIT
        // FOUND BY WALKING BACK INTO THE HALL. A roster is a pure function of
        // the dock, and `killCaptain` erases the person from `m_captains` - so
        // the slot that produced them simply produced them again, and a captain
        // the game had just announced as "lost with all hands" was standing in
        // the same crew hall twenty minutes later asking for a berth, at the
        // same cut. That refutes ruling 14 (*"the hull is gone and so is the
        // person"*) as a fact the player can see, and it refutes it in the one
        // place the ruling is supposed to bite: `killCaptain`'s own comment
        // says a captain "is a name that was drawn once", and the name is drawn
        // from a seed that does not know they are dead.
        //
        // ⚑⚑ THE DEAD ARE THEREFORE KEPT AND THE DISMISSED ARE NOT, and the
        // asymmetry is the whole point rather than an inconsistency: dismissal
        // is a door the player can walk back through, and death is the one
        // consequence in this game that flying back cannot undo. It costs one
        // 64-bit key per captain who dies - `CastMemory`'s bargain exactly, a
        // sparse record of what HAPPENED beside a roster derived from the seed.
        if (std::find(m_lostCaptains.begin(), m_lostCaptains.end(), who) != m_lostCaptains.end()) {
            continue;
        }
        out.push_back({.name = std::string(kGivenNames[given]) + " " + kFamilyNames[family],
                       .trade = kCaptainTrades[trade],
                       .who = who,
                       .cut = cut});
    }
}

bool SpaceWorld::hireCaptain(std::size_t candidateIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to hire a captain", outError);
    }
    std::vector<CaptainCandidate> hall;
    captainCandidates(hall);
    if (hall.empty()) {
        return refuse("no crew hall here - nobody is looking for a berth", outError);
    }
    if (candidateIndex >= hall.size()) {
        return refuse("no such candidate", outError);
    }
    const CaptainCandidate& pick = hall[candidateIndex];
    m_captains.push_back({.name = pick.name, .trade = pick.trade, .who = pick.who, .cut = pick.cut});
    SOL_LOG_INFO("hired %s (%s) at %s, %.0f%% of takings",
                 pick.name.c_str(),
                 pick.trade.c_str(),
                 dockedStationName(),
                 static_cast<double>(pick.cut) * 100.0);
    return true;
}

bool SpaceWorld::dismissCaptain(std::size_t captainIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to dismiss a captain", outError);
    }
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    const Captain& captain = m_captains[captainIndex];
    if (captain.ship != kNoIndex) {
        return refuse("'" + captain.name + "' is holding a ship - recall them first", outError);
    }
    SOL_LOG_INFO("dismissed captain %s", captain.name.c_str());
    m_captains.erase(m_captains.begin() + static_cast<std::ptrdiff_t>(captainIndex));
    return true;
}

bool SpaceWorld::assignCaptain(std::size_t captainIndex, std::size_t fleetIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to give a captain a ship", outError);
    }
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    if (fleetIndex >= m_fleet.size()) {
        return refuse("no such ship", outError);
    }
    if (fleetIndex == m_activeShip) {
        return refuse("that is the ship you are flying", outError);
    }
    const OwnedShip& ship = m_fleet[fleetIndex];
    // ⚑⚑ THE SAME STATION, NOT MERELY THE SAME SYSTEM - `sellShip`'s and
    // `switchShip`'s rule, and here it earns itself twice over: a captain has
    // to physically take the ship, so both of you have to be standing on the
    // dock it is parked at.
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("that ship is stored elsewhere", outError);
    }
    if (const Captain* holder = captainOf(fleetIndex); holder != nullptr) {
        return refuse("'" + holder->name + "' already has that ship", outError);
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.ship != kNoIndex) {
        return refuse("'" + captain.name + "' already has a ship", outError);
    }
    captain.ship = static_cast<std::uint32_t>(fleetIndex);
    SOL_LOG_INFO("%s takes '%s' at %s", captain.name.c_str(), ship.defId.c_str(), dockedStationName());
    return true;
}

bool SpaceWorld::recallCaptain(std::size_t captainIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to recall a captain", outError);
    }
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.ship == kNoIndex) {
        return refuse("'" + captain.name + "' has no ship", outError);
    }
    // ⚑⚑⚑⚑ AND NOT WHILE THEY HAVE ORDERS, WHICH THE LIVE DRIVE FOUND AND NO
    // TEST DID. A captain given a route but not yet under way is still parked
    // on your dock, so every condition below passes and the hull came back -
    // leaving a person with no ship and a standing order to run one, which is
    // the exact `ship == kNoIndex && order.kind != None` pair the v40 LOADER
    // REFUSES. Second path to a save the game writes happily and then declines
    // to open, and the same shape as the `switchShip` hole above.
    //
    // ⚑⚑⚑ THE SHARP HALF IS WHERE THE RULE ALREADY WAS: the Crew tab's fill
    // computes `captainCanRecall = parkedHere && !ordered` and greys the button
    // correctly, so the SCREEN knew and the WORLD did not. This file states the
    // hazard about `firstFreeMountFor` in its own words - duplicating a rule in
    // the screen is how the button and the transaction drift apart - and this
    // is that, with the screen being the half that was right.
    if (captain.order.kind != OrderKind::None) {
        return refuse("'" + captain.name + "' has orders - stand them down first", outError);
    }
    // ⚑⚑⚑ BOUNDS-CHECKED RATHER THAN TRUSTED, AND THE MUTATION PASS IS
    // WHY. `sellShip` refuses to sell a hull somebody is holding, so this index
    // cannot dangle - which made this line a correctness property held up by a
    // guard in a different function, with nothing here saying so. Breaking that
    // guard did not produce a failing check: it produced `vector subscript out
    // of range` and killed the process before the checks could print. Same
    // lesson as Phase 38 stage C's dangling `targetIndex`: say it where it is
    // relied on.
    if (captain.ship >= m_fleet.size()) {
        return refuse("'" + captain.name + "' has no ship", outError);
    }
    const OwnedShip& ship = m_fleet[captain.ship];
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("'" + captain.name + "' is not here - their ship is stored elsewhere", outError);
    }
    SOL_LOG_INFO("%s hands back '%s'", captain.name.c_str(), ship.defId.c_str());
    captain.ship = kNoIndex;
    return true;
}

// --- Standing orders (Phase 39 stage B) -------------------------------------

double SpaceWorld::haulLegSeconds(std::uint32_t fromMarket, std::uint32_t toMarket) const
{
    const std::vector<sim::StationMarket>& markets = m_economy.markets();
    if (fromMarket >= markets.size() || toMarket >= markets.size()) {
        return 0.0;
    }
    // The coarse fleet's own numbers, not a second set. `traderLegSeconds` is
    // documented as "in-system travel per endpoint" and `jumpSeconds` as per
    // gate transit, and this is `Economy::beginTransit`'s line verbatim - so a
    // captain and a hauler crossing the same lane take the same time over it,
    // and `sim::routeOf` decomposes both the same way.
    const std::uint8_t hops =
        m_economy.hopCount(markets[fromMarket].systemIndex, markets[toMarket].systemIndex);
    const sim::EconomyParams& params = m_economy.params();
    return params.traderLegSeconds * 2.0 +
           static_cast<double>(hops == sim::kUnreachableHops ? 0u : hops) * params.jumpSeconds;
}

void SpaceWorld::haulDestinations(std::vector<HaulDestination>& out) const
{
    out.clear();
    const std::uint32_t here = dockedMarket();
    if (!isDocked() || here == kNoIndex) {
        return; // a run starts from the dock you are standing on
    }
    const std::vector<sim::StationMarket>& markets = m_economy.markets();
    const std::uint32_t hereSystem = markets[here].systemIndex;
    // THE LIST IS WHAT THE PLAYER REMEMBERS, WHICH IS `SurveySim`'s LEDGER AND
    // NOT THE MARKET TABLE. You cannot send a captain somewhere you have never
    // seen a price from - the same knowledge the Trade tab's "elsewhere" column
    // and the map's trade overlay read, so buying market intel is what widens a
    // fleet's reach and this phase adds no second notion of "known".
    for (const sim::MarketMemory& memory : m_survey.marketMemory()) {
        if (memory.market == here || memory.market >= markets.size()) {
            continue;
        }
        const std::uint32_t system = markets[memory.market].systemIndex;
        const std::uint8_t hops = m_economy.hopCount(hereSystem, system);
        if (hops == sim::kUnreachableHops) {
            continue; // past `maxTradeJumps`: the economy would not plan it either
        }
        const sim::SystemSpec& spec = m_galaxy.systems[system];
        out.push_back({.market = memory.market,
                       .station = spec.stations[markets[memory.market].stationIndex].name,
                       .system = spec.name,
                       .hops = hops});
    }
    std::sort(out.begin(), out.end(), [](const HaulDestination& a, const HaulDestination& b) {
        if (a.hops != b.hops) {
            return a.hops < b.hops;
        }
        return a.station < b.station;
    });
}

bool SpaceWorld::orderHaul(std::size_t captainIndex, std::uint32_t market, float floor, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to give a captain a route", outError);
    }
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.ship >= m_fleet.size()) {
        return refuse("'" + captain.name + "' has no ship", outError);
    }
    // THE SAME DOCK AGAIN, AND FOR STAGE A'S REASON RATHER THAN FOR SYMMETRY:
    // a route starts where the hull is, so if the hull is not here there is no
    // "from", and the destination list the player picked out of was measured
    // from somewhere else entirely.
    const OwnedShip& ship = m_fleet[captain.ship];
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("'" + captain.name + "' is not here - their ship is stored elsewhere", outError);
    }
    if (captain.haul.leg.phase == sim::TraderPhase::InTransit) {
        // Cannot be reached through the door above (a hull in transit is stored
        // nowhere), and said here anyway: re-pointing a LADEN hull at a market
        // it did not buy for is the one way an order change loses money silently.
        return refuse("'" + captain.name + "' is on a haul - cancel the order first", outError);
    }
    const std::uint32_t here = dockedMarket();
    if (here == kNoIndex) {
        return refuse("no market here to run a route from", outError);
    }
    if (market >= m_economy.markets().size()) {
        return refuse("no such market", outError);
    }
    if (market == here) {
        return refuse("that is the dock you are standing on", outError);
    }
    if (m_survey.remembered(market) == nullptr) {
        return refuse("you have never seen that market's prices", outError);
    }
    if (m_economy.hopCount(m_currentSystem, m_economy.markets()[market].systemIndex) ==
        sim::kUnreachableHops) {
        return refuse("no route there inside a hauler's range", outError);
    }
    // ⚑ THE FLOOR IS CLAMPED HERE RATHER THAN TRUSTED, on the same rule the
    // save loader applies to the same field: this is the one door the value
    // comes in by, and a floor past `kMaxSellFloor` is a captain who can never
    // sell. Clamping rather than refusing, because unlike the ten conditions
    // above it there is no way for a player using the screen to ask for one.
    captain.order = {.kind = OrderKind::Haul,
                     .marketA = here,
                     .marketB = market,
                     .stopping = false,
                     .floor = core::clamp(floor, 0.0f, kMaxSellFloor)};
    captain.haul.leg.origin = here;
    captain.haul.leg.market = here;
    captain.haul.leg.phase = sim::TraderPhase::Idle;
    const sim::StationMarket& far = m_economy.markets()[market];
    SOL_LOG_INFO("%s will run %s <-> %s (%.0f s a leg)",
                 captain.name.c_str(),
                 dockedStationName(),
                 m_galaxy.systems[far.systemIndex].stations[far.stationIndex].name.c_str(),
                 haulLegSeconds(here, market));
    return true;
}

float SpaceWorld::shipMiningPower(const OwnedShip& ship) const
{
    if (m_defs == nullptr) {
        return 0.0f;
    }
    // ⚑⚑ THE FIT, NOT THE HULL, AND THAT IS WHAT MAKES THE ORDER A DECISION.
    // `resolvedShipDef` is "the ship as flown" - a hull with what is actually
    // bolted into its mounts, which is where the player's shipyard money went -
    // so a freighter with two Deep Core Lasers cuts eighteen units a second and
    // the same freighter straight off the forecourt cuts nothing and cannot be
    // posted at all.
    //
    // ⚑ SUMMED OVER EVERY BEAM RATHER THAN THE BEST ONE, on `applyShipDef`'s
    // rule: the hull's mount list IS the armament, and a second laser is a
    // hardpoint the player gave up something else for. It walks the def's
    // mounts rather than a spawned `ShipArmament`, because this is asked at the
    // Crew tab about a hull parked on a pad, which has no body to read.
    float power = 0.0f;
    const assets::ShipDef def = resolvedShipDef(ship);
    for (const assets::ShipMount& mount : def.mounts) {
        if (!assets::mountTakesWeapon(mount.kind) || mount.fit.empty()) {
            continue;
        }
        const assets::WeaponDef* weapon = m_defs->findWeapon(mount.fit.c_str());
        if (weapon != nullptr) {
            power += weapon->miningPower;
        }
    }
    return power;
}

float SpaceWorld::shipGunPower(const OwnedShip& ship) const
{
    // ⚑⚑ `shipMiningPower`'s twin, over the same mounts and by the same
    // argument (stage D). The two combat orders refuse a hull that cannot
    // shoot, exactly as "mine here" refuses one that cannot cut, and the
    // failure they prevent is the same silent one: a beat flown by a freighter
    // is an order that every screen reports as working right up until the
    // hull is shot down without returning fire.
    //
    // ⚑ DAMAGE PER SECOND RATHER THAN A GUN COUNT, because the question the
    // refusal asks is "can this hull hurt anything" and a mount holding a
    // mining laser answers no while counting as a weapon everywhere else -
    // which is precisely the freighter a player would try to post first.
    if (m_defs == nullptr) {
        return 0.0f;
    }
    float power = 0.0f;
    const assets::ShipDef def = resolvedShipDef(ship);
    for (const assets::ShipMount& mount : def.mounts) {
        if (!assets::mountTakesWeapon(mount.kind) || mount.fit.empty()) {
            continue;
        }
        const assets::WeaponDef* weapon = m_defs->findWeapon(mount.fit.c_str());
        if (weapon != nullptr && weapon->damage > 0.0f) {
            power += weapon->damage * std::max(weapon->rateOfFire, 0.0f);
        }
    }
    return power;
}

bool SpaceWorld::setSellFloor(std::size_t captainIndex, float floor)
{
    if (captainIndex >= m_captains.size()) {
        return false;
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.order.kind != OrderKind::Haul) {
        return false;
    }
    // Clamped rather than refused, for `orderHaul`'s reason: the screen cannot
    // ask for a value outside the presets, so a caller that does is a script or
    // a console line, and the useful answer is the nearest floor this game can
    // actually hold out for.
    captain.order.floor = core::clamp(floor, 0.0f, kMaxSellFloor);
    return true;
}

bool SpaceWorld::orderMine(std::size_t captainIndex, std::string* outError)
{
    // EVERY DOOR `orderHaul` CHECKS, IN THE SAME ORDER AND FOR THE SAME
    // REASONS, because they are facts about giving a person an order and not
    // about which order it is. What differs is only the last two.
    if (!isDocked()) {
        return refuse("must be docked to give a captain an order", outError);
    }
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.ship >= m_fleet.size()) {
        return refuse("'" + captain.name + "' has no ship", outError);
    }
    const OwnedShip& ship = m_fleet[captain.ship];
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("'" + captain.name + "' is not here - their ship is stored elsewhere", outError);
    }
    if (captain.order.kind != OrderKind::None) {
        return refuse("'" + captain.name + "' already has orders - cancel them first", outError);
    }
    const std::uint32_t here = dockedMarket();
    if (here == kNoIndex) {
        return refuse("no market here to sell the ore at", outError);
    }
    // ⚑⚑⚑ THE ROCK IS ASKED FOR BEFORE THE ORDER IS TAKEN, AND IT IS THE ONE
    // REFUSAL THAT COULD NOT BE ANSWERED FROM THE CREW TAB'S OWN STATE. A field
    // is a pure function of the system's seed (`fieldCountFor` is the single
    // definition of that rule and the galaxy generator already asks it, which
    // is why a Mining Outpost is never placed where there is nothing to dig).
    // The Core tier draws from {0, 1} fields, so "this system has no rock" is a
    // real and reachable answer rather than a defensive line.
    if (m_mining.fieldCount(m_currentSystem) == 0) {
        return refuse("there is nothing to mine in this system", outError);
    }
    if (shipMiningPower(ship) <= 0.0f) {
        return refuse("'" + captain.name + "' has no mining beam on that hull", outError);
    }
    captain.order = {.kind = OrderKind::Mine, .marketA = here, .marketB = kNoIndex, .stopping = false};
    captain.mine = {};
    // THE HULL LEAVES THE PAD, THROUGH THE SAME TWO LINES `beginCaptainTransit`
    // USES AND FOR ITS REASON RATHER THAN BY COPYING IT. `sellShip`,
    // `switchShip` and `recallCaptain` all refuse a hull whose stored station is
    // not the one you are standing on, and `kNoIndex` is never that - so a ship
    // out at a rock cannot be sold, boarded or handed back through any door,
    // and none of the three needed a clause about mining either.
    OwnedShip& hull = m_fleet[captain.ship];
    hull.storedSystem = kNoIndex;
    hull.storedStation = kNoIndex;
    SOL_LOG_INFO("%s will work the rock in %s and sell at %s (%.1f units/s)",
                 captain.name.c_str(),
                 m_galaxy.systems[m_currentSystem].name.c_str(),
                 dockedStationName(),
                 static_cast<double>(shipMiningPower(hull)));
    return true;
}

bool SpaceWorld::orderPatrol(std::size_t captainIndex, std::string* outError)
{
    // `orderMine`'s doors again, and it is a THIRD copy of the same six lines
    // on purpose rather than a shared helper: each order refuses on its own
    // last clause and the shared part is where the player's name for the
    // refusal comes from. A helper would have to take the error strings back
    // out as parameters, which is the same code with an indirection on it.
    if (!isDocked()) {
        return refuse("must be docked to give a captain an order", outError);
    }
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.ship >= m_fleet.size()) {
        return refuse("'" + captain.name + "' has no ship", outError);
    }
    const OwnedShip& ship = m_fleet[captain.ship];
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("'" + captain.name + "' is not here - their ship is stored elsewhere", outError);
    }
    if (captain.order.kind != OrderKind::None) {
        return refuse("'" + captain.name + "' already has orders - cancel them first", outError);
    }
    const std::uint32_t here = dockedMarket();
    if (here == kNoIndex) {
        return refuse("a patrol is posted from a dock, and this one has no market", outError);
    }
    // ⚑⚑⚑ AND THE ONE CLAUSE THAT IS THIS ORDER'S OWN: A HULL WITH NOTHING IN
    // ITS WEAPON MOUNTS IS NOT A PATROL. It is `orderMine`'s beam check pointed
    // at the other kind of hardpoint, and the failure it prevents is silent: a
    // hull posted to a beat flies the beat, finds a raider, closes on it and is
    // shot down without ever returning fire, and every screen in the game
    // reports that as the order working.
    //
    // ⚑⚑⚑⚑ BUT IT IS A FLOOR AND NOT A JUDGEMENT, AND THE SHIPPED DATA IS WHY
    // - found by a test that asserted the opposite and failed. `sol.mining_laser`
    // has `damage = 3.0` beside its `mining_power = 4.0`, and so does its mk2,
    // because `WeaponDef`'s own comment insists on it: "0 leaves a weapon a
    // weapon - a mining laser is an ordinary hardpoint choice, not a mode". So
    // EVERY weapon this game ships can hurt something, and the state this clause
    // refuses is reachable only through an EMPTY mount - a hull whose fitting
    // was sold, or a mod's hull with no weapon hardpoint at all.
    //
    // ⚑⚑ WHICH MEANS THE REAL ANSWER TO "CAN THIS HULL FIGHT" IS A NUMBER AND
    // NOT A BOOLEAN, and it is given to the player rather than enforced: the
    // Crew tab prints the hull's damage per second beside the button, exactly as
    // the mining row prints "cuts 13.0 units a second". A freighter with one
    // mining laser CAN be posted to a beat, at nine damage a second, and finding
    // out that this is not enough is a thing the player is allowed to do. The
    // alternative is an invented threshold, and this file has no honest number
    // to put in one.
    if (shipGunPower(ship) <= 0.0f) {
        return refuse("'" + captain.name + "' has no guns on that hull", outError);
    }
    captain.order = {.kind = OrderKind::Patrol, .marketA = here, .marketB = kNoIndex, .stopping = false};
    captain.mine = {};
    // The hull leaves the pad, `orderMine`'s two lines and its reason: a ship
    // out on a beat cannot be sold, boarded or handed back, and none of those
    // three doors needed a clause about patrolling either.
    OwnedShip& hull = m_fleet[captain.ship];
    hull.storedSystem = kNoIndex;
    hull.storedStation = kNoIndex;
    SOL_LOG_INFO("%s will patrol %s", captain.name.c_str(), m_galaxy.systems[m_currentSystem].name.c_str());
    return true;
}

bool SpaceWorld::orderEscort(std::size_t captainIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to give a captain an order", outError);
    }
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.ship >= m_fleet.size()) {
        return refuse("'" + captain.name + "' has no ship", outError);
    }
    const OwnedShip& ship = m_fleet[captain.ship];
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("'" + captain.name + "' is not here - their ship is stored elsewhere", outError);
    }
    if (captain.order.kind != OrderKind::None) {
        return refuse("'" + captain.name + "' already has orders - cancel them first", outError);
    }
    if (shipGunPower(ship) <= 0.0f) {
        return refuse("'" + captain.name + "' has no guns on that hull", outError);
    }
    // ⚑⚑⚑ AND THE FENCE, WHICH IS RULING 4's AND IS CHECKED RATHER THAN
    // ASSUMED. "Every order in this phase is given to exactly one captain, and
    // nothing here reads more than one captain at a time" - and a second escort
    // is the first thing in this game that would have been a FLEET: two hulls
    // holding station on one point, resolving an order between them. That is
    // Phase 40 and it is refused here by name rather than allowed to arrive as
    // a bug in stage E's readout.
    for (std::size_t i = 0; i < m_captains.size(); ++i) {
        if (i != captainIndex && escorting(m_captains[i].order.kind)) {
            return refuse("'" + m_captains[i].name + "' is already flying as your escort", outError);
        }
    }
    // ⚑⚑ NO MARKET, AND IT IS THE ONLY ORDER WITH NONE. The other three name a
    // place; this one names a person who moves, so `marketA` stays unset and
    // `captainSystem` answers from the player. Writing the dock in here "for
    // symmetry" is how a field that means "where they are" starts meaning
    // "where they were hired", which is the shape `CaptainOrder`'s own comment
    // refuses for `stopping`.
    captain.order = {.kind = OrderKind::Escort, .marketA = kNoIndex, .marketB = kNoIndex, .stopping = false};
    captain.mine = {};
    OwnedShip& hull = m_fleet[captain.ship];
    hull.storedSystem = kNoIndex;
    hull.storedStation = kNoIndex;
    SOL_LOG_INFO("%s will fly as your escort", captain.name.c_str());
    return true;
}

std::string SpaceWorld::captainHailLine(std::size_t captainIndex) const
{
    if (captainIndex >= m_captains.size()) {
        return "Standing by.";
    }
    const Captain& captain = m_captains[captainIndex];
    switch (captain.order.kind) {
    case OrderKind::Mine:
        return captain.mine.phase == MinePhase::Selling ? "Hold's full, taking it in now."
                                                        : "Still cutting. Plenty of rock left out here.";
    case OrderKind::Patrol:
        return "On the beat. Quiet so far.";
    case OrderKind::Escort:
        return "Right behind you. Say the word.";
    case OrderKind::Haul: {
        const sim::TraderRoute route = captainRoute(captainIndex);
        return route.leg == sim::TraderLeg::Arrive ? "Nearly in. Cargo's intact."
               : route.leg == sim::TraderLeg::Jump ? "Between gates. Nothing to report."
                                                   : "Loaded and running. See you at the far end.";
    }
    case OrderKind::None:
        break;
    }
    return "Waiting on orders.";
}

bool SpaceWorld::cancelOrder(std::size_t captainIndex, std::string* outError)
{
    if (captainIndex >= m_captains.size()) {
        return refuse("no such captain", outError);
    }
    Captain& captain = m_captains[captainIndex];
    if (captain.order.kind == OrderKind::None) {
        return refuse("'" + captain.name + "' has no orders", outError);
    }
    // NOT DOCKED-GATED, and it is the only captain verb that is not. Every
    // other one moves a hull or a person between you and a dock you are both
    // standing on; calling somebody off a route is a message, and requiring a
    // flight to the far end of their own run to send it would make a bad order
    // cost more to cancel than it cost to give.
    // ⚑⚑⚑ THE SAME FIELD ANSWERS BOTH ORDER KINDS AND IT IS TRUE FOR TWO
    // DIFFERENT REASONS, WHICH IS WORTH SAYING BECAUSE ONE OF THEM IS A HAZARD
    // AND THE OTHER IS COURTESY (stage C). For a haul, `stopping` exists
    // because there is nowhere to put a laden hull between two gates - the
    // falls-between-representations defect reached by pressing a button. For a
    // mining order that hazard does not exist at all: the dock is in the same
    // system as the rock, so standing down on the spot would be perfectly safe.
    // It waits anyway, because the hold is full of ore the player's captain
    // spent real minutes cutting and throwing it away to save one flight across
    // one system is not a saving anybody asked for. They take the load in and
    // park.
    // ⚑⚑⚑⚑ AND `stationary()` IS THE WRONG PREDICATE FOR *THIS* RULE, WHICH A
    // TEST CAUGHT AND WHICH IS THE STAGE'S SECOND BORROWED-RULE FAILURE (stage
    // D). It is exactly right for the REPRESENTATION - a patrol holds a bubble
    // for the same reason a mining order does - and it is wrong here, because
    // the reason a mining captain waits is THE HOLD, and a patrol has no hold.
    // Reading `stationary()` sent a cancelled patrol into `MinePhase::Selling`,
    // a phase its tick never looks at, and the order simply never ended: the
    // Crew tab said "standing down" forever. ⚑ Stage C's own headline was a
    // pace borrowed from a neighbouring order carrying its endpoint's
    // assumptions; this is the same shape one stage on, and the lesson holds -
    // a predicate that names the right SET can still answer the wrong QUESTION.
    if (captain.order.kind == OrderKind::Mine) {
        captain.order.stopping = true;
        captain.mine.phase = MinePhase::Selling;
        SOL_LOG_INFO("%s will bring the load in and stand down", captain.name.c_str());
        return true;
    }
    // ⚑⚑ A COMBAT ORDER STANDS DOWN BY COMING HOME, which is the same courtesy
    // one sentence up with the reason changed. There is no cargo to save, so
    // this could safely end on the spot - and a hull abandoned in open space is
    // one the player then has to fly out and collect, because `OwnedShip` parks
    // at a PAD and nowhere else. They fly back to a dock and park, and the tick
    // that flies them is the same one that was flying the beat.
    if (fighting(captain.order.kind)) {
        captain.order.stopping = true;
        SOL_LOG_INFO("%s is coming in to stand down", captain.name.c_str());
        return true;
    }
    if (captain.haul.leg.phase == sim::TraderPhase::InTransit) {
        captain.order.stopping = true;
        SOL_LOG_INFO("%s will stand down at the end of this leg", captain.name.c_str());
        return true;
    }
    // ⚑⚑⚑⚑ AND THE HOLD IS SETTLED BEFORE THE ORDER GOES, WHICH STAGE E HAD TO
    // ADD AND WHICH WAS UNREACHABLE UNTIL IT DID. This branch ends the order on
    // the spot because the captain is parked at a market with nothing in
    // flight - and before the sell floor existed, a parked captain had always
    // just sold, so the hold was always empty here and clearing the order cost
    // nothing. A floor makes "parked AND laden" an ordinary state, and then
    // this line strands the player's money: ruling 7 bought that cargo out of
    // their credits, the order that would have settled it is gone, and no
    // arrival will ever come round again. Nothing anywhere would have said so.
    //
    // The floor is deliberately IGNORED. Standing down is the player saying the
    // run is over, and a floor is an instruction about which trades to wait
    // for - there is nothing left to wait for. Better a bad price than money
    // that cannot be reached.
    settleCaptainSale(captain, captain.haul.leg.market, /*ignoreFloor=*/true);
    captain.order = {};
    SOL_LOG_INFO("%s stands down", captain.name.c_str());
    return true;
}

sim::TraderRoute SpaceWorld::captainRoute(std::size_t captainIndex) const
{
    if (captainIndex >= m_captains.size()) {
        return {};
    }
    const sim::EconomyTrader& leg = m_captains[captainIndex].haul.leg;
    const std::vector<sim::StationMarket>& markets = m_economy.markets();
    if (leg.origin >= markets.size() || leg.market >= markets.size()) {
        return {};
    }
    const std::uint32_t fromSystem = markets[leg.origin].systemIndex;
    const std::uint32_t toSystem = markets[leg.market].systemIndex;
    const std::uint8_t hops = m_economy.hopCount(fromSystem, toSystem);
    // THE SAME DECOMPOSITION THE COARSE FLEET READS, and that is the whole
    // reason `routeOf` was lifted out of `Economy` this stage. A second copy of
    // "elapsed < legSeconds means Depart" living in this file is how a captain
    // and a hauler would eventually disagree about where the gate is.
    return sim::routeOf(
        leg, m_economy.params(), fromSystem, toSystem, hops == sim::kUnreachableHops ? 0u : hops);
}

std::uint32_t SpaceWorld::captainSystem(std::size_t captainIndex) const
{
    if (captainIndex >= m_captains.size()) {
        return kNoIndex;
    }
    const Captain& captain = m_captains[captainIndex];
    if (captain.ship >= m_fleet.size()) {
        return kNoIndex;
    }
    // ⚑⚑⚑ A STATIONARY CAPTAIN IS IN THE SYSTEM THEIR ORDER NAMES, AND IT IS
    // ANSWERED HERE BECAUSE THE HULL IS PARKED NOWHERE (stage C). `orderMine`
    // clears `storedSystem`/`storedStation` for the reason `beginCaptainTransit`
    // does - a hull that is flying is not on a pad, and the three stored-ship
    // rules refuse `kNoIndex` for free - so the fallback below would answer
    // "nowhere" for a ship that is very much somewhere. The ORDER is the
    // durable statement of where they are, which is the same thing that makes
    // `bubbleHoldsPlayerAsset` answerable without a body.
    if (stationary(captain.order.kind)) {
        return captain.order.marketA < m_economy.markets().size()
                   ? m_economy.markets()[captain.order.marketA].systemIndex
                   : kNoIndex;
    }
    const sim::TraderRoute route = captainRoute(captainIndex);
    if (route.leg == sim::TraderLeg::None) {
        return m_fleet[captain.ship].storedSystem; // parked at a station somewhere
    }
    return route.system == sim::kNoSystem ? kNoIndex : route.system;
}

bool SpaceWorld::systemIsInstantiated(std::uint32_t system) const
{
    for (const std::unique_ptr<SystemBubble>& bubble : m_bubbles) {
        if (bubble->system == system) {
            return true;
        }
    }
    return false;
}

void SpaceWorld::settleCaptainSale(Captain& captain, std::uint32_t market, bool ignoreFloor)
{
    CaptainHaul& haul = captain.haul;
    CaptainLedger& ledger = captain.ledger;
    if (haul.leg.cargo <= 0.0f || market >= m_economy.markets().size()) {
        return;
    }
    const float aboard = haul.leg.cargo;
    // ⚑⚑⚑⚑ THE FLOOR IS JUDGED BEFORE THE STOCK MOVES, BECAUSE `sell` CANNOT BE
    // UNDONE (stage E). This is the whole of "sell when the price clears X":
    // quote what this end would pay for the units it could actually take, price
    // the share of the outlay those units carry, and refuse the trade if the
    // margin is under what the player asked for.
    //
    // ⚑⚑⚑ THE BASIS IS THE SHARE, NOT THE WHOLE OUTLAY, and getting that wrong
    // would have made the floor fire on good trades: a market with room for
    // half the load pays half the revenue, and scoring that against ALL of the
    // cost reads as a 50% loss on a run that is breaking even. It is the same
    // apportionment the settle below already does for the cut, asked one step
    // earlier - which is why `sellableUnits` had to exist.
    //
    // ⚑⚑⚑⚑ AND A CAPTAIN STANDING DOWN IGNORES THE FLOOR, WHICH IS NOT A
    // COURTESY BUT THE FIELD REFUSING TO EAT THE PLAYER'S CAPITAL. Ruling 7
    // funds the cargo out of the player's credits at the BUY, so an unsold hold
    // is money already spent and not yet recovered. Without this clause a
    // cancel while the floor is unmet clears the order - and with it the floor
    // it was judged by - and parks a hull with the player's money still sitting
    // in its hold, recoverable by nothing: the order is gone, so no arrival
    // will ever settle it again. Standing down is the player saying "we are
    // done", and being done means taking what the load fetches.
    const bool holdOut = captain.order.floor > 0.0f && !captain.order.stopping && !ignoreFloor;
    const float movable = m_economy.sellableUnits(market, haul.leg.commodity, aboard);
    if (movable > 0.0f && holdOut && haul.outlay > 0.0) {
        const double quoted = static_cast<double>(m_economy.quoteSell(market, haul.leg.commodity, aboard));
        const double basis = haul.outlay * static_cast<double>(movable / aboard);
        if (quoted < basis * (1.0 + static_cast<double>(captain.order.floor))) {
            // ⚑⚑⚑⚑ AND THE LOAD RIDES ON RATHER THAN WAITING HERE (the user's
            // ruling 16). The captain keeps flying the route and tests the
            // floor again at the other end, which is the behaviour the full
            // warehouse branch below already has - and it is the answer to the
            // warning `captainThink` left against this exact moment: a captain
            // parked at one end holding cargo "would look identical to a broken
            // one". A hull that is always on a leg never does. It also means a
            // market that moves in the player's favour gets taken the next time
            // round, which a captain sitting at the far end would miss.
            SOL_LOG_INFO("%s held %.0f %s at %s: %.0f cr is under the %.0f%% floor on %.0f",
                         captain.name.c_str(),
                         static_cast<double>(movable),
                         haul.leg.commodity < m_commodityIds.size()
                             ? m_commodityIds[haul.leg.commodity].c_str()
                             : "cargo",
                         m_galaxy.systems[m_economy.markets()[market].systemIndex].name.c_str(),
                         quoted,
                         static_cast<double>(captain.order.floor) * 100.0,
                         basis);
            return;
        }
    }
    const sim::TradeResult sold = m_economy.sell(market, haul.leg.commodity, aboard);
    if (sold.units <= 0.0f) {
        // A full warehouse: the load stays in the hold and rides on to the
        // other end, which is `Economy`'s own answer and for its own reason -
        // tipping it into space is a galaxy-wide leak of goods.
        return;
    }
    // THE COST BASIS LEAVES THE HOLD WITH THE UNITS THAT SOLD, rather than the
    // whole outlay being settled against a partial sale. A market that can only
    // take half the load would otherwise book the entire purchase against half
    // the revenue and read as a disastrous run, and the captain would be paid
    // nothing on the half that actually made money.
    const double share = core::clamp(static_cast<double>(sold.units / aboard), 0.0, 1.0);
    const double basis = haul.outlay * share;
    const double gross = static_cast<double>(sold.credits);
    const double profit = gross - basis;
    // THE CUT IS OF THE PROFIT AND NEVER OF THE SALE (the user's ruling 6,
    // taken before a line was written). On a thin margin a cut of the gross
    // takes more than the run made, so a bad route would DRAIN the player
    // rather than merely underperform - and ruling 3's "an idle captain costs
    // nothing rather than bleeding" would stop being true of a working one. A
    // haul that loses money pays them nothing; it does not bill them either,
    // because a captain has no purse of their own to bill from.
    const double cut = profit > 0.0 ? profit * static_cast<double>(captain.cut) : 0.0;
    m_playerCredits += gross - cut;
    haul.outlay -= basis;
    haul.leg.cargo = std::max(0.0f, aboard - sold.units);
    ledger.earned += profit - cut;
    ledger.paid += cut;
    SOL_LOG_INFO("%s sold %.0f %s for %.0f cr (%.0f profit, %.0f to them)",
                 captain.name.c_str(),
                 static_cast<double>(sold.units),
                 haul.leg.commodity < m_commodityIds.size() ? m_commodityIds[haul.leg.commodity].c_str()
                                                            : "cargo",
                 gross,
                 profit,
                 cut);
}

void SpaceWorld::beginCaptainTransit(Captain& captain, std::uint32_t destination)
{
    CaptainHaul& haul = captain.haul;
    haul.leg.origin = haul.leg.market;
    haul.leg.market = destination;
    haul.leg.phase = sim::TraderPhase::InTransit;
    haul.leg.legTotal = haulLegSeconds(haul.leg.origin, destination);
    haul.leg.travelRemaining = haul.leg.legTotal;
    // THE HULL IS PARKED NOWHERE WHILE IT IS FLYING, AND THAT IS WHAT MAKES THE
    // THREE STORED-SHIP RULES HOLD WITHOUT A FOURTH BEING WRITTEN. `sellShip`,
    // `switchShip` and `recallCaptain` all refuse a hull whose stored station is
    // not the one you are standing on, and `kNoIndex` is never that - so a ship
    // mid-haul cannot be sold, boarded or handed back through any door, and none
    // of the three needed a new clause about routes.
    OwnedShip& ship = m_fleet[captain.ship];
    ship.storedSystem = kNoIndex;
    ship.storedStation = kNoIndex;
}

void SpaceWorld::captainThink(Captain& captain, std::uint32_t here, std::uint32_t there)
{
    CaptainHaul& haul = captain.haul;
    // Room may have opened here since arrival: shift what the last stop could
    // not take before thinking about a new load. `traderThink`'s own opening.
    settleCaptainSale(captain, here);
    if (haul.leg.cargo > 0.0f) {
        beginCaptainTransit(captain, there); // laden: no room aboard for a new haul
        return;
    }

    const float capacity = resolvedShipDef(m_fleet[captain.ship]).cargoCapacity;
    if (capacity > 1.0f) {
        // WHAT TO CARRY IS THE CAPTAIN'S JUDGEMENT, WHICH IS WHAT THE CUT BUYS.
        // The ranking is `Economy::traderThink`'s - return on capital per
        // second, so nobody ever declines to haul something cheap with a good
        // margin - with its market loop deleted, because the ORDER names the
        // market, and with its PRICES replaced. That replacement is the whole
        // finding of this stage.
        //
        // ⚑⚑⚑⚑ A HAUL PRICED AT THE MARGINAL PRICE IS PRICED AT A LIE, AND
        // THE FIRST MEASUREMENT OF THIS STAGE WAS EIGHT ROUTES OUT OF EIGHT
        // LOSING MONEY. `Economy::buy` charges at the midpoint of the stock the
        // trade moves and `sell` pays at the midpoint of the stock it fills, so
        // a 200-unit hold costs about 1.4x its quoted price and fetches about
        // 0.8x - which turns a route that reads as a 20% margin into a 40%
        // loss. `traderThink` estimates the same way and gets away with it only
        // because it ranks across EVERY reachable market and so lands where a
        // hold barely moves the curve; a captain pinned to one route by an
        // order has no such luxury. `quoteBuy`/`quoteSell` are the honest
        // numbers and they are what this asks. Phase 37 found this exact
        // arithmetic in `playerBuy`'s purse clamp; this is the same error in
        // the PROFIT ESTIMATE, one layer along.
        //
        // ⚑⚑⚑ AND THE SIZE IS PART OF THE JUDGEMENT, NOT A CONSTANT. Price
        // impact grows with the block, so on a thin route a half-load clears
        // where a full one does not - and the difference between a captain who
        // deadheads forever and one who earns is being willing to carry less.
        // Four sizes rather than a bisection: the profit curve over size is
        // concave from zero, so a coarse sample lands near the peak, and the
        // alternative would be `unitsWithin`'s machinery for a number nobody is
        // going to audit to the credit.
        static constexpr float kLoadFractions[] = {1.0f, 0.75f, 0.5f, 0.25f};
        const auto seconds = static_cast<float>(std::max(1.0, haulLegSeconds(here, there)));
        float bestRate = 0.0f;
        float bestUnits = 0.0f;
        std::uint32_t best = kNoIndex;
        for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(m_commodityIds.size()); ++c) {
            const float onHand = std::min(capacity, m_economy.stock(here, c));
            if (onHand <= 1.0f) {
                continue;
            }
            // A station with no hold for the good has negative room and is
            // never scored, which is how a captain learns not to fly salvage to
            // a dock that cannot take it.
            //
            // ⚑⚑ AND THE ROOM IS WHAT WILL BE LEFT ONCE EVERYTHING ALREADY
            // FLYING THERE HAS LANDED. `traderThink` has subtracted `inbound`
            // since Phase 8g and said why, and while it was private a captain
            // was the one hauler in the galaxy planning blind - it would buy
            // into a shortage a hundred and twenty coarse traders were already
            // on their way to fill. ⚑ HONESTY ABOUT THIS ONE: it was added on
            // the theory that it explained the losses below, and it did not -
            // the numbers came out byte-identical, because `inbound` is zero at
            // most (market, commodity) pairs with 120 traders spread over
            // sixteen hundred of them. It is kept because it is the rule every
            // other hauler follows, not because it moved anything.
            const float room =
                m_economy.capacityOf(there, c) - m_economy.stock(there, c) - m_economy.inbound(there, c);
            if (room <= 1.0f) {
                continue;
            }
            for (const float fraction : kLoadFractions) {
                const float units = std::min(onHand * fraction, room);
                if (units <= 1.0f) {
                    continue;
                }
                const float cost = m_economy.quoteBuy(here, c, units);
                const float revenue = m_economy.quoteSell(there, c, units);
                const float profit = revenue - cost;
                // ⚑⚑⚑⚑ A MARGIN THAT DOES NOT CLEAR THE SPREAD IS NOT A TRADE,
                // AND THIS IS THE THIRD THING THE MEASUREMENT FOUND. Ranking by
                // rate takes the BEST rate available, which on a fixed route
                // through a galaxy a hundred and twenty haulers are already
                // arbitraging is routinely near zero: one measured leg tied up
                // 2,999 cr for four minutes to make ELEVEN. That is not merely
                // a poor trade, it is a losing one, because the estimate is
                // made at departure and a three-hop leg is long enough for the
                // far market to move about 20% under it - so a 0.4% expectation
                // realises as a 16% loss about as often as not.
                //
                // ⚑⚑⚑ THE FLOOR IS `priceSpread` AND IT IS BORROWED RATHER
                // THAN INVENTED. That constant is already the number this
                // economy uses to say what a thin margin IS - its own comment:
                // "without a spread a thin margin is indistinguishable from a
                // good one and a round trip at one station is free". So the
                // rule is that a haul must clear the spread AGAIN, over and
                // above paying it, which is the smallest honest definition of
                // "worth the trip" available without inventing a taste number.
                if (!(cost > 0.0f) || !(profit > cost * m_economy.params().priceSpread)) {
                    continue;
                }
                const float rate = profit / (cost * seconds);
                if (rate > bestRate) {
                    bestRate = rate;
                    bestUnits = units;
                    best = c;
                }
            }
        }
        if (best != kNoIndex) {
            // THE PURSE CLAMP GOES THROUGH `unitsWithin`, AND PHASE 37 IS WHY.
            // The cost of `u` units is quadratic in `u` because the price moves
            // as the trade fills, so dividing the purse by the marginal price
            // over-estimates by ~17% on a trade big enough to matter - which is
            // exactly how `playerBuy` put the player in DEBT for twenty-nine
            // phases. A captain buying is the second caller of that lesson, and
            // it is a worse one: nobody is watching this trade happen.
            const float units = m_economy.unitsWithin(here, best, bestUnits, m_playerCredits);
            if (units > 0.0f) {
                const sim::TradeResult bought = m_economy.buy(here, best, units);
                m_playerCredits -= bought.credits;
                haul.leg.commodity = best;
                haul.leg.cargo = bought.units;
                haul.outlay += bought.credits;
                SOL_LOG_INFO("%s loaded %.0f %s for %.0f cr",
                             captain.name.c_str(),
                             static_cast<double>(bought.units),
                             m_commodityIds[best].c_str(),
                             static_cast<double>(bought.credits));
            }
        }
    }
    // Empty or not, the leg is flown. The order is a RUN between two places,
    // and an empty return is what a hauler on a fixed route actually does - a
    // captain who sat at one end waiting for a margin would be answering stage
    // E's question ("what a captain does when the price never clears") three
    // stages early, and would look identical to a broken one.
    beginCaptainTransit(captain, there);
}

void SpaceWorld::captainArrive(Captain& captain)
{
    CaptainHaul& haul = captain.haul;
    const std::uint32_t market = haul.leg.market;
    settleCaptainSale(captain, market);
    haul.leg.phase = sim::TraderPhase::Idle;
    haul.leg.origin = market; // parked: the leg it just flew is over
    haul.leg.travelRemaining = 0.0;
    haul.leg.legTotal = 0.0;
    // AND THE HULL IS PARKED WHERE IT LANDED, which is what makes "fly to one
    // of them and find them where the screen said they were" a question with an
    // answer: `OwnedShip::storedSystem` is what every other screen in the game
    // already reads to say where a ship of yours is, so a captain's hull answers
    // it through the field that has always meant that.
    const sim::StationMarket& row = m_economy.markets()[market];
    OwnedShip& ship = m_fleet[captain.ship];
    ship.storedSystem = row.systemIndex;
    ship.storedStation = row.stationIndex;
}

void SpaceWorld::rollCaptainAttrition(std::size_t captainIndex, double dt)
{
    Captain& captain = m_captains[captainIndex];
    if (captain.haul.leg.cargo <= 0.0f) {
        return; // an empty hull has nothing to lose, and stage D owns the hull
    }
    const float perSecond = m_factionSim.params().traderLossPerSecond;
    if (perSecond <= 0.0f) {
        return;
    }
    const sim::TraderRoute route = captainRoute(captainIndex);
    // Depart and Arrive only - `FactionSim::attrition`'s rule and its reason:
    // those are exactly the windows a body can exist in, so every loss rolled
    // here is one the player could have flown to and prevented. The gate network
    // is the safe part of a haul because it is honestly nowhere.
    if (route.leg != sim::TraderLeg::Depart && route.leg != sim::TraderLeg::Arrive) {
        return;
    }
    if (route.system >= m_galaxy.systems.size()) {
        return;
    }
    // AND NOT IN A SYSTEM THAT IS BEING SIMULATED. `attrition` skips only the
    // player's own system; this skips every INSTANTIATED one, because since
    // Phase 38 those are not the same set. A captain with a body in a live
    // bubble is being simulated at full fidelity, and rolling a coarse loss
    // against it as well is precisely the failure the phase's risk register
    // names first: a captain that is both things at once.
    //
    // WHAT THAT BUYS IN THIS STAGE IS A SAFE HARBOUR, AND IT IS HONEST RATHER
    // THAN IDEAL: the fine layer cannot kill a captain's hull yet either,
    // because the death path - wreck, insurance, standing, and a log line the
    // player can find - is stage D's. So a promoted captain is currently
    // invulnerable. Better a documented gap than a hull that dies in the sky
    // with nothing that knows how to report it.
    if (systemIsInstantiated(route.system)) {
        return;
    }
    const float risk = m_factionSim.danger(route.system) * perSecond * static_cast<float>(dt);
    if (risk <= 0.0f) {
        return; // quiet system: no roll at all, so peace costs no entropy
    }
    if (m_captainRng.nextFloat01() >= risk) {
        return;
    }
    // THE HOLD IS GONE; THE HULL AND THE PERSON ARE NOT, AND THE LEG GOES ON.
    // `Economy::loseTrader` sends its trader back to the market it left, because
    // there the hauler itself is what was destroyed and the fleet slot is
    // recycled. A captain is somebody the player hired: killing them needs a
    // wreck, an insurance answer, a standing consequence and a line the player
    // can find, and every one of those is stage D. So stage B takes the cargo -
    // which is what makes a route through a contested system a decision - and
    // leaves the ship to the stage that can bury it.
    SOL_LOG_WARN("%s lost their cargo in %s (%.0f cr written off)",
                 captain.name.c_str(),
                 m_galaxy.systems[route.system].name.c_str(),
                 captain.haul.outlay);
    captain.ledger.earned -= captain.haul.outlay;
    captain.haul.outlay = 0.0;
    captain.haul.leg.cargo = 0.0f;
    ++captain.ledger.losses;
}

void SpaceWorld::tickCaptains(double dt)
{
    if (m_captains.empty() || m_economy.markets().empty() || m_defs == nullptr) {
        return;
    }
    // ⚑⚑⚑ THE STATIONARY HALF IS TICKED BY ITS BUBBLE AND NOT BY THIS LOOP, SO
    // ALL THIS OWES IT IS A BUBBLE TO BE TICKED IN (stage C). Done here rather
    // than inside the per-bubble pass for the obvious reason that a bubble which
    // does not exist yet cannot be iterated to, and done before the haul loop so
    // a captain posted this tick is working on the next one rather than the one
    // after.
    openStationaryCaptainBubbles();
    for (std::size_t i = 0; i < m_captains.size(); ++i) {
        Captain& captain = m_captains[i];
        if (captain.order.kind != OrderKind::Haul) {
            continue;
        }
        if (captain.ship >= m_fleet.size()) {
            // Unreachable through any door this file opens - `sellShip` and
            // `switchShip` both refuse a hull somebody is flying - and stood
            // down rather than left pointing at a slot, because the alternative
            // to a guard here is an index that dangles for a whole session.
            captain.order = {};
            continue;
        }
        if (captain.haul.leg.phase == sim::TraderPhase::Idle) {
            if (captain.order.stopping) {
                captain.order = {};
                SOL_LOG_INFO("%s stands down", captain.name.c_str());
                continue;
            }
            const std::uint32_t here = captain.haul.leg.market;
            // The other end of the run. A captain standing at neither - which
            // no path here produces - is sent to A, so a recovered order is a
            // run rather than a stall.
            const std::uint32_t there =
                here == captain.order.marketA ? captain.order.marketB : captain.order.marketA;
            if (here >= m_economy.markets().size() || there >= m_economy.markets().size()) {
                captain.order = {};
                continue;
            }
            captainThink(captain, here, there);
            continue;
        }
        // Being shot at: the delivery waits, the fight does not. `Economy`'s own
        // line, through the same one-tick-lagged flag the trader reconcile
        // writes and the coarse step reads.
        if (i < m_captainDetained.size() && m_captainDetained[i] != 0) {
            continue;
        }
        rollCaptainAttrition(i, dt);
        captain.haul.leg.travelRemaining -= dt;
        if (captain.haul.leg.travelRemaining <= 0.0) {
            captainArrive(captain);
        }
    }
}

void SpaceWorld::applyShipDef(ecs::Registry& registry,
                              std::uint32_t entityIndex,
                              const assets::ShipDef& def,
                              const assets::DefDatabase& defs)
{
    RenderShape& shape = registry.storage<RenderShape>().get(entityIndex);
    shape.scale = {def.scale, def.scale, def.scale};
    shape.model = modelIdFromName(defs, def.model, "ship def", kRoleShip);
    registry.storage<ShipControl>().get(entityIndex).tuning = toShipTuning(def.flight);
    if (isPlayerEntity(registry, entityIndex)) {
        m_playerCargoCapacity = def.cargoCapacity;
        m_scanRange = def.scanRange > 0.0f ? def.scanRange : 1.0f;
        m_scanSpeed = def.scanSpeed > 0.0f ? def.scanSpeed : 1.0f;
        m_collectorRange = def.collectorRange > 0.0f ? def.collectorRange : 1.0f;
        m_signature = std::max(def.signature, kMinSignature);
    }

    ShipPower& power = registry.storage<ShipPower>().get(entityIndex);
    power.tuning.weaponCapacitor = def.power.weaponCapacitor;
    power.tuning.weaponRechargeRate = def.power.weaponRecharge;
    if (power.state.weaponCharge > power.tuning.weaponCapacitor) {
        power.state.weaponCharge = power.tuning.weaponCapacitor;
    }

    // Def edits (and hot-reloads) refit the defenses at full strength.
    ShipDefense& defense = registry.storage<ShipDefense>().get(entityIndex);
    defense.tuning = sim::DefenseTuning{.shieldStrength = def.defense.shieldStrength,
                                        .shieldRegenRate = def.defense.shieldRegen,
                                        .shieldRegenDelay = def.defense.shieldRegenDelay,
                                        .armor = def.defense.armor,
                                        .hull = def.defense.hull};
    sim::resetDefense(defense.state, defense.tuning);

    // ⚑ THE GUN COMES OFF THE MOUNTS NOW (Phase 31 stage B), and `def` here is
    // the RESOLVED def - so this one path serves an NPC hull flying its
    // authored `fit` and the player's ship flying whatever they bought.
    //
    // ⚑⚑ AND SINCE STAGE C1 IT IS EVERY GUN, NOT THE FIRST. Stage B fitted
    // the first weapon-taking mount and carried the rest unfired, saying so
    // out loud because a heuristic that picks a "best" gun is a design
    // decision smuggled into a loop. There is no pick to make now: the hull's
    // mount list IS the armament, in the order the author wrote it, and that
    // order is what decides who fires first when the capacitor runs short.
    // ⚑⚑ EVERY PLACE ON THE HULL FIRST (Phase 31 stage F), because condition
    // is a fact about the MOUNT and not about what is in it - so unlike the
    // two walks below this one skips nothing. An empty engine mount, an
    // internal subsystem bay nobody has authored kit for, a turret ring with
    // no gun on it: each is a piece of ship somebody can shoot at, and each
    // gets an entry here.
    //
    // ⚑ AND IT RESETS TO FULL, exactly as the defences three lines up do and
    // for the same reason - this runs on a refit, a ship swap and a def
    // hot-reload, and a player who has just paid a shipyard for a new fitting
    // has been to a shipyard. That is the repair rule this stage arrives at
    // rather than invents: the game already healed a hull at a refit and this
    // heals the mounts with it.
    ShipMounts& mounts = registry.storage<ShipMounts>().get(entityIndex);
    mounts = ShipMounts{};
    for (const assets::ShipMount& mount : def.mounts) {
        if (mounts.count >= kMaxShipMounts) {
            // Named rather than truncated in silence, on `kMaxShipWeapons`'
            // rule - but the consequence here is different and worth saying
            // out loud: an overflow mount is INDESTRUCTIBLE, because the only
            // alternative that fits in the array would be to reindex, and
            // `ShipWeapon::mount` is an index into the DEF's list.
            SOL_LOG_WARN("ship '%s': more than %u mounts; '%s' and any after it cannot be damaged",
                         def.id.c_str(),
                         kMaxShipMounts,
                         mount.id.c_str());
            break;
        }
        MountCondition& condition = mounts.mounts[mounts.count++];
        condition.at[0] = mount.at[0];
        condition.at[1] = mount.at[1];
        condition.at[2] = mount.at[2];
        condition.external = mount.external;
        condition.kind = mount.kind;
        condition.maxHp = assets::mountHitPoints(mount.size);
        condition.hp = condition.maxHp;
    }

    ShipArmament& armament = registry.storage<ShipArmament>().get(entityIndex);
    armament = ShipArmament{};
    for (std::uint32_t m = 0; m < def.mounts.size(); ++m) {
        const assets::ShipMount& mount = def.mounts[m];
        if (!assets::mountTakesWeapon(mount.kind) || mount.fit.empty()) {
            continue;
        }
        const assets::WeaponDef* weaponDef = defs.findWeapon(mount.fit.c_str());
        if (weaponDef == nullptr) {
            SOL_LOG_WARN("ship '%s': mount '%s' names unknown weapon def '%s'",
                         def.id.c_str(),
                         mount.id.c_str(),
                         mount.fit.c_str());
            continue;
        }
        if (armament.count >= kMaxShipWeapons) {
            // Said once per hull and named, rather than truncated in silence:
            // the ceiling is a fact about the save format (see
            // `kMaxShipWeapons`), and a hull that hits it is content the
            // format has to grow for, not an author's mistake to swallow.
            SOL_LOG_WARN("ship '%s': more than %u fitted weapon mounts; '%s' and any after it are "
                         "carried but will not fire",
                         def.id.c_str(),
                         kMaxShipWeapons,
                         mount.id.c_str());
            break;
        }
        ShipWeapon& weapon = armament.weapons[armament.count++];
        // ⚑ WHICH PLACE ON THE HULL THIS GUN CAME OUT OF (Phase 31 stage C3).
        // Recorded here because here is the only walk that knows: everything
        // downstream sees a flattened component with no def and no mount id,
        // and a second walk to recover it would be a copy of the four skip
        // conditions above waiting to disagree with them.
        weapon.mount = m;
        // ⚑ AND EVERY GUN COMES OUT OF THIS LOOP IN GROUP 1, on every hull,
        // including the player's. A fire group is the pilot's choice, so the
        // saved fit lays it over the top afterwards - see
        // `applyPilotFireGroups`, and the field comment on `ShipWeapon::group`
        // for why an NPC must never carry anything else.
        weapon.kind = weaponDef->kind == "hitscan" ? WeaponKind::Hitscan : WeaponKind::Projectile;
        weapon.damage = weaponDef->damage;
        weapon.rateOfFire = weaponDef->rateOfFire;
        weapon.range = weaponDef->range;
        weapon.projectileSpeed = weaponDef->projectileSpeed;
        weapon.energyCost = weaponDef->energyCost;
        weapon.miningPower = weaponDef->miningPower;
        // ⚑ WHERE THE GUN IS, copied at scale 1 and scaled at the muzzle. A
        // mount with no `at` is INTERNAL (decisions/014 rule 2), which for a
        // gun is a contradiction - you cannot shoot out of a sealed hull - so
        // it keeps the zero default and fires from the hull's centre. That is
        // survivable and visible rather than refused, because refusing it
        // belongs at the def layer where an author can be told about it.
        weapon.at[0] = mount.at[0];
        weapon.at[1] = mount.at[1];
        weapon.at[2] = mount.at[2];
        // ⚑ AND WHICH WAY IT LOOKS (Phase 31 stage C2). Both come off the
        // MOUNT and never off the weapon def, which is what lets one Pulse
        // Cannon be a bolted nose gun on a shuttle and a traversing ring on a
        // freighter without being authored twice - the asymmetry stage B put
        // into `mountAcceptsKind`, now with something reading it.
        weapon.aim[0] = mount.aim[0];
        weapon.aim[1] = mount.aim[1];
        weapon.aim[2] = mount.aim[2];
        weapon.arc = mount.arc;
        if (!mount.external) {
            SOL_LOG_WARN("ship '%s': weapon mount '%s' has no `at`, so its gun fires from the hull "
                         "centre; an armed mount should be external",
                         def.id.c_str(),
                         mount.id.c_str());
        }
        // Resolved here because this is the one place that holds the
        // WeaponDef; the muzzle only ever sees the flattened component.
        //
        // ⚑ TWO MODELS SINCE PHASE 31 STAGE E, and they resolve under
        // different rules on purpose - see `fittingModelOf` for why an unset
        // bolt falls back to its role and an unset gun draws nothing.
        weapon.boltModel = modelOverrideOr(defs, weaponDef->boltModel, "weapon def", kRoleBolt, true);
        weapon.fittingModel = fittingModelOf(defs, weaponDef->model, "weapon def");
    }

    // ⚑⚑ AND EVERYTHING ELSE BOLTED TO THE OUTSIDE (Phase 31 stage E2). The
    // same walk, filtered the other way: a mount that does NOT take a weapon,
    // that is EXTERNAL, and whose fitting names a mesh. Three conditions and
    // each drops a different thing - a gun (it is in the armament above), an
    // internal mount (`decisions/014` rule 2 says it is never drawn), and kit
    // nobody has authored art for (which leaves the mount bare, exactly as an
    // unarted gun does).
    ShipFittings& fittings = registry.storage<ShipFittings>().get(entityIndex);
    fittings = ShipFittings{};
    for (std::uint32_t m = 0; m < def.mounts.size(); ++m) {
        const assets::ShipMount& mount = def.mounts[m];
        if (assets::mountTakesWeapon(mount.kind) || !mount.external || mount.fit.empty()) {
            continue;
        }
        const assets::ComponentDef* component = defs.findComponent(mount.fit.c_str());
        if (component == nullptr) {
            SOL_LOG_WARN("ship '%s': mount '%s' names unknown component def '%s'",
                         def.id.c_str(),
                         mount.id.c_str(),
                         mount.fit.c_str());
            continue;
        }
        const ModelId model = fittingModelOf(defs, component->model, "component def");
        if (model == kNoModel) {
            continue; // authored without a mesh: the mount stays bare
        }
        if (fittings.count >= kMaxDrawnFittings) {
            SOL_LOG_WARN("ship '%s': more than %u drawn external fittings; '%s' and any after it are "
                         "fitted but will not be drawn",
                         def.id.c_str(),
                         kMaxDrawnFittings,
                         mount.id.c_str());
            break;
        }
        FittedPart& part = fittings.parts[fittings.count++];
        part.mount = m;
        part.at[0] = mount.at[0];
        part.at[1] = mount.at[1];
        part.at[2] = mount.at[2];
        part.aim[0] = mount.aim[0];
        part.aim[1] = mount.aim[1];
        part.aim[2] = mount.aim[2];
        part.model = model;
    }
}

// ⚑⚑ WHERE A GUN POINTS, IN ONE PLACE (Phase 31 stage C2). Before this every
// gun on every ship fired down the hull's boresight, which is what one nose
// gun did and what `aim` and `arc` sat in the def file unread for two stages
// waiting to change.
//
// The rule, in the order it is applied:
//
//   1. A gun's REST direction is its mount's `aim`, rotated by the hull. The
//      default `aim` is the ship's own nose, so a hull that authored neither
//      key behaves exactly as it did before C2.
//   2. A gun with no ring (`arc` 0) seeks nothing: it points where it is
//      bolted and fires whenever the trigger is down. That is the shuttle's
//      and the interceptor's nose gun, and the pilot aims it by flying.
//   3. A gun WITH a ring is laid by a gunner. It seeks the ship's target,
//      leading it with its OWN projectile speed - which is not the summary's
//      `leadSpeed`, because that answers a different question for the HUD.
//      With no target it follows the nose, so a trigger held in empty space
//      still fires forward.
//   4. A target it could not reach anyway is not sought. A gun laid on
//      something outside its own range would take a mining beam off the rock
//      in front of it to track a fighter three kilometres away that it cannot
//      touch, and reach is the one fact a gun has about what it can hit.
//   5. Whatever it sought is clamped into the ring, and a gun that cannot
//      bear HOLDS ITS FIRE. Firing into the stop would spend a shot and a
//      slice of capacitor on a bolt that leaves at an angle nobody chose.
bool layGun(const GunneryFrame& frame,
            const ShipWeapon& weapon,
            core::DVec3& outMuzzle,
            core::DVec3& outBearing)
{
    // The muzzle first, because the lead solution is fired FROM it: on a
    // `scale = 4.0` hull a dorsal ring stands six metres off the centreline,
    // and the intercept a turret flies is its own, not the hull's.
    const core::Vec3 offset{static_cast<float>(static_cast<double>(weapon.at[0]) * frame.hullScale),
                            static_cast<float>(static_cast<double>(weapon.at[1]) * frame.hullScale),
                            static_cast<float>(static_cast<double>(weapon.at[2]) * frame.hullScale)};
    outMuzzle = frame.position + toDVec3(rotate(frame.orientation, offset));

    const core::DVec3 rest =
        toDVec3(rotate(frame.orientation, core::Vec3{weapon.aim[0], weapon.aim[1], weapon.aim[2]}));
    if (weapon.arc <= 0.0f) {
        return sim::layWithinArc(rest, rest, 0.0, outBearing);
    }

    core::DVec3 sought = frame.forward;
    if (frame.hasTarget && length(frame.targetPosition - outMuzzle) <= static_cast<double>(weapon.range)) {
        // Hitscan arrives the instant it is fired, so it is laid straight at
        // the target; a bolt is laid where the target is going to be. Passing
        // an enormous speed for the first is what `computeInterceptDirection`
        // already means by instant, and is what the pilot brain does with it.
        const double projectileSpeed = weapon.kind == WeaponKind::Projectile && weapon.projectileSpeed > 1.0f
                                           ? static_cast<double>(weapon.projectileSpeed)
                                           : 1.0e9;
        (void)sim::computeInterceptDirection(
            outMuzzle, frame.velocity, frame.targetPosition, frame.targetVelocity, projectileSpeed, sought);
    }
    return sim::layWithinArc(rest, sought, static_cast<double>(weapon.arc), outBearing);
}

// ⚑⚑ HOW A FITTING STANDS IN ITS MOUNT (Phase 31 stage E). See the header
// for the contract; what is here is the two-step that satisfies it.
//
// `lookAlong` puts the model's nose on the bearing by the SHORTEST arc, which
// leaves the roll about that bearing entirely unconstrained - and a gun is not
// rotationally symmetric about its own barrel. So the second step rolls it
// until the model's +Y is as near the mount's `aim` as it can be, which is
// what stands a dorsal turret up and hangs a ventral one upside down without
// either being authored differently.
//
// ⚑ THE ROLL IS SIGNED AND MEASURED IN THE PLANE PERPENDICULAR TO THE BARREL,
// which is why the angle comes out of an `atan2` of a triple product rather
// than an `acos` of a dot. An unsigned angle would roll a turret the short way
// round exactly half the time and the wrong way the rest.
core::Quat fittingRotation(core::Vec3 bearing, core::Vec3 mountAim)
{
    const float bearingLength = length(bearing);
    if (bearingLength < 1.0e-6f) {
        return core::Quat::identity();
    }
    const core::Vec3 forward = bearing * (1.0f / bearingLength);
    const core::Quat aligned = lookAlong(toDVec3(forward));

    // What is left of the mount's own facing once the part along the barrel is
    // taken out: the piece of it a roll can actually reach.
    const core::Vec3 flattened = mountAim - forward * dot(mountAim, forward);
    const float flattenedLength = length(flattened);
    if (flattenedLength < 1.0e-4f) {
        // Laid straight out of its own ring, so there is no roll to choose and
        // every answer is as good as the next. The shortest arc stands.
        return aligned;
    }
    const core::Vec3 wanted = flattened * (1.0f / flattenedLength);
    const core::Vec3 current = rotate(aligned, core::Vec3{0.0f, 1.0f, 0.0f});
    const float roll = std::atan2(dot(cross(current, wanted), forward), dot(current, wanted));
    return core::fromAxisAngle(forward, roll) * aligned;
}

// ⚑⚑ THE SAME CONSTRUCTION WITH THE OPPOSITE CONSTRAINT (Phase 31 stage
// E2). See the header for why a gun and a pod cannot share one function: a gun
// pins its BARREL and takes what roll is left, a pod pins its MOUNTING FACE and
// takes what roll is left. Here +Y goes exactly onto `aim` and -Z lands as near
// `reference` as a right angle allows.
core::Quat mountRotation(core::Vec3 aim, core::Vec3 reference)
{
    const float aimLength = length(aim);
    if (aimLength < 1.0e-6f) {
        return core::Quat::identity();
    }
    const core::Vec3 up = aim * (1.0f / aimLength);
    // The shortest arc taking the model's +Y onto the way out of the plating.
    const core::Vec3 from{0.0f, 1.0f, 0.0f};
    const float alignment = core::clamp(dot(from, up), -1.0f, 1.0f);
    core::Quat aligned = core::Quat::identity();
    if (alignment < -0.9999f) {
        // Straight down through the hull - a belly mount, which is half the
        // shipped freighter. Any perpendicular axis does for the half turn.
        aligned = core::fromAxisAngle({1.0f, 0.0f, 0.0f}, 3.14159265f);
    } else if (alignment < 0.9999f) {
        aligned = core::fromAxisAngle(normalize(cross(from, up)), std::acos(alignment));
    }

    // Then roll about `aim` until the model's nose is as near `reference` as it
    // gets. Signed, for `fittingRotation`'s reason.
    const core::Vec3 flattened = reference - up * dot(reference, up);
    const float flattenedLength = length(flattened);
    if (flattenedLength < 1.0e-4f) {
        return aligned; // bolted facing the way the hull points: any roll does
    }
    const core::Vec3 wanted = flattened * (1.0f / flattenedLength);
    const core::Vec3 current = rotate(aligned, core::Vec3{0.0f, 0.0f, -1.0f});
    const float roll = std::atan2(dot(cross(current, wanted), up), dot(current, wanted));
    return core::fromAxisAngle(up, roll) * aligned;
}

// ⚑⚑ WHO THE PLAYER IS AT WAR WITH, IN ONE PLACE (promoted out of
// `contactOrder` in Phase 31 stage C2). The contact cycle's threat ranking and
// a turret's decision to open fire are the same question asked twice, and two
// answers to it would be a radar that paints a ship red beside a ring that
// will not shoot it - the "one predicate in one file" rule Phase 30 stage D
// arrived at for `securityAnswers`.
//
// Lowest first: 0 is shooting at you RIGHT NOW, 1 is hostile by standing
// policy, 2 is everybody else. Being shot at beats policy because a patrol
// that has decided to kill you is more urgent than a hostile freighter minding
// its own business three hundred kilometres out.
int SpaceWorld::threatTier(std::uint32_t entityIndex) const
{
    // ⚑⚑ THROUGH THE REGISTRY RATHER THAN THROUGH THE POOL, AND THAT IS NOT A
    // STYLE PREFERENCE. `Registry::storage<T>() const` ASSERTS the pool exists,
    // and a pool exists only once something has been emplaced into it - so a
    // bubble with no piloted ship in it has no `ShipPilot` pool to ask, and
    // asking is a hard assert rather than a null. `Registry::tryGet` answers
    // null for a missing pool, which is the same answer it already gives for a
    // ship that simply has no pilot, so the branch below covers both.
    //
    // ⚑ IT WENT UNREACHED FOR THREE STAGES BECAUSE OF WHEN IT WAS CALLED, NOT
    // BECAUSE IT WAS SAFE. C2 asks this during the firing pass, and by then the
    // pilot system has already created the pool with its own non-const
    // `storage<ShipPilot>()`. Phase 31 stage E asks it while BUILDING A FRAME -
    // which happens before the first tick of a fresh world - and a fixture
    // whose only other ship is a pilotless console spawn takes it down.
    const ShipPilot* pilot =
        playerRegistry().tryGet<ShipPilot>(playerRegistry().entityFromIndex(entityIndex));
    if (pilot == nullptr) {
        return 2; // an inert console spawn: nobody is flying it, so it threatens nothing
    }
    if (pilot->state == PilotState::Attack && pilot->hasTarget != 0 &&
        isPlayerEntity(playerRegistry(), pilot->targetIndex)) {
        return 0;
    }
    // ⚑⚑⚑ A HULL OF YOURS IS NEVER A THREAT TO YOU, AND BEFORE STAGE D IT
    // COULD BE. A captain's freighter wore the local owner's colours, so
    // flying your own trade route through a government you had angered painted
    // YOUR OWN SHIP red on the radar - and this function is deliberately the
    // same one a turret asks before opening fire, so the ring would have
    // agreed with the radar and shot it. Asked of the thing, above the faction
    // number, because the faction number is exactly what was wrong.
    if (playerOwnedHull(playerRegistry(), entityIndex)) {
        return 2;
    }
    // An unaffiliated console spawn has no faction to consult and Lua treats
    // it as unconditionally player-hostile (the pre-8b rule).
    if (pilot->factionIndex >= m_factionTable.size()) {
        return kHostileThreatTier;
    }
    return m_factionSim.playerHostile(pilot->factionIndex) ? kHostileThreatTier : 2;
}

GunneryFrame SpaceWorld::gunneryFrame(const ecs::Registry& registry, std::uint32_t entityIndex) const
{
    GunneryFrame frame;
    const Transform& transform = registry.storage<Transform>().get(entityIndex);
    frame.position = transform.position;
    frame.orientation = transform.orientation;
    frame.forward = toDVec3(rotate(transform.orientation, core::Vec3{0.0f, 0.0f, -1.0f}));
    if (const FlightBody* body = playerRegistry().storage<FlightBody>().tryGet(entityIndex);
        body != nullptr) {
        frame.velocity = body->velocity;
    }
    if (const RenderShape* shape = playerRegistry().storage<RenderShape>().tryGet(entityIndex);
        shape != nullptr) {
        frame.hullScale = static_cast<double>(shape->scale.x);
    }

    // ⚑ WHOSE TARGET, AND THE TWO ANSWERS ARE DELIBERATELY DIFFERENT SOURCES.
    // The player's guns follow the SELECTION - the thing the HUD is showing a
    // shield readout for, which is the only "what am I shooting at" this game
    // has ever had. An NPC's follow its pilot's, which Lua chose. There is no
    // third case: a ship with neither has no target, and its turrets look down
    // the nose.
    //
    // ⚑⚑ AND THE PLAYER'S SELECTION HAS TO BE HOSTILE, WHICH IS A RULED
    // DECISION AND NOT AN OBVIOUS ONE. Laying on the bare selection was the
    // simpler rule and it makes a trap the game had never had: hail a patrol,
    // forget to change the selection, hold the trigger to cut a rock, and a
    // dorsal ring puts a bolt into the police while your nose is on the
    // asteroid. A ring is a gunner, and a gunner does not open on someone you
    // are not at war with.
    //
    // ⚑ What that buys is a shape rather than just a safety: you OPEN with the
    // nose, and the rings join once it is a fight. `threatTier` is read live
    // every tick, so the moment a neutral you shot at starts shooting back it
    // is tier 0 and every ring on the hull comes round onto it.
    //
    // An NPC needs no such gate: its pilot's target IS its enemy, chosen by
    // the Lua brain that decided to attack.
    std::uint32_t targetIndex = kNoIndex;
    if (isPlayerEntity(playerRegistry(), entityIndex)) {
        targetIndex = targetShipEntityIndex();
        if (targetIndex != kNoIndex && threatTier(targetIndex) > kHostileThreatTier) {
            targetIndex = kNoIndex;
        }
        // Through the registry rather than through the pool - see `threatTier`
        // for why a const reader of this pool must tolerate its absence.
    } else if (const ShipPilot* pilot =
                   playerRegistry().tryGet<ShipPilot>(playerRegistry().entityFromIndex(entityIndex));
               pilot != nullptr && pilot->hasTarget != 0) {
        targetIndex = pilot->targetIndex;
    }
    if (targetIndex == kNoIndex) {
        return frame;
    }
    const Transform* targetTransform = playerRegistry().storage<Transform>().tryGet(targetIndex);
    if (targetTransform == nullptr) {
        return frame;
    }
    frame.hasTarget = true;
    frame.targetPosition = targetTransform->position;
    if (const FlightBody* body = playerRegistry().storage<FlightBody>().tryGet(targetIndex);
        body != nullptr) {
        frame.targetVelocity = body->velocity;
    }
    return frame;
}

std::uint32_t SpaceWorld::targetShipEntityIndex() const
{
    const std::size_t total = m_targets.size() + playerShips().size();
    if (total == 0) {
        return kNoIndex;
    }
    // The same wrap every other reader of the selection applies, because a
    // targeted ship can die out from under m_targetIndex.
    const std::size_t index = m_targetIndex % total;
    if (index < m_targets.size()) {
        return kNoIndex; // a station, a planet, a gate, a field: not a gunnery target
    }
    const ecs::Entity ship = playerShips()[index - m_targets.size()].entity;
    return playerRegistry().isAlive(ship) ? ship.index : kNoIndex;
}

// Which triggers this ship's guns are spread across (Phase 31 stage C3).
// ⚑⚑⚑ "SHOOT A FREIGHTER'S DRIVE OFF AND WATCH IT STOP, STILL ALIVE" - Phase
// 31's own exit criterion, and this counting loop is the half of it that says
// what "off" means. The share of a hull's engine mounts still standing is what
// its main drive can still push with, so one drive of two leaves half the
// acceleration and the only drive there is leaves none.
//
// ⚑ IT IS A COUNT OF MOUNTS AND NOT OF FITTINGS. No `[[ship.mount]]` of kind
// `engine` in this game carries a `fit` - a drive bell is part of the hull -
// which is exactly why condition lives on the mount, and why this can be asked
// of a hull nobody has outfitted.
float driveFraction(const ShipMounts& mounts)
{
    std::uint32_t total = 0;
    std::uint32_t standing = 0;
    for (std::uint32_t m = 0; m < mounts.count; ++m) {
        if (mounts.mounts[m].kind != assets::MountKind::Engine) {
            continue;
        }
        ++total;
        standing += mounts.mounts[m].destroyed() ? 0u : 1u;
    }
    return total == 0 ? 1.0f : static_cast<float>(standing) / static_cast<float>(total);
}

void repairMounts(ShipMounts& mounts)
{
    for (std::uint32_t m = 0; m < mounts.count; ++m) {
        mounts.mounts[m].hp = mounts.mounts[m].maxHp;
    }
}

bool shieldsArePowered(const ShipMounts& mounts)
{
    bool any = false;
    for (std::uint32_t m = 0; m < mounts.count; ++m) {
        if (mounts.mounts[m].kind != assets::MountKind::Shield) {
            continue;
        }
        if (!mounts.mounts[m].destroyed()) {
            return true;
        }
        any = true;
    }
    return !any;
}

std::uint32_t fireGroupsInUse(const ShipArmament& armament)
{
    std::uint32_t mask = 0;
    for (std::uint32_t i = 0; i < armament.count; ++i) {
        const ShipWeapon& weapon = armament.weapons[i];
        if (weapon.kind == WeaponKind::None || weapon.group < 1 || weapon.group > kFireGroupCount) {
            continue;
        }
        mask |= 1u << (weapon.group - 1);
    }
    return mask;
}

// ⚑ A SELECTION THAT POINTS AT NOTHING IS THE ONE FAILURE THIS FEATURE CAN
// PRODUCE ON ITS OWN, and it produces it two ways: a refit that removes the
// last gun in the selected group, and a regroup that moves it out. Either
// leaves a trigger wired to nothing, which reads exactly like a broken gun -
// so the selection is walked back to the lowest group that has one.
//
// An unarmed ship keeps group 1: there is nothing to point at, and leaving the
// number alone means fitting a gun later finds the selection where it was.
void normalizeFireGroup(ShipArmament& armament)
{
    const std::uint32_t mask = fireGroupsInUse(armament);
    if (mask == 0 || (armament.selectedGroup >= 1 && armament.selectedGroup <= kFireGroupCount &&
                      (mask & (1u << (armament.selectedGroup - 1))) != 0)) {
        return;
    }
    for (std::uint32_t group = 1; group <= kFireGroupCount; ++group) {
        if ((mask & (1u << (group - 1))) != 0) {
            armament.selectedGroup = group;
            return;
        }
    }
}

// ⚑ ONE WALK OF THE GUNS, and every caller outside the firing pass goes
// through it (Phase 31 stage C1). The four questions it answers used to be
// four reads of the single `ShipWeapon`, spread across the pilot brain, the
// HUD's lead marker and the prospecting readout - and with guns plural each
// of them has a different right answer, so leaving them to pick a gun each
// would be three heuristics nobody wrote down.
ArmamentSummary SpaceWorld::armamentSummary(const ecs::Registry& registry, std::uint32_t entityIndex) const
{
    ArmamentSummary summary;
    const ShipArmament* armament = registry.storage<ShipArmament>().tryGet(entityIndex);
    if (armament == nullptr) {
        return summary;
    }
    for (std::uint32_t i = 0; i < armament->count; ++i) {
        const ShipWeapon& weapon = armament->weapons[i];
        // ⚑ THE SELECTED GROUP ONLY (Phase 31 stage C3). Every field below is
        // read to predict what the trigger will do, and a gun in an unselected
        // group is not going to do anything - so counting its reach here would
        // draw a lead marker for a bolt that is not coming and light a mining
        // prompt for a beam the trigger is not wired to.
        if (weapon.kind == WeaponKind::None || weapon.group != armament->selectedGroup) {
            continue;
        }
        summary.armed = true;
        summary.maxRange = std::max(summary.maxRange, weapon.range);
        // ⚑ THE FIRST PROJECTILE GUN THE PILOT HAS TO AIM (Phase 31 stage
        // C2 narrowed this from stage C1's "the first projectile gun"). The
        // lead marker's whole job is to say where to point the NOSE, and a
        // turret does not care where the nose points - it lays itself, with
        // its own speed, on the same target. A marker taken off a ring is
        // therefore an instruction about a gun that was never listening.
        //
        // A hull whose every projectile gun traverses has no marker at all,
        // and that is the honest answer rather than a gap: there is nothing
        // the pilot could do with one.
        if (summary.leadSpeed <= 0.0f && weapon.kind == WeaponKind::Projectile && weapon.arc <= 0.0f) {
            summary.leadSpeed = weapon.projectileSpeed;
        }
        if (weapon.miningPower > 0.0f) {
            summary.canMine = true;
            // ⚑ The furthest MINING beam, which is not the furthest gun. A
            // ship with a 3 km cannon and an 800 m laser can cut at 800 m,
            // and a max taken over all guns would say 3 km.
            summary.miningRange = std::max(summary.miningRange, weapon.range);
        }
    }
    return summary;
}

ecs::Entity SpaceWorld::spawnShipAt(SystemBubble& bubble,
                                    const assets::ShipDef& def,
                                    const assets::DefDatabase& defs,
                                    const core::DVec3& position,
                                    const char* factionName)
{
    ecs::Registry& registry = bubble.registry;
    const ecs::Entity e = registry.create();
    registry.emplace<Transform>(e, Transform{.position = position, .previousPosition = position});
    registry.emplace<RenderShape>(e, RenderShape{});
    registry.emplace<FlightBody>(e);
    // Default input is assist-on with zero commands = station-keeping until a
    // pilot (Phase 6 AI) writes real commands.
    registry.emplace<ShipControl>(e);
    registry.emplace<ShipPower>(e);
    registry.emplace<ShipDefense>(e);
    registry.emplace<ShipArmament>(e);
    registry.emplace<ShipFittings>(e);
    registry.emplace<ShipMounts>(e);
    applyShipDef(registry, e.index, def, defs);
    std::string name = def.name;
    if (factionName != nullptr && factionName[0] != '\0') {
        name += std::string(" (") + factionName + ")";
    }
    // ⚑ Into THIS system's ship list. An entity index is issued per registry
    // and every registry starts at zero, so a world-scoped list keyed on the
    // index alone would hand one system's death the other system's ship def.
    bubble.spawnedShips.push_back({.entity = e, .defId = def.id, .name = std::move(name)});
    return e;
}

ecs::Entity SpaceWorld::spawnShipFromDef(const assets::ShipDef& def, const assets::DefDatabase& defs)
{
    const sim::ShipState player = shipState();
    const core::Vec3 forward = rotate(player.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
    const double distance = 150.0 + 100.0 * static_cast<double>(def.scale);
    const core::DVec3 position =
        player.position + core::DVec3{forward.x * distance, forward.y * distance, forward.z * distance};
    const ecs::Entity e = spawnShipAt(playerBubble(), def, defs, position, nullptr);
    Transform& transform = playerRegistry().storage<Transform>().get(e.index);
    transform.orientation = player.orientation;
    transform.previousOrientation = player.orientation;
    return e;
}

TargetInfo SpaceWorld::currentTargetInfo() const
{
    const std::size_t total = m_targets.size() + playerShips().size();
    // m_targetIndex can go stale when a targeted ship dies; wrap it here.
    const std::size_t index = total > 0 ? m_targetIndex % total : 0;

    if (index < m_targets.size()) {
        TargetInfo info;
        info.nav = m_targets[index];
        return info;
    }
    return contactInfo(index - m_targets.size());
}

TargetInfo SpaceWorld::contactInfo(std::size_t shipSlot) const
{
    TargetInfo info;
    if (shipSlot >= playerShips().size()) {
        return info;
    }
    const SpawnedShip& ship = playerShips()[shipSlot];
    const Transform& transform = playerRegistry().storage<Transform>().get(ship.entity.index);
    info.nav = NavTarget{.name = ship.name, .position = transform.position, .surfaceRadius = 0.0};
    info.isShip = true;
    info.velocity = playerRegistry().storage<FlightBody>().get(ship.entity.index).velocity;
    if (const ShipPilot* pilot = playerRegistry().tryGet<ShipPilot>(ship.entity);
        pilot != nullptr && pilot->factionIndex < m_factionTable.size()) {
        info.factionName = m_factionTable[pilot->factionIndex].name;
        info.attitude = playerAttitudeName(pilot->factionIndex);
    }
    if (const ShipDefense* defense = playerRegistry().tryGet<ShipDefense>(ship.entity)) {
        const float strength = defense->tuning.shieldStrength > 0.0f ? defense->tuning.shieldStrength : 1.0f;
        info.shieldFore = defense->state.shieldFore / strength;
        info.shieldAft = defense->state.shieldAft / strength;
        info.hull = defense->tuning.hull > 0.0f ? defense->state.hull / defense->tuning.hull : 0.0f;
    }
    return info;
}

SpaceWorld::NavKind SpaceWorld::navTargetKind(std::size_t index) const
{
    const std::size_t gateBase = m_planetTargetBase - m_gates.size();
    if (index < gateBase) {
        return NavKind::Station;
    }
    if (index < m_planetTargetBase) {
        return NavKind::Gate;
    }
    if (index < m_planetTargetBase + planets().size()) {
        return NavKind::Planet;
    }
    if (index == m_starTargetIndex) {
        return NavKind::Star;
    }
    const std::size_t slot = index - m_signalTargetBase;
    return index >= m_signalTargetBase && slot < m_dynamicTargets.size() ? m_dynamicTargets[slot].kind
                                                                         : NavKind::Signal;
}

std::uint32_t SpaceWorld::navTargetStation(std::size_t index) const
{
    // Stations lead the static head, so a slot is a station exactly when it
    // sits before the gates begin.
    const std::size_t gateBase = m_planetTargetBase - m_gates.size();
    return index < gateBase ? static_cast<std::uint32_t>(index) : kNoIndex;
}

std::uint32_t SpaceWorld::navTargetGate(std::size_t index) const
{
    const std::size_t gateBase = m_planetTargetBase - m_gates.size();
    return index >= gateBase && index < m_planetTargetBase ? static_cast<std::uint32_t>(index - gateBase)
                                                           : kNoIndex;
}

SpaceWorld::NavKnowledge SpaceWorld::navKnowledge(std::size_t index) const
{
    // ⚑ Only the static head's stations and gates are fogged (Phase 8z).
    //
    // The star and the planets are AU-scale scenery visible from the rim, and
    // the user's own ruling is that arrival still hands them over. The whole
    // dynamic tail is already knowledge-gated by construction and says so where
    // it is built: a signal enters it only once discovered, a wreck exists
    // because something died in front of you, a bookmark because you wrote it,
    // a berth because you were cleared for it, an objective because you took
    // the contract, and a field needs no finding at all.
    const std::uint32_t station = navTargetStation(index);
    if (station != kNoIndex) {
        return m_survey.stationIdentified(m_currentSystem, station)   ? NavKnowledge::Identified
               : m_survey.stationDiscovered(m_currentSystem, station) ? NavKnowledge::Contact
                                                                      : NavKnowledge::Hidden;
    }
    const std::uint32_t gate = navTargetGate(index);
    if (gate != kNoIndex) {
        return m_survey.gateIdentified(m_currentSystem, gate)   ? NavKnowledge::Identified
               : m_survey.gateDiscovered(m_currentSystem, gate) ? NavKnowledge::Contact
                                                                : NavKnowledge::Hidden;
    }
    return NavKnowledge::Identified;
}

SpaceWorld::NavKind SpaceWorld::navTargetDrawKind(std::size_t index) const
{
    // An unidentified station or gate wears the contact glyph, so the shape on
    // the radar and the map never says what the name is withholding.
    return navKnowledge(index) == NavKnowledge::Contact ? NavKind::Signal : navTargetKind(index);
}

void SpaceWorld::snapSelectionToVisible()
{
    if (m_targets.empty() || m_targetIndex >= m_targets.size() || navTargetVisible(m_targetIndex)) {
        return; // already fine, or already on a ship rather than a nav slot
    }
    for (std::size_t step = 1; step <= m_targets.size(); ++step) {
        const std::size_t slot = (m_targetIndex + step) % m_targets.size();
        if (navTargetVisible(slot)) {
            m_targetIndex = slot;
            m_navSlot = slot;
            return;
        }
    }
    // Cannot happen in a generated galaxy — the star is never hidden — but a
    // hand-built system with nothing visible must not leave a stale selection.
    m_targetIndex = 0;
    m_navSlot = 0;
}

bool SpaceWorld::identifyStructure(std::size_t index)
{
    const std::uint32_t station = navTargetStation(index);
    const std::uint32_t gate = navTargetGate(index);
    bool changed = false;
    if (station != kNoIndex) {
        changed = m_survey.notifyStationIdentified(m_galaxy, m_currentSystem, station);
    } else if (gate != kNoIndex) {
        changed = m_survey.notifyGateIdentified(m_galaxy, m_currentSystem, gate);
    }
    if (changed) {
        refreshStaticTargetNames();
    }
    return changed;
}

void SpaceWorld::refreshStaticTargetNames()
{
    if (m_currentSystem >= m_galaxy.systems.size()) {
        return;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const std::size_t gateBase = m_planetTargetBase - m_gates.size();
    for (std::size_t i = 0; i < gateBase && i < m_targets.size(); ++i) {
        const std::uint32_t station = static_cast<std::uint32_t>(i);
        m_targets[i].name = m_survey.stationIdentified(m_currentSystem, station) ? spec.stations[station].name
                                                                                 : anonymousContactName(i);
    }
    for (std::size_t i = gateBase; i < m_planetTargetBase && i < m_targets.size(); ++i) {
        const std::uint32_t gate = static_cast<std::uint32_t>(i - gateBase);
        // A gate's name carries its destination, which is precisely what
        // identifying it buys — so an unidentified one must not show it.
        m_targets[i].name =
            m_survey.gateIdentified(m_currentSystem, gate) ? m_gates[gate].name : anonymousContactName(i);
    }
}

std::uint32_t SpaceWorld::navTargetBody(std::size_t index) const
{
    if (index == m_starTargetIndex) {
        return 0;
    }
    if (index >= m_planetTargetBase && index < m_planetTargetBase + planets().size()) {
        return static_cast<std::uint32_t>(index - m_planetTargetBase + 1);
    }
    return kNoIndex;
}

namespace {

// One accessor per dynamic kind: a slot only answers for what it actually is.
[[nodiscard]] std::uint32_t
slotIndexOfKind(SpaceWorld::NavKind want, SpaceWorld::NavKind got, std::uint32_t index)
{
    return want == got ? index : 0xffff'ffffu;
}

} // namespace

std::uint32_t SpaceWorld::navTargetSignal(std::size_t index) const
{
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slotIndexOfKind(NavKind::Signal, slot.kind, slot.index);
}

std::uint32_t SpaceWorld::navTargetField(std::size_t index) const
{
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slotIndexOfKind(NavKind::Field, slot.kind, slot.index);
}

std::uint32_t SpaceWorld::navTargetWreck(std::size_t index) const
{
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slotIndexOfKind(NavKind::Wreck, slot.kind, slot.index);
}

std::uint32_t SpaceWorld::navTargetBookmark(std::size_t index) const
{
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slotIndexOfKind(NavKind::Bookmark, slot.kind, slot.index);
}

// --- Bookmarks (Phase 8h) ----------------------------------------------------

std::string SpaceWorld::suggestBookmarkName(const core::DVec3& position) const
{
    // Named from the nearest thing that already has a name, which is what a
    // person would write down: "3.4 Mm from Ceres" beats "Bookmark 4".
    const NavTarget* nearest = nullptr;
    double bestSquared = 0.0;
    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        // Never name a bookmark after another bookmark: the result is circular
        // ("At * Rich Rock"), it carries the display prefix, and what the
        // player wants is the name of a PLACE, not of their own earlier note.
        if (navTargetKind(i) == NavKind::Bookmark) {
            continue;
        }
        const core::DVec3 offset = m_targets[i].position - position;
        const double squared = dot(offset, offset);
        if (nearest == nullptr || squared < bestSquared) {
            nearest = &m_targets[i];
            bestSquared = squared;
        }
    }
    if (nearest == nullptr) {
        return "Waypoint";
    }
    const double meters = std::sqrt(bestSquared);
    char buffer[96] = {};
    if (meters < 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "At %s", nearest->name.c_str());
    } else if (meters < 1.0e6) {
        std::snprintf(buffer, sizeof(buffer), "%.0f km from %s", meters / 1000.0, nearest->name.c_str());
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f Mm from %s", meters / 1.0e6, nearest->name.c_str());
    }
    return buffer;
}

bool SpaceWorld::addBookmarkAt(const core::DVec3& position, const std::string& name)
{
    const std::string chosen = name.empty() ? suggestBookmarkName(position) : name;
    if (m_survey.addBookmark(m_currentSystem, position, chosen, 0, m_worldSeconds) == 0) {
        return false; // this system is at its cap
    }
    rebuildDynamicTargets();
    return true;
}

bool SpaceWorld::addBookmarkHere(const std::string& name)
{
    return addBookmarkAt(playerRegistry().storage<Transform>().get(playerEntityIndex()).position, name);
}

bool SpaceWorld::removeBookmark(std::uint32_t id)
{
    if (!m_survey.removeBookmark(id)) {
        return false;
    }
    // The nav tail compacts through the same path a decayed wreck takes, so
    // the player's selection and any scan in flight follow it rather than
    // silently pointing at whatever moved into the slot.
    rebuildDynamicTargets();
    return true;
}

bool SpaceWorld::selectBookmark(std::uint32_t id)
{
    for (std::size_t i = m_signalTargetBase; i < m_targets.size(); ++i) {
        if (navTargetBookmark(i) == id) {
            return selectTarget(i);
        }
    }
    return false;
}

std::size_t SpaceWorld::currentTargetIndex() const
{
    const std::size_t total = m_targets.size() + playerShips().size();
    return total > 0 ? m_targetIndex % total : 0;
}

bool SpaceWorld::selectTarget(std::size_t index)
{
    if (index >= m_targets.size() + playerShips().size()) {
        return false;
    }
    // ⚑ Phase 8z: the one choke point every outright selection goes through —
    // the map's Set Target, the click pick, and the console's sol.target. A
    // lever that could select an undiscovered station would be able to reach a
    // state the game cannot, which is 8u's rule, and it would hand the fog's
    // one secret to anything downstream that reads the selection.
    if (index < m_targets.size() && !navTargetVisible(index)) {
        return false;
    }
    m_targetIndex = index;
    // Selecting outright (the map's Set Target, a console call) also updates
    // that class's remembered slot, so a later T or C resumes from what the
    // player actually chose rather than from a stale cycle position.
    if (index < m_targets.size()) {
        m_navSlot = index;
    } else {
        m_contactSlot = index - m_targets.size();
    }
    return true;
}

void SpaceWorld::cycleNavTarget(int step)
{
    if (m_targets.empty()) {
        return;
    }
    // ⚑ Backwards is `+ size - 1`, never `- 1`: these are size_t, so stepping
    // below zero wraps to a value no modulo brings back.
    const std::size_t advance = step < 0 ? m_targets.size() - 1 : 1;
    // Already on a nav point: step to the next one. Coming back from the
    // contact cycle: return to where this class left off, so switching
    // classes costs one press rather than a walk back around the list.
    std::size_t slot = m_targetIndex < m_targets.size() ? (m_targetIndex + advance) % m_targets.size()
                                                        : m_navSlot % m_targets.size();
    // Phase 8z: walk past what has not been found yet. The list still holds
    // every station and gate — it is world state and NPCs anchor to it — so the
    // cycle is where the player stops seeing them. A full lap finding nothing
    // leaves the selection alone rather than parking it on a hidden slot, which
    // is what keeps every downstream consumer (autopilot, hail, dock request,
    // the scan) free of a fog check of its own.
    for (std::size_t walked = 0; walked < m_targets.size(); ++walked) {
        if (navTargetVisible(slot)) {
            m_navSlot = slot;
            m_targetIndex = slot;
            return;
        }
        slot = (slot + advance) % m_targets.size();
    }
}

void SpaceWorld::contactOrder(std::vector<std::size_t>& out) const
{
    std::vector<int> tiers;
    contactOrder(out, tiers);
}

void SpaceWorld::contactOrder(std::vector<std::size_t>& out, std::vector<int>& tiers) const
{
    out.clear();
    tiers.clear();
    if (playerShips().empty()) {
        return;
    }
    const core::DVec3 playerPosition =
        playerRegistry().storage<Transform>().get(playerEntityIndex()).position;

    // The tiering moved out to `threatTier` in Phase 31 stage C2, because a
    // turret asks the same question and two answers to it would be a radar
    // painting a ship red beside a ring that will not shoot it.
    auto tierOf = [&](const SpawnedShip& ship) { return threatTier(ship.entity.index); };

    struct Ranked
    {
        std::size_t slot;
        int tier;
        double distanceSquared;
    };

    std::vector<Ranked> ranked;
    ranked.reserve(playerShips().size());
    for (std::size_t i = 0; i < playerShips().size(); ++i) {
        const SpawnedShip& ship = playerShips()[i];
        const core::DVec3 offset =
            playerRegistry().storage<Transform>().get(ship.entity.index).position - playerPosition;
        ranked.push_back({.slot = i, .tier = tierOf(ship), .distanceSquared = dot(offset, offset)});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.tier != b.tier ? a.tier < b.tier : a.distanceSquared < b.distanceSquared;
    });
    out.reserve(ranked.size());
    tiers.reserve(ranked.size());
    for (const Ranked& entry : ranked) {
        out.push_back(entry.slot);
        tiers.push_back(entry.tier);
    }
}

void SpaceWorld::cycleContact(int step)
{
    std::vector<std::size_t> order;
    contactOrder(order);
    if (order.empty()) {
        return;
    }
    // Phase 15: the step walks the RANKING, not the slot — the order is
    // threat-then-distance and stepping raw ship slots would wander through it
    // arbitrarily. Backwards is `+ size - 1` for the size_t reason above.
    const std::size_t advance = step < 0 ? order.size() - 1 : 1;
    // Coming from a nav target, the first press lands on the head of the
    // threat order — the thing shooting at you, which is the whole point of
    // giving contacts their own key. Backwards from a nav target lands on the
    // TAIL, mirroring that, so a back-press after a forward-press returns
    // where the player already was instead of skipping the list's far end.
    // Already on a ship, step along the order from wherever the current one
    // sits in it.
    std::size_t next = step < 0 ? order.size() - 1 : 0;
    if (m_targetIndex >= m_targets.size()) {
        const std::size_t current = m_targetIndex - m_targets.size();
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i] == current) {
                next = (i + advance) % order.size();
                break;
            }
        }
    }
    m_contactSlot = order[next];
    m_targetIndex = m_targets.size() + m_contactSlot;
}

std::size_t SpaceWorld::objectiveTargetIndex() const
{
    for (std::size_t slot = 0; slot < m_dynamicTargets.size(); ++slot) {
        if (m_dynamicTargets[slot].kind == NavKind::Objective) {
            return m_signalTargetBase + slot;
        }
    }
    return kNoTarget;
}

bool SpaceWorld::selectObjective()
{
    // Selected outright rather than cycled to: the whole point of the item is
    // that the player never has to hunt for where they were sent, and hunting
    // through twenty nav slots is the same complaint one level down.
    const std::size_t index = objectiveTargetIndex();
    return index != kNoTarget && selectTarget(index);
}

bool SpaceWorld::selectNearestHostile()
{
    std::vector<std::size_t> order;
    std::vector<int> tiers;
    contactOrder(order, tiers);
    // The order is already threat-then-distance, so the nearest hostile is its
    // head whenever the head is hostile at all — anything else means there is
    // nothing hostile in the system to jump to.
    if (order.empty() || tiers[0] > 1) {
        return false;
    }
    return selectTarget(m_targets.size() + order[0]);
}

bool SpaceWorld::targetShipByName(const char* namePart)
{
    for (std::size_t i = 0; i < playerShips().size(); ++i) {
        if (playerShips()[i].name.find(namePart) != std::string::npos) {
            m_contactSlot = i;
            m_targetIndex = m_targets.size() + i;
            return true;
        }
    }
    return false;
}

ecs::Entity SpaceWorld::spawnPilotFromDef(const assets::ShipDef& def,
                                          const assets::DefDatabase& defs,
                                          PilotRole role,
                                          std::uint32_t factionIndex)
{
    const ecs::Entity e = spawnShipFromDef(def, defs);
    playerRegistry().emplace<ShipPilot>(e, ShipPilot{.role = role, .factionIndex = factionIndex});
    if (factionIndex < m_factionTable.size()) {
        playerShips().back().name = def.name + " (" + m_factionTable[factionIndex].name + ")";
    }
    return e;
}

namespace {

// Role/state pip policies (decisions/003 consequence: simple per-role triage).
sim::PowerPips pipsForPilot(PilotState state)
{
    switch (state) {
    case PilotState::Attack:
        return {3, 2, 1};
    case PilotState::Flee:
        return {0, 4, 2};
    case PilotState::Idle:
    case PilotState::Patrol:
    case PilotState::Travel:
    // An inspection is not a fight and must not read as the run-up to one:
    // shifting to weapons pips would light the target's threat readout on a
    // ship that has come to look at your cargo.
    case PilotState::Inspect:
        break;
    }
    return {2, 2, 2};
}

} // namespace

bool SpaceWorld::pilotAttackPlayer(ecs::Entity entity)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Attack;
    pilot->targetIndex = playerEntityIndex();
    pilot->hasTarget = 1;
    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotEngageEnemy(ecs::Entity entity)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr || pilot->factionIndex >= m_factionTable.size()) {
        return false;
    }
    constexpr double kSensorRange = 8.0e4; // meters
    const core::DVec3 self = playerRegistry().storage<Transform>().get(entity.index).position;

    std::uint32_t bestTarget = kNoIndex;
    double bestDistance = kSensorRange;
    const auto consider = [&](std::uint32_t targetIndex) {
        const Transform* transform = playerRegistry().storage<Transform>().tryGet(targetIndex);
        const ShipDefense* defense = playerRegistry().storage<ShipDefense>().tryGet(targetIndex);
        if (transform == nullptr || defense == nullptr || !defense->state.alive()) {
            return;
        }
        const double distance = length(transform->position - self);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestTarget = targetIndex;
        }
    };

    const ecs::Pool<ShipPilot>& pilots = playerRegistry().storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const std::uint32_t otherIndex = pilots.entityIndices()[i];
        const std::uint32_t otherFaction = pilots.values()[i].factionIndex;
        if (otherIndex == entity.index || otherFaction >= m_factionTable.size() ||
            !m_factionSim.atWar(pilot->factionIndex, otherFaction)) {
            continue;
        }
        consider(otherIndex);
    }
    if (m_factionSim.playerHostile(pilot->factionIndex) && !isDocked()) {
        consider(playerEntityIndex());
    }
    if (bestTarget == kNoIndex) {
        return false;
    }
    pilot->state = PilotState::Attack;
    pilot->targetIndex = bestTarget;
    pilot->hasTarget = 1;
    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotUnderFire(ecs::Entity entity) const
{
    const ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    return pilot != nullptr && pilot->threatTimer > 0.0f;
}

bool SpaceWorld::pilotEngageThreat(ecs::Entity entity)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr || pilot->threatTimer <= 0.0f) {
        return false;
    }
    // The threat is a remembered entity index rather than a search result, so
    // it has to be re-checked before it is flown at: the ship that shot us six
    // seconds ago may be dead, and an index outliving its entity is how a
    // pilot ends up attacking whatever was spawned into the slot next.
    const ShipDefense* defense = playerRegistry().storage<ShipDefense>().tryGet(pilot->threatIndex);
    if (defense == nullptr || !defense->state.alive() ||
        playerRegistry().storage<Transform>().tryGet(pilot->threatIndex) == nullptr) {
        pilot->threatTimer = 0.0f;
        return false;
    }
    pilot->state = PilotState::Attack;
    pilot->targetIndex = pilot->threatIndex;
    pilot->hasTarget = 1;
    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotHuntTrader(ecs::Entity entity)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    const Transform* transform = pilot != nullptr ? playerRegistry().tryGet<Transform>(entity) : nullptr;
    const ShipControl* control = pilot != nullptr ? playerRegistry().tryGet<ShipControl>(entity) : nullptr;
    if (pilot == nullptr || transform == nullptr || control == nullptr ||
        pilot->factionIndex >= m_factionTable.size()) {
        return false;
    }

    // Who this hunter would attack, in one row. ⚑ The test is the coarse
    // layer's own: FactionSim::raidCandidates picks a system to raid by "at
    // war with, or relations below hostile", and a raider in the bubble
    // deciding whose freighter to burn is the same judgement one level down.
    // Deriving it here rather than inventing a predation-specific rule is what
    // keeps a raid the player watches consistent with a raid they only read
    // about on the map.
    m_preyHostile.assign(m_factionTable.size(), 0);
    for (std::uint32_t other = 0; other < m_factionTable.size(); ++other) {
        const bool hostile =
            m_factionSim.atWar(pilot->factionIndex, other) ||
            m_factionSim.relation(pilot->factionIndex, other) < m_factionSim.params().hostileThreshold;
        m_preyHostile[other] = hostile ? 1u : 0u;
    }
    // ⚑⚑⚑⚑ AND ONE ROW MORE THAN THE FACTION TABLE HAS, WHICH IS THE PHASE
    // SPEC'S OWN DIAGNOSIS ANSWERED WHERE IT COSTS ONE LINE (stage D). "There
    // is no player faction row" is true and stays true: this row lives for the
    // length of this function, is never saved, never hashed and never seen by
    // the other ninety-two readers of `m_factionTable`. What a hunter needs
    // about the player is one bit - am I hostile to them - and `playerHostile`
    // has always been able to answer it.
    //
    // ⚑⚑⚑ WHAT PUTS A CANDIDATE ON THIS ROW IS OWNERSHIP AND NEVER A FACTION
    // NUMBER (`playerOwnedHull`). Before this a captain's hull wore the local
    // owner's colours, so a raider hunted it exactly when it was at war with a
    // government the player may never have met - and left it alone while
    // hunting the player themselves.
    const std::uint32_t playerPreyRow = static_cast<std::uint32_t>(m_factionTable.size());
    m_preyHostile.push_back(m_factionSim.playerHostile(pilot->factionIndex) ? 1u : 0u);

    m_preyCandidates.clear();
    const ecs::Pool<TraderPuppet>& puppets = playerRegistry().storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        const std::uint32_t index = puppets.entityIndices()[i];
        const Transform* body = playerRegistry().storage<Transform>().tryGet(index);
        const ShipPilot* hauler = playerRegistry().storage<ShipPilot>().tryGet(index);
        const ShipDefense* defense = playerRegistry().storage<ShipDefense>().tryGet(index);
        if (body == nullptr || hauler == nullptr || defense == nullptr || !defense->state.alive()) {
            continue;
        }
        const sim::TraderRoute route = m_economy.route(puppets.values()[i].traderIndex);
        m_preyCandidates.push_back({.index = index,
                                    .position = body->position,
                                    .faction = hauler->factionIndex,
                                    .paced = puppets.values()[i].paced != 0,
                                    .inbound = route.leg == sim::TraderLeg::Arrive});
    }

    // Miners are prey too (Phase 8x stage 6), and the note this phase came
    // from asked for exactly that: ships in the sectors "so they can be raided
    // or protected". A miner is never paced — nothing schedules it, it is
    // parked at a rock — and it counts as inbound, because that flag means
    // "will still be here when you arrive" and a ship working a field is the
    // truest case of it there is.
    const ecs::Pool<MinerPuppet>& miners = playerRegistry().storage<MinerPuppet>();
    for (std::size_t i = 0; i < miners.size(); ++i) {
        const std::uint32_t index = miners.entityIndices()[i];
        const Transform* body = playerRegistry().storage<Transform>().tryGet(index);
        const ShipPilot* crew = playerRegistry().storage<ShipPilot>().tryGet(index);
        const ShipDefense* defense = playerRegistry().storage<ShipDefense>().tryGet(index);
        if (body == nullptr || crew == nullptr || defense == nullptr || !defense->state.alive()) {
            continue;
        }
        m_preyCandidates.push_back({.index = index,
                                    .position = body->position,
                                    .faction = crew->factionIndex,
                                    .paced = false,
                                    .inbound = true});
    }

    // ⚑⚑⚑ AND THE PLAYER'S OWN CAPTAINS ARE PREY (stage D), which is the half of
    // "whose ship is that" that the player FEELS rather than reads. A freighter
    // of yours on a lane was invisible to every raider in the game: prey came
    // from the `TraderPuppet` and `MinerPuppet` pools and a captain is in
    // neither. ⚑ Paced for the same reason a coarse trader is - through the
    // middle of a leg the schedule outruns every hull in the game - and inbound
    // is read off the record for an itinerant captain and asserted for a
    // stationary one, because "will still be here when you arrive" is what a
    // ship parked at a rock or holding a beat is the truest case of.
    const ecs::Pool<CaptainPuppet>& mine = playerRegistry().storage<CaptainPuppet>();
    for (std::size_t i = 0; i < mine.size(); ++i) {
        const std::uint32_t index = mine.entityIndices()[i];
        const Transform* body = playerRegistry().storage<Transform>().tryGet(index);
        const ShipDefense* defense = playerRegistry().storage<ShipDefense>().tryGet(index);
        if (body == nullptr || defense == nullptr || !defense->state.alive()) {
            continue;
        }
        const CaptainPuppet& puppet = mine.values()[i];
        if (puppet.captainIndex >= m_captains.size()) {
            continue;
        }
        const OrderKind kind = m_captains[puppet.captainIndex].order.kind;
        m_preyCandidates.push_back(
            {.index = index,
             .position = body->position,
             .faction = playerPreyRow,
             .paced = puppet.paced != 0,
             .inbound = stationary(kind) || escorting(kind) ||
                        captainRoute(puppet.captainIndex).leg == sim::TraderLeg::Arrive});
    }

    const std::uint32_t prey = sim::choosePrey(
        transform->position, sim::preyReach(m_galaxyParams.gateDistance), m_preyCandidates, m_preyHostile);
    if (prey == sim::kNoPrey) {
        // Nothing left to take. A hunter that keeps its Travel state here
        // would fly at the last place it saw a hauler for as long as the
        // system stayed empty, so the hunt ending puts it back on the ground
        // floor and lets the rest of pilot_think decide what it does instead.
        if (pilot->state == PilotState::Travel) {
            pilot->state = PilotState::Idle;
            pilot->hasTarget = 0;
        }
        return false;
    }

    const double distance =
        length(playerRegistry().storage<Transform>().get(prey).position - transform->position);
    // ⚑ Two states, one target, and the split is not a tuning choice: the
    // dogfight steering never leaves the normal envelope (a few hundred m/s)
    // while a trade lane is hundreds of thousands of kilometres, so a raider
    // told to "attack" something across the system would close on it for
    // twenty minutes. Travel is the cruise drive and already exists for
    // exactly this distance, so an intercept is a trade leg with a ship at the
    // end of it. Weapon range is the handover, because that is precisely where
    // flying stops being useful and fighting starts.
    // ⚑ The LONGEST gun decides the handover (Phase 31 stage C1). Closing to
    // the shortest would walk a ship past the range its best gun already
    // reached; a gun that cannot reach from here simply misses, which costs
    // charge and nothing else.
    const ArmamentSummary armament = armamentSummary(playerRegistry(), entity.index);
    const double engageRange = armament.armed && armament.maxRange > 0.0f
                                   ? static_cast<double>(armament.maxRange)
                                   : kTraderArrivalRange;
    pilot->targetIndex = prey;
    pilot->hasTarget = 1;
    pilot->state = distance <= engageRange ? PilotState::Attack : PilotState::Travel;
    // ⚑ It flies to where the hauler is GOING, not to where the hauler is —
    // and this is the stage's own lesson turned on the hunter. A drive watched
    // a raider close a stern chase from 185,073 km to 9,869 km in ten seconds
    // and then lose the ship anyway, because a leg ends on its own schedule:
    // the prey went back on the record, or arrived, and its body went with it.
    // You cannot catch a hauler in the middle of its leg, so you meet it at
    // the end of one. The puppet already carries that point, so the raider
    // gets to the pad first and waits, which is what an ambush is.
    const TraderPuppet* preyPuppet = playerRegistry().storage<TraderPuppet>().tryGet(prey);
    pilot->waypoint = preyPuppet != nullptr ? preyPuppet->destination
                                            : playerRegistry().storage<Transform>().get(prey).position;

    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotFlee(ecs::Entity entity)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    // Run from what is actually shooting. Flee steers away from targetIndex,
    // and before Phase 8x nothing guaranteed that field meant anything at all
    // — a hauler that had never picked a target fled from entity 0, which is
    // the player, so the one ship coming to help was the one it ran from.
    if (pilot->threatTimer > 0.0f &&
        playerRegistry().storage<Transform>().tryGet(pilot->threatIndex) != nullptr) {
        pilot->targetIndex = pilot->threatIndex;
        pilot->hasTarget = 1;
    }
    pilot->state = PilotState::Flee;
    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotIdle(ecs::Entity entity)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Idle;
    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotPatrolTo(ecs::Entity entity, core::DVec3 waypoint)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Patrol;
    pilot->waypoint = waypoint;
    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

// The primitive decisions/019 §3 assumed was already there - see the header for
// why `pilotPatrolTo` above could not stand in for it.
bool SpaceWorld::pilotTravelTo(ecs::Entity entity, core::DVec3 waypoint)
{
    ShipPilot* pilot =
        playerRegistry().isAlive(entity) ? playerRegistry().tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Travel;
    pilot->waypoint = waypoint;
    pilot->respondTimer = kResponseGiveUpSeconds;
    if (ShipPower* power = playerRegistry().tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

namespace {

// How far a responder will cross to answer a call. Reads the LIVE rating: a
// system that is currently being fought over answers its far corners badly,
// which is the one place decisions/019 lets the live number touch enforcement.
[[nodiscard]] double responseReachFor(float live, double gateDistance)
{
    return static_cast<double>(std::abs(live)) * gateDistance * 2.0;
}

// How many hulls come. Reads the BASELINE, because this is force drawn from the
// garrison and sizing it off the live rating is the spiral the phase refuses -
// a raid would thin the answer to itself. One always stays home.
[[nodiscard]] std::uint32_t respondersFor(float baseline)
{
    const std::uint32_t garrison = patrolsFor(std::abs(baseline));
    return garrison > 1u ? garrison - 1u : 1u;
}

} // namespace

std::uint32_t SpaceWorld::respondTo(core::DVec3 position, std::uint32_t offenderIndex, ResponseCause cause)
{
    return respondTo(playerBubble(), position, offenderIndex, cause);
}

void SpaceWorld::considerResponse(std::uint32_t targetIndex, std::uint32_t attackerIndex, core::DVec3 at)
{
    considerResponse(playerBubble(), targetIndex, attackerIndex, at);
}

std::uint32_t SpaceWorld::respondTo(SystemBubble& bubble,
                                    core::DVec3 position,
                                    std::uint32_t offenderIndex,
                                    ResponseCause cause)
{
    ecs::Registry& registry = bubble.registry;
    // ⚑⚑ THE CAUSE IS READ SINCE PHASE 36 STAGE D, AND WHAT IT DECIDES IS
    // WHETHER THE LAW IS ALLOWED TO CONJURE HULLS. Weapons fire is somebody
    // dying, so a short-handed garrison tops itself up from the nearest
    // station - that is step 2 below and it exists because a raided system's
    // reach shrinks. A pilot who declined a paperwork check is not worth
    // launching a wing over: that dispatch DIVERTS ONLY. Without the split,
    // running from a stop would materialise ships out of nothing at the far
    // end of a 600,000 km lane, which is `017`'s tax arriving by a side door.
    const bool topUp = cause == ResponseCause::WeaponsFire;
    m_lastResponse = ResponseReport{};
    const std::uint32_t owner = systemOwnerFaction(bubble.system);
    if (owner >= m_factionTable.size() || m_defs == nullptr) {
        return 0;
    }
    const float live = systemSecurity(bubble.system);
    const float baseline = systemSecurityBaseline(bubble.system);
    m_lastResponse.live = live;
    m_lastResponse.responderFaction = owner;
    // ⚑ Nobody comes, and that is an ANSWER rather than a failure: it is the
    // zero band of decisions/019 doing exactly what it says on the map. ⚑⚑ And
    // it says it through `securityAnswers` rather than through its own
    // comparison, because since stage D the MAP makes this same claim to the
    // player before they fly anywhere - see the header.
    if (!securityAnswers(live)) {
        return 0;
    }

    const double reach = responseReachFor(live, m_galaxyParams.gateDistance);
    const std::uint32_t wanted = respondersFor(baseline);
    m_lastResponse.reach = reach;

    // 1. DIVERT the nearest un-engaged local hulls. Nothing is created, nothing
    // appears from nowhere, and the time it takes is the time the flight takes.
    struct Candidate
    {
        double distance = 0.0;
        std::uint32_t index = 0;
    };

    std::vector<Candidate> candidates;
    const ecs::Pool<ShipPilot>& pilots = registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        const std::uint32_t index = pilots.entityIndices()[i];
        if (pilot.factionIndex != owner || index == offenderIndex) {
            continue;
        }
        // Already fighting, or already on a call: a response that pulls a ship
        // out of the fight it was sent to is not a response.
        if (pilot.state == PilotState::Attack || pilot.respondTimer > 0.0f) {
            continue;
        }
        // Haulers are not police. A clan's raiders ARE - down the negative band
        // the resident wing is the local law, which is decisions/019 decision 2
        // meaning what it says.
        if (pilot.role == PilotRole::Trader) {
            continue;
        }
        // ⚑⚑⚑ AND NEITHER IS A SHIP OF YOURS, WHICH THE FACTION FILTER ABOVE
        // WOULD HAVE LET THROUGH (stage D). A hauling captain was already
        // excluded by the line above, for the wrong reason - it is a Trader,
        // not because it is yours - and a PATROL captain is `PilotRole::Patrol`
        // wearing the local owner's colours, so the government would have
        // conscripted a hull the player paid for into answering its calls. You
        // hired them; the Hegemony did not.
        if (playerOwnedHull(registry, index)) {
            continue;
        }
        const Transform* transform = registry.storage<Transform>().tryGet(index);
        const ShipDefense* defense = registry.storage<ShipDefense>().tryGet(index);
        if (transform == nullptr || defense == nullptr || !defense->state.alive()) {
            continue;
        }
        const double distance = length(transform->position - position);
        if (distance <= reach) {
            candidates.push_back({distance, index});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.index < b.index;
    });
    for (const Candidate& candidate : candidates) {
        if (m_lastResponse.diverted >= wanted) {
            break;
        }
        if (pilotTravelTo(registry.entityFromIndex(candidate.index), position)) {
            ++m_lastResponse.diverted;
        }
    }
    if (m_lastResponse.diverted > 0) {
        SOL_LOG_INFO("response: %u of %u diverted, live %+.3f, reach %.0f km, nearest %.0f km",
                     m_lastResponse.diverted,
                     wanted,
                     static_cast<double>(live),
                     reach / 1000.0,
                     candidates.front().distance / 1000.0);
    }
    if (m_lastResponse.diverted >= wanted || !topUp) {
        return m_lastResponse.diverted;
    }

    // 2. TOP UP FROM THE NEAREST STATION, or failing that the nearest gate.
    // Never from the offender's own position - see spawnWing.
    //
    // ⚑⚑⚑⚑ IT TOPS UP RATHER THAN ONLY FIRING WHEN NOBODY IS IN RANGE, AND A
    // TEST IS WHY. Reach reads the LIVE rating, so a raided system's reach
    // shrinks - and with it the number of local hulls close enough to divert.
    // Measured: a system that sent two answered with ONE once it was being
    // raided, which is the spiral getting back in through a side door. `wanted`
    // comes from the BASELINE, so the shortfall is made up rather than lost:
    // HOW MANY come is the garrison's size, and the live rating decides only
    // how far away they start and therefore how long they take.
    const std::uint32_t shortfall = wanted - m_lastResponse.diverted;
    const sim::SystemSpec& spec = m_galaxy.systems[bubble.system];
    core::DVec3 origin = playfieldHub(spec);
    double best = std::numeric_limits<double>::max();
    bool found = false;
    for (const sim::StationSpec& station : spec.stations) {
        const double distance = length(station.position - position);
        if (distance < best) {
            best = distance;
            origin = station.position;
            found = true;
        }
    }
    if (!found) {
        // ⚑ Out of the SPEC rather than out of `m_gates`, which is the player's
        // side data (Phase 38 stage B). A wing launched toward an incident in
        // another bubble would otherwise have started from a gate belonging to
        // the system the player happens to be standing in - a point in the
        // wrong frame, and one that reads as plausible because both systems
        // place their contents around a barycentre origin.
        for (const sim::GateSpec& gate : spec.gates) {
            const double distance = length(gate.position - position);
            if (distance < best) {
                best = distance;
                origin = gate.position;
                found = true;
            }
        }
    }
    const GameFaction& faction = m_factionTable[owner];
    const std::span<const std::string> roster =
        faction.pirate() ? factionRoster(faction, assets::RosterCell::Raider, assets::RosterCell::Count)
                         : factionRoster(faction, assets::RosterCell::Patrol, assets::RosterCell::Raider);
    const std::size_t before = registry.storage<ShipPilot>().size();
    spawnWing(bubble,
              owner,
              faction.pirate() ? assets::RosterCell::Raider : assets::RosterCell::Patrol,
              roster,
              baseline,
              shortfall,
              origin,
              700.0,
              PilotState::Travel,
              &position);
    m_lastResponse.spawned = static_cast<std::uint32_t>(registry.storage<ShipPilot>().size() - before);
    if (m_lastResponse.spawned > 0) {
        SOL_LOG_INFO("response: %u launched from %.0f km out, live %+.3f, reach %.0f km",
                     m_lastResponse.spawned,
                     best / 1000.0,
                     static_cast<double>(live),
                     reach / 1000.0);
    }
    return m_lastResponse.diverted + m_lastResponse.spawned;
}

const char* SpaceWorld::noticeReasonName(NoticeReason reason)
{
    switch (reason) {
    case NoticeReason::RandomCheck:
        return "random check";
    case NoticeReason::Dark:
        return "running dark";
    case NoticeReason::Wanted:
        return "wanted";
    case NoticeReason::None:
        break;
    }
    return "none";
}

SpaceWorld::NoticeReason SpaceWorld::considerNotice(double dt)
{
    if (m_noticeCooldown > 0.0) {
        m_noticeCooldown = std::max(0.0, m_noticeCooldown - dt);
        return NoticeReason::None;
    }
    if (isDocked() || dt <= 0.0) {
        return NoticeReason::None;
    }
    // ⚑⚑ THE LAW HAS TO BE ABLE TO SAY SOMETHING. Measured on the shipped
    // galaxy: only 26 of 85 systems are held by a faction whose legality table
    // is non-empty. The Freight Guild holds 25 systems and declares nothing
    // illegal; 29 more are clan-held and a clan has no table at all. Stopping a
    // pilot on behalf of a jurisdiction with nothing to charge them with is the
    // definition of the tax `017` warns about, so it does not happen.
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner >= m_factionTable.size()) {
        return NoticeReason::None;
    }
    // ⚑⚑⚑⚑ A HOLDER IS REQUIRED; A TABLE IS NOT, AND THAT IS PHASE 36 STAGE D
    // NARROWING A STAGE B RULING RATHER THAN REVERSING IT. Stage B refused
    // every stop a table-less jurisdiction could make, on the sound argument
    // that stopping a pilot nobody can charge with anything is exactly the tax
    // `017` names - and its test proved it by parking a DARK pilot on a patrol
    // for thirty minutes. That is the 0.03/s rate. The tax argument is about
    // the ESCALATION, not about the stop existing: a random check is 0.0008/s,
    // 37 times rarer, and it is the only way the phase's own exit criterion -
    // "be stopped by somebody with nothing to charge you with" - can ever be
    // flown. The Freight Guild holds 25 of 85 systems and declares no law.
    //
    // ⚑ Nobody at all still means nobody: `law == nullptr` is a system no
    // faction holds, and there is no one there to ask.
    const assets::FactionDef* law = jurisdictionOf(m_currentSystem);
    if (law == nullptr) {
        return NoticeReason::None;
    }
    const bool hasTable = !law->contraband.empty() || !law->restricted.empty();
    // ⚑ And the place has to be policed at all. `securityAnswers` is Phase 30's
    // silence band, read here rather than re-derived: a system whose live rating
    // has been ground to nothing by raiding is one where nobody is checking
    // papers, which is the same answer it already gives the responder dispatch.
    if (!securityAnswers(systemSecurity(m_currentSystem))) {
        return NoticeReason::None;
    }

    const std::uint32_t playerIndex = playerEntityIndex();
    const Transform* playerTransform = playerRegistry().storage<Transform>().tryGet(playerIndex);
    if (playerTransform == nullptr) {
        return NoticeReason::None;
    }

    // Which reason applies is decided ONCE for the player, not per patrol: it
    // is a fact about the ship being looked at, and a rate that changed with
    // how many hulls happened to be in range would make a busy system a
    // different game rather than a busier one.
    NoticeReason reason = NoticeReason::RandomCheck;
    double perSecond = m_noticeParams.cleanPerSecond;
    // ⚑⚑ A TRANSPONDER CHECK IS A LAW CHECK, AND A POSTED PRICE IS NOT. A
    // jurisdiction with no table has no opinion about who you say you are -
    // which is why `hasTable` gates `Dark` and deliberately does not gate
    // `Wanted`: a bounty is that faction's OWN money, and the Guild will stop
    // a pilot it has put a price on whatever its cargo law says. (A station
    // still refuses a dark ship a berth anywhere, stage A - docking is
    // consent, and patrolling is law. They are different questions.)
    if (runningDark() && hasTable) {
        reason = NoticeReason::Dark;
        perSecond = m_noticeParams.darkPerSecond;
    } else if (m_factionSim.bounty(owner) > 0.0f) {
        reason = NoticeReason::Wanted;
        perSecond = m_noticeParams.wantedPerSecond;
    }

    // The nearest patrol of the OWNING faction that is in range. Somebody
    // else's navy parked in this system does not check papers here.
    const ecs::Pool<ShipPilot>& pilots = playerRegistry().storage<ShipPilot>();
    std::uint32_t bestIndex = kNoIndex;
    double bestDistance = m_noticeParams.range;
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        if (pilot.role != PilotRole::Patrol || pilot.factionIndex != owner) {
            continue;
        }
        const std::uint32_t index = pilots.entityIndices()[i];
        const Transform* transform = playerRegistry().storage<Transform>().tryGet(index);
        if (transform == nullptr) {
            continue;
        }
        const double distance = length(transform->position - playerTransform->position);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = index;
        }
    }
    if (bestIndex == kNoIndex) {
        return NoticeReason::None;
    }

    // ⚑⚑⚑⚑ AND THIS IS WHERE A FIT GETS TO ARGUE WITH IT (Phase 36 stage E).
    // `signature` multiplies whichever rate the reason above chose, rather than
    // only the dark one: a dampener is a fact about the hull and not about why
    // somebody is looking, so it quietens a random check and a posted price by
    // exactly as much. ⚑ It is applied to the RATE and never to `range` two
    // lines down - see `kMinSignature` for the measurement that ruled the
    // envelope out, and note that shrinking it would also move the line at
    // which `tickInspection` calls you a runner, which is an OFFENCE. A
    // countermeasure that made it easier to commit one would be a trap.
    perSecond *= static_cast<double>(m_signature);

    // ⚑⚑ A RATE PER SECOND, CONVERTED HERE AND NOWHERE ELSE. The tick is 60 Hz
    // today; expressing this per call would silently retune the whole phase the
    // day that changes, and the balance is the phase.
    const double chance = 1.0 - std::exp(-perSecond * dt);
    // ⚑⚑⚑ A MEMBER RNG THAT ADVANCES, NOT A ROLL SEEDED FROM THE CLOCK. The
    // first cut seeded a fresh `Rng` from `m_worldSeconds`, which is the idiom
    // `requestDocking` uses - and it is wrong HERE, because that one rolls once
    // per player action while this rolls every frame. Anything that does not
    // advance between calls (a paused clock, a test driving this directly, two
    // frames inside the same millisecond) returns the SAME number forever, so
    // the rate is not low - it is zero or one. The probe found it by measuring
    // 0 stops in 30 minutes for a dark pilot parked on top of a patrol.
    if (static_cast<double>(m_noticeRng.nextU32()) * 0x1.0p-32 >= chance) {
        return NoticeReason::None;
    }

    m_noticeCooldown = m_noticeParams.cooldownSeconds;
    m_lastNotice = {.reason = reason,
                    .patrolIndex = bestIndex,
                    .factionIndex = owner,
                    .distance = bestDistance,
                    .atWorldSeconds = m_worldSeconds,
                    .count = m_lastNotice.count + 1};
    return reason;
}

const char* SpaceWorld::inspectionOutcomeName(InspectionOutcome outcome)
{
    switch (outcome) {
    case InspectionOutcome::Complied:
        return "complied";
    case InspectionOutcome::Ran:
        return "ran";
    case InspectionOutcome::Lapsed:
        return "lapsed";
    case InspectionOutcome::Lost:
        return "lost";
    case InspectionOutcome::None:
        break;
    }
    return "none";
}

const char* SpaceWorld::inspectionVerdictName(InspectionVerdict verdict)
{
    switch (verdict) {
    case InspectionVerdict::Clean:
        return "clean";
    case InspectionVerdict::NoLaw:
        return "no law";
    case InspectionVerdict::Duty:
        return "duty";
    case InspectionVerdict::Seizure:
        return "seizure";
    case InspectionVerdict::Fled:
        return "fled";
    case InspectionVerdict::None:
        break;
    }
    return "none";
}

std::string SpaceWorld::inspectorName() const
{
    return m_inspection.factionIndex < m_factionTable.size() ? m_factionTable[m_inspection.factionIndex].name
                                                             : std::string("Patrol");
}

bool SpaceWorld::beginInspection(std::uint32_t patrolIndex, NoticeReason reason)
{
    if (heldForInspection() || isDocked() || reason == NoticeReason::None) {
        return false;
    }
    ShipPilot* pilot = playerRegistry().storage<ShipPilot>().tryGet(patrolIndex);
    if (pilot == nullptr || pilot->role != PilotRole::Patrol) {
        return false;
    }
    // ⚑⚑ A FACTION THAT HAS DECIDED TO SHOOT YOU DOES NOT STOP TO CHECK YOUR
    // PAPERS FIRST. The same reading of `playerHostile` that `tickDocking`
    // already uses to revoke a clearance - and without it the stop cancels
    // itself one frame after it opens, because Lua's patrol branch calls
    // `pilot_engage_enemy` on every think while not attacking and a hostile
    // player is exactly what that finds. A mechanic that visibly starts and
    // then aborts reads as a bug; one that never starts reads as a rule.
    if (pilot->factionIndex < m_factionTable.size() && m_factionSim.playerHostile(pilot->factionIndex)) {
        return false;
    }
    // ⚑⚑⚑⚑ A STOP CANNOT BE OPENED FROM OUTSIDE THE ENVELOPE, AND THE LIVE
    // DRIVE IS WHAT FOUND THAT THIS NEEDED SAYING HERE. `considerNotice` has
    // always required the patrol to be within `range`, so the ordinary road
    // could not do it - but `sol.inspect_me()` picks the NEAREST patrol with
    // no range test of its own, and in a system whose nearest hull is a gate
    // picket that is 600,000 km. The hold opened and `tickInspection` ended it
    // as `Ran` on the very next frame, at 0% progress.
    //
    // ⚑⚑⚑ HARMLESS UNTIL STAGE D, AND A REAL BUG THE MOMENT RUNNING HAD A
    // PRICE: the drive was charged 400 credits of bounty and 8 standing for
    // fleeing a patrol that was never within a hundred thousand kilometres of
    // it. So the check goes at the CHOKE POINT rather than in the lever -
    // every road in, scripted or not, now refuses a stop nobody could have
    // complied with, which is the same "put the rule where it cannot be gone
    // around" argument stage A's dark refusal was written under.
    const Transform* patrolNow = playerRegistry().storage<Transform>().tryGet(patrolIndex);
    const Transform* playerNow = playerRegistry().storage<Transform>().tryGet(playerEntityIndex());
    if (patrolNow == nullptr || playerNow == nullptr ||
        length(patrolNow->position - playerNow->position) > m_noticeParams.range) {
        return false;
    }
    pilot->state = PilotState::Inspect;
    pilot->targetIndex = playerEntityIndex();
    pilot->hasTarget = 1;
    m_inspection = {.patrolIndex = patrolIndex,
                    .factionIndex = pilot->factionIndex,
                    .reason = reason,
                    .secondsLeft = m_inspectionParams.holdSeconds,
                    .scanProgress = 0.0f,
                    // Seeded here rather than left at zero, or the drive would
                    // read as locked for the one frame before `tickInspection`
                    // first runs - which at 5,500 km/s is not a rounding error.
                    .distance = length(patrolNow->position - playerNow->position)};
    ++m_lastInspection.opened;

    // ⚑ THE DEMAND, AND IT SAYS WHY. Stage B spoke one line for all three
    // reasons because there was nothing hanging off it yet; a stop that always
    // opens with the same sentence teaches the player that the reason does not
    // matter, which is the opposite of what this phase is for. All three are
    // under the comms panel's measured ~50-character budget, counted before
    // they were drawn - stage A shipped 58 and it reached the player clipped.
    const char* demand = "Routine check. Hold your heading."; // 33
    switch (reason) {
    case NoticeReason::Dark:
        demand = "Unidentified contact. Hold for scan."; // 36
        break;
    case NoticeReason::Wanted:
        demand = "There is a price on you. Hold position."; // 39
        break;
    case NoticeReason::RandomCheck:
    case NoticeReason::None:
        break;
    }
    say(inspectorName(), demand);
    SOL_LOG_INFO("inspection: opened (%s) by %s", noticeReasonName(reason), inspectorName().c_str());
    return true;
}

void SpaceWorld::endInspection(InspectionOutcome outcome, const char* reason)
{
    if (!heldForInspection()) {
        return;
    }
    // Hand the patrol back to its own business. Idle rather than Patrol for the
    // reason a finished responder goes Idle (Phase 30 stage C): `pilot_think`
    // is what knows where this pilot's next leg is, and Idle is what asks it.
    if (ShipPilot* pilot = playerRegistry().storage<ShipPilot>().tryGet(m_inspection.patrolIndex);
        pilot != nullptr && pilot->state == PilotState::Inspect) {
        pilot->state = PilotState::Idle;
        pilot->hasTarget = 0;
    }
    if (reason != nullptr) {
        say(inspectorName(), reason);
    }
    SOL_LOG_INFO("inspection: %s at %.0f%% (%s)",
                 inspectionOutcomeName(outcome),
                 static_cast<double>(m_inspection.scanProgress) * 100.0,
                 noticeReasonName(m_inspection.reason));
    m_lastInspection = {.outcome = outcome,
                        .reason = m_inspection.reason,
                        .factionIndex = m_inspection.factionIndex,
                        .atWorldSeconds = m_worldSeconds,
                        .progressAtEnd = m_inspection.scanProgress,
                        .opened = m_lastInspection.opened,
                        .complied =
                            m_lastInspection.complied + (outcome == InspectionOutcome::Complied ? 1u : 0u),
                        .ran = m_lastInspection.ran + (outcome == InspectionOutcome::Ran ? 1u : 0u),
                        // ⚑ The verdict fields are left at their defaults on
                        // purpose: this stop has not been ruled on yet, and the
                        // ruling lands next frame through the hook. What must
                        // NOT reset is the session tally beside them - a
                        // designated initialiser writes every field, so an
                        // omitted `seizures` would silently zero the count the
                        // moment a clean pilot was waved through once.
                        .seizures = m_lastInspection.seizures};
    // ⚑⚑ QUEUED BEFORE THE HOLD IS CLEARED, because the ruling is about who
    // stopped you and why, and both of those live on the record that is about
    // to be thrown away. Phase 35 stage D's rule - record the fact where the
    // thing that invalidates it happens - applied rather than rediscovered.
    queueVerdict(outcome);
    m_inspection = InspectionHold{};
    m_holdRefusalTimer = 0.0;
    // ⚑⚑ THE COOLDOWN STARTS WHEN THE STOP ENDS, NOT WHEN IT OPENED. A hold can
    // run for a minute, so charging the 90 s against the moment of notice would
    // let the next patrol open one the instant this one let go - and a second
    // checkpoint immediately after the first is the tax `017` names, arriving
    // through a clock rather than through a rate.
    m_noticeCooldown = m_noticeParams.cooldownSeconds;
}

void SpaceWorld::tickInspection(double dt)
{
    m_holdRefusalTimer = std::max(0.0, m_holdRefusalTimer - dt);
    if (!heldForInspection()) {
        return;
    }
    // ⚑ Docking out from under a hold IS running, and it is the one way out
    // that does not involve flying anywhere: you were told to hold station and
    // you went inside instead.
    if (isDocked()) {
        endInspection(InspectionOutcome::Ran, "You docked out on us. Noted.");
        return;
    }
    ShipPilot* pilot = playerRegistry().storage<ShipPilot>().tryGet(m_inspection.patrolIndex);
    const Transform* patrol = playerRegistry().storage<Transform>().tryGet(m_inspection.patrolIndex);
    if (pilot == nullptr || patrol == nullptr) {
        endInspection(InspectionOutcome::Lost, nullptr); // dead: nobody left to speak
        return;
    }
    // ⚑⚑ PULLED OFF BY ITS OWN THINK, AND IT SAYS SO. Lua's patrol branch calls
    // `pilot_engage_enemy` on every think while not attacking, so a war enemy
    // wandering into sensor range outranks a paperwork check - which is right,
    // and happened in ONE of the six manoeuvres flown against a live hold. What
    // would be wrong is it happening in silence: a HELD chip that vanishes with
    // no line is a glitch, and the same beat with a sentence on it is a patrol
    // that had something better to do.
    if (pilot->state != PilotState::Inspect) {
        endInspection(InspectionOutcome::Lost, "Break off. We have other business.");
        return;
    }
    const Transform* player = playerRegistry().storage<Transform>().tryGet(playerEntityIndex());
    if (player == nullptr) {
        endInspection(InspectionOutcome::Lost, nullptr);
        return;
    }

    // ⚑⚑ THE SAME 80 km NOTICE USES, AND DELIBERATELY NOT A NUMBER OF ITS OWN.
    // A player who has learned "that ship can see me from 80 km" has learned
    // the whole geometry of this phase; a scan that broke at some other radius
    // would be a second rule to discover by dying to it.
    const core::DVec3 toPlayer = player->position - patrol->position;
    const double distance = length(toPlayer);
    m_inspection.distance = distance;
    if (distance > m_noticeParams.range) {
        endInspection(InspectionOutcome::Ran, "You are running. That is noted.");
        return;
    }

    m_inspection.secondsLeft -= dt;
    if (m_inspection.secondsLeft <= 0.0) {
        // ⚑ The clock running out is not the same as leaving: it is the pilot
        // who kept their distance until the patrol gave up. Kept as its own
        // outcome rather than folded into `Ran` because stage D may well want
        // to price "would not come in" differently from "left".
        endInspection(InspectionOutcome::Lapsed, "Lost your signal. Move along.");
        return;
    }

    // ⚑⚑⚑ THE SCAN NEEDS THE PATROL CLOSE, AND THAT IS WHAT MAKES RUNNING A
    // REAL CHOICE. See `inspectionScanRange` for the measurement: with the cone
    // as the only way out, six flown manoeuvres all complied. Out here the
    // demand has been made and nothing else is happening yet - which is the
    // warning a pilot spotted from 40 km gets and a pilot met at a gate does not.
    if (distance > inspectionScanRange()) {
        m_inspection.scanProgress = 0.0f;
        return;
    }

    // ⚑⚑⚑ THE PLAYER'S OWN CONE RULE, POINTED THE OTHER WAY. `tickScanning`'s
    // comment is "scanning is a held aim, not a checkbox", and this is that
    // same sentence with the patrol holding the aim. It RESETS rather than
    // pauses, which is `stopScan`'s behaviour verbatim - so breaking the cone
    // costs the whole scan, and the two scans in this game read identically
    // from the cockpit whichever end of one you are on.
    if (distance > 1.0) {
        const core::Vec3 forward = rotate(patrol->orientation, core::Vec3{0.0f, 0.0f, -1.0f});
        const core::DVec3 direction = toPlayer * (1.0 / distance);
        const double aim = static_cast<double>(forward.x) * direction.x +
                           static_cast<double>(forward.y) * direction.y +
                           static_cast<double>(forward.z) * direction.z;
        if (aim < kScanConeCosine) {
            m_inspection.scanProgress = 0.0f;
            return;
        }
    }

    // ⚑⚑⚑⚑ THE HALF OF THE COUNTERMEASURE THAT IS A TACTIC RATHER THAN A
    // STAT (Phase 36 stage E). `017` rejected detection-and-consequence-only
    // because "with no stop to survive, a signature dampener is a bigger number
    // and nothing else" - so the kit has to change the stop, and this is where
    // it does. A quieter hull takes LONGER to read: 12 s at 1.0, 24 s at 0.5,
    // 34 s at the floor. What does NOT move is `holdSeconds`, and that is the
    // whole mechanic - the 60 s grant is a fixed budget, so stretching the read
    // is what turns a `Complied` into a `Lapsed`, and stage D prices a lapse at
    // nothing while it prices running at 400 credits and 8 standing.
    const double loudness = std::max(static_cast<double>(m_signature), 1.0e-3);
    const double seconds = std::max(m_inspectionParams.scanSeconds / loudness, 1.0e-3);
    m_inspection.scanProgress += static_cast<float>(dt / seconds);
    if (m_inspection.scanProgress >= 1.0f) {
        m_inspection.scanProgress = 1.0f;
        // ⚑ Stage C ends here ON PURPOSE. What was in the hold is stage D's
        // question, and `commodityLegality` is already sitting there waiting to
        // be asked it - see the phase spec's "judgement is a function call".
        endInspection(InspectionOutcome::Complied, "Scan complete. On your way.");
    }
}

float SpaceWorld::addPlayerCargo(std::uint32_t commodity, float units)
{
    if (commodity >= m_playerCargo.size()) {
        return 0.0f;
    }
    const float moved = units >= 0.0f ? std::min(units, m_playerCargoCapacity - playerCargoTotal())
                                      : std::max(units, -m_playerCargo[commodity]);
    m_playerCargo[commodity] += moved;
    return moved;
}

SpaceWorld::HoldJudgement SpaceWorld::judgeHold() const
{
    HoldJudgement found;
    const assets::FactionDef* law = jurisdictionOf(m_currentSystem);
    if (law == nullptr) {
        // Nobody holds this place at all. In the shipped galaxy that is
        // exactly one dock (`sol.lantern`, the authored lawless system), and
        // it is a different answer from "the holder has no opinion" below.
        found.worst = assets::Legality::Unpoliced;
        return found;
    }
    found.holderHasTable = !law->contraband.empty() || !law->restricted.empty();

    const std::vector<sim::EconomyCommodity>& priced = m_economy.params().commodities;
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(m_playerCargo.size()); ++c) {
        const float units = m_playerCargo[c];
        if (units <= 0.0f) {
            continue;
        }
        // ⚑ The jurisdiction that holds the system RIGHT NOW, which is Phase 33
        // stage D's ruling and not an approximation: `commodityLegality` reads
        // `systemOwnerFaction` rather than the founding claim, so a border that
        // moved while you were in the lane moves the law with it. You can clear
        // a gate legal and be read as a criminal at the station without a crate
        // in the hold having moved.
        const assets::Legality verdict = commodityLegality(m_currentSystem, c);
        const double worth =
            static_cast<double>(units) * (c < priced.size() ? static_cast<double>(priced[c].basePrice) : 0.0);
        if (verdict > found.worst) {
            found.worst = verdict;
            found.commodity = c;
            found.units = units;
            found.value = worth;
        } else if (verdict == found.worst && found.commodity != kNoIndex) {
            // Same tier, so it is the same offence: a fine on one crate of a
            // hold carrying five is a fine somebody would happily take.
            found.units += units;
            found.value += worth;
        }
    }
    return found;
}

float SpaceWorld::seizeContraband()
{
    float taken = 0.0f;
    for (std::uint32_t c = 0; c < static_cast<std::uint32_t>(m_playerCargo.size()); ++c) {
        if (m_playerCargo[c] <= 0.0f ||
            commodityLegality(m_currentSystem, c) != assets::Legality::Contraband) {
            continue;
        }
        taken += m_playerCargo[c];
        m_playerCargo[c] = 0.0f;
    }
    return taken;
}

void SpaceWorld::creditShadowStanding(float delta)
{
    if (delta <= 0.0f) {
        return;
    }
    m_factionSim.addStanding(shadowFactionIndex(), delta);
}

void SpaceWorld::recordPlayerTrade(std::uint32_t commodity, double credits)
{
    if (credits <= 0.0) {
        return;
    }
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    // ⚑⚑ THE KEY IS THE JURISDICTION'S OPINION, NOT THE GOODS CLASS, and the
    // two are deliberately different questions. `GoodsClass::Illicit` says which
    // WAREHOUSE will hold a crate and is the same answer in all 81 systems;
    // `commodityLegality` reads whoever holds this system RIGHT NOW. A crate of
    // stims is the fence's trade everywhere and a crime in 25 systems, and it
    // takes both to be the act this axis is about.
    const bool bannedHere = commodityLegality(m_currentSystem, commodity) == assets::Legality::Contraband;
    const bool theirTrade = commodityClass(commodity) == assets::GoodsClass::Illicit;
    if (bannedHere && theirTrade) {
        m_factionSim.recordTrade(shadowFactionIndex(), credits);
        if (owner < m_factionTable.size()) {
            // ⚑ The same magnitude `recordTrade` would have added, negated. It
            // is spelled out rather than routed through `recordTrade` because
            // that function floors at a positive credit count by contract, and
            // giving it a negative would be reading it backwards.
            m_factionSim.addStanding(owner,
                                     -static_cast<float>(credits) * m_factionSim.params().commerceRate);
        }
        return;
    }
    // Cases 2 and 3 in the header: somebody with an opinion was involved, or
    // nobody was. Either way no goodwill is owed.
    if (bannedHere || theirTrade) {
        return;
    }
    if (owner < m_factionTable.size()) {
        m_factionSim.recordTrade(owner, credits); // commerce goodwill
    }
}

void SpaceWorld::queueVerdict(InspectionOutcome outcome)
{
    // ⚑⚑ ONLY THE TWO OUTCOMES SOMEBODY READ A HOLD FOR, AND THAT IS THE
    // USER'S SECOND RULING EXPRESSED AS A QUEUE RATHER THAN AS A BRANCH.
    // `Lapsed` is the patrol that never closed and `Lost` is the patrol that
    // found something better to do; in neither case did anyone look at
    // anything, so there is nothing to rule on and no line to say. Writing it
    // here rather than inside the ruling means no future answer can charge for
    // a scan that never happened.
    if (outcome != InspectionOutcome::Complied && outcome != InspectionOutcome::Ran) {
        return;
    }
    m_pendingVerdict = {.factionIndex = m_inspection.factionIndex,
                        .reason = m_inspection.reason,
                        .outcome = outcome,
                        .found = judgeHold(),
                        // The hook's only entropy, drawn from the same member
                        // Rng notice rolls off - which advances. Stage B's
                        // sharpest lesson was a roll seeded from the clock at a
                        // per-frame site, and this is a per-event site, but the
                        // advancing generator is right at both.
                        .roll = static_cast<double>(m_noticeRng.nextU32()) * 0x1.0p-32};
    m_hasPendingVerdict = true;
    m_verdictFaction = m_inspection.factionIndex;
}

bool SpaceWorld::takeInspectionVerdict(PendingVerdict& out)
{
    if (!m_hasPendingVerdict) {
        return false;
    }
    out = m_pendingVerdict;
    m_hasPendingVerdict = false;
    return true;
}

void SpaceWorld::settleVerdict(InspectionVerdict verdict,
                               double credits,
                               float units,
                               float bounty,
                               float standing,
                               const std::string& message)
{
    if (!message.empty()) {
        say(m_verdictFaction < m_factionTable.size() ? m_factionTable[m_verdictFaction].name
                                                     : std::string("Patrol"),
            message);
    }
    m_lastInspection.verdict = verdict;
    m_lastInspection.creditsTaken = credits;
    m_lastInspection.unitsSeized = units;
    m_lastInspection.bountyPosted = bounty;
    m_lastInspection.standingSpent = standing;
    if (verdict == InspectionVerdict::Seizure) {
        ++m_lastInspection.seizures;
    }
    SOL_LOG_INFO("verdict: %s - %.0f cr, %.1f units, %.0f cr posted, %+.0f standing",
                 inspectionVerdictName(verdict),
                 credits,
                 static_cast<double>(units),
                 static_cast<double>(bounty),
                 static_cast<double>(standing));

    // ⚑⚑⚑ THE LAW GOES AND LOOKS, AND ONLY IF IT DECIDED THIS COST SOMETHING.
    // A jurisdiction (or a mod) that waves a runner through has decided not to
    // care, and dispatching anyway would make the pass a lie told with a
    // straight face.
    //
    // ⚑⚑ WHAT THIS CALL DOES NOT DO IS MAKE ANYBODY SHOOT, WHICH IS THE SPEC'S
    // "fired on is `respondTo` with a new cause and nothing else" corrected.
    // It sends hulls to a PLACE in `PilotState::Travel`; the shooting is
    // `pilotEngageEnemy` picking the player up on a later think, and that reads
    // `playerHostile` - standing. The dispatch decides WHO IS NEARBY when the
    // number crosses; the number is what decides that anything happens at all.
    // ⚑ The two are independent: `respondTo` never consults standing, so the
    // order they are done in here is free. That was worth checking rather than
    // asserting - an earlier version of this comment claimed the opposite.
    const bool charged = credits > 0.0 || bounty > 0.0f || standing < 0.0f;
    if (verdict == InspectionVerdict::Fled && charged) {
        if (const Transform* player = playerRegistry().storage<Transform>().tryGet(playerEntityIndex());
            player != nullptr) {
            (void)respondTo(player->position, playerEntityIndex(), ResponseCause::FledInspection);
        }
    }
    m_verdictFaction = kNoIndex;
}

void SpaceWorld::inspectionPass(const std::string& message)
{
    if (m_verdictFaction == kNoIndex) {
        return;
    }
    // ⚑ Three verdicts share one answer, and the difference between them is
    // not what happened - it is what the player is being told about the
    // galaxy. "Your hold is clean" is a jurisdiction that looked; "nothing
    // here we care about" is a jurisdiction that has no table to look at, and
    // that is the phase's best single demonstration that law is a property of
    // a place rather than of a crate.
    const InspectionVerdict verdict = m_pendingVerdict.outcome == InspectionOutcome::Ran
                                          ? InspectionVerdict::Fled
                                      : m_pendingVerdict.found.holderHasTable ? InspectionVerdict::Clean
                                                                              : InspectionVerdict::NoLaw;
    settleVerdict(verdict, 0.0, 0.0f, 0.0f, 0.0f, message);
}

void SpaceWorld::inspectionFine(double credits, const std::string& message)
{
    if (m_verdictFaction == kNoIndex) {
        return;
    }
    // ⚑⚑ CAPPED AT THE PURSE, WITH NO DEBT AND NO FALLBACK SEIZURE, WHICH IS
    // THE USER'S THIRD RULING MEANT LITERALLY: restricted goods are licensed,
    // so this is a bill and the hold stays yours. A pilot with twenty credits
    // pays twenty. That leaves a broke smuggler carrying licensed cargo for
    // very little, and it is the right place for the softness - the phase's
    // teeth are in the seizure, which is a different tier of the same table.
    const double taken = std::max(0.0, std::min(credits, m_playerCredits));
    m_playerCredits -= taken;
    const InspectionVerdict verdict = m_pendingVerdict.outcome == InspectionOutcome::Ran
                                          ? InspectionVerdict::Fled
                                          : InspectionVerdict::Duty;
    settleVerdict(verdict, taken, 0.0f, 0.0f, 0.0f, message);
}

void SpaceWorld::inspectionSeize(double bounty, const std::string& message)
{
    if (m_verdictFaction == kNoIndex) {
        return;
    }
    // ⚑ A patrol cannot lift a crate off a ship that is not there. The same
    // answer therefore means two things depending on how the stop ended, and
    // both are what a faction would do: take it and post you, or - with you
    // already gone - just post you.
    const bool present = m_pendingVerdict.outcome == InspectionOutcome::Complied;
    const float units = present ? seizeContraband() : 0.0f;
    const float posted = static_cast<float>(std::max(0.0, bounty));
    const float standing = present ? m_verdictParams.contrabandStanding : m_verdictParams.fledStanding;
    // ⚑ Spending the standing is the consequence with teeth; see
    // `settleVerdict` for why the dispatch beneath it is not, and why the
    // order of the two turned out not to matter.
    m_factionSim.addStanding(m_verdictFaction, standing);
    m_factionSim.addBounty(m_verdictFaction, posted);
    // ⚑⚑⚑⚑ AND THE OTHER END OF IT (Phase 37 stage E). The same number,
    // negated, to the people whose crate this was - so the act that made you a
    // criminal here is the act that makes you known there.
    //
    // ⚑⚑⚑ GUARDED ON WHAT WAS ABOARD RATHER THAN ON THE VERDICT, and the
    // `Ran` arm is why. `applyDefaultVerdict` answers a runner with this
    // function whatever the hold held, so keying off the call site would have
    // paid a pilot who fled an empty-handed stop - free standing with the one
    // faction whose standing is supposed to cost something, and reachable
    // without ever touching contraband. `m_pendingVerdict.found` was taken at
    // queue time, before the crates moved, so it still knows what was there
    // even on the arm where the patrol never got to look. ⚑ The fiction is
    // exact: the Ninth Shift knows what you were carrying because they sold it
    // to you.
    if (m_pendingVerdict.found.worst == assets::Legality::Contraband) {
        creditShadowStanding(-standing);
    }
    settleVerdict(present ? InspectionVerdict::Seizure : InspectionVerdict::Fled,
                  0.0,
                  units,
                  posted,
                  standing,
                  message);
}

void SpaceWorld::applyDefaultVerdict(const PendingVerdict& pending)
{
    if (m_verdictFaction == kNoIndex) {
        return;
    }
    char line[128] = {};
    // ⚑⚑ EVERY LINE HERE IS COUNTED AGAINST THE COMMS PANEL'S ~50 CHARACTERS
    // BEFORE IT IS WRITTEN, not after. Stage A shipped a 58-character refusal
    // and it reached the player as "Squawk your transponder or s" - nothing in
    // this project measures a string against a panel, so the only defence is
    // counting, and the test asserts the lengths as well as the wording.
    if (pending.outcome == InspectionOutcome::Ran) {
        inspectionSeize(static_cast<double>(m_verdictParams.fledBounty),
                        "You ran. There's a price on you now."); // 36
        return;
    }
    switch (pending.found.worst) {
    case assets::Legality::Contraband: {
        const double posted = std::max(static_cast<double>(m_verdictParams.bountyFloor),
                                       static_cast<double>(m_verdictParams.bountyPerUnit) *
                                           static_cast<double>(pending.found.units));
        inspectionSeize(posted, "Contraband. We're seizing it, and posting you."); // 46
        return;
    }
    case assets::Legality::Restricted: {
        const double duty = static_cast<double>(m_verdictParams.dutyRate) * pending.found.value;
        if (m_playerCredits + 0.5 < duty) {
            inspectionFine(duty, "Licensed cargo. We'll take what you have."); // 41
            return;
        }
        std::snprintf(line, sizeof(line), "Licensed cargo. Duty is %.0f credits.", duty); // <= 41
        inspectionFine(duty, line);
        return;
    }
    case assets::Legality::Unpoliced:
    case assets::Legality::Legal:
        break;
    }
    inspectionPass(pending.found.holderHasTable ? "Hold's clean. Safe flying."            // 26
                                                : "Nothing here we care about. Fly on."); // 35
}

void SpaceWorld::considerResponse(SystemBubble& bubble,
                                  std::uint32_t targetIndex,
                                  std::uint32_t attackerIndex,
                                  core::DVec3 at)
{
    ecs::Registry& registry = bubble.registry;
    if (bubble.responseCooldown > 0.0 || attackerIndex == kNoIndex || attackerIndex == targetIndex) {
        return;
    }
    const std::uint32_t owner = systemOwnerFaction(bubble.system);
    if (owner >= m_factionTable.size()) {
        return;
    }
    // Somebody the local law protects. There is no CRIME until Phase 36, so
    // the trigger is the thing that already exists: a hull belonging to the
    // faction that polices this system taking fire from one that does not.
    const ShipPilot* victim = registry.storage<ShipPilot>().tryGet(targetIndex);
    if (victim == nullptr || victim->factionIndex != owner) {
        return;
    }
    if (!isPlayerEntity(registry, attackerIndex)) {
        const ShipPilot* attacker = registry.storage<ShipPilot>().tryGet(attackerIndex);
        if (attacker == nullptr || attacker->factionIndex == owner) {
            return; // friendly fire is not an incident anybody is dispatched to
        }
    }
    bubble.responseCooldown = kResponseCooldownSeconds;
    (void)respondTo(bubble, at, attackerIndex, ResponseCause::WeaponsFire);
}

core::DVec3 SpaceWorld::pilotPosition(ecs::Entity entity) const
{
    if (!playerRegistry().isAlive(entity)) {
        return core::DVec3{};
    }
    const Transform* transform = playerRegistry().tryGet<Transform>(entity);
    return transform == nullptr ? core::DVec3{} : transform->position;
}

double SpaceWorld::shipHullFraction(ecs::Entity entity) const
{
    if (!playerRegistry().isAlive(entity)) {
        return 0.0;
    }
    const ShipDefense* defense = playerRegistry().tryGet<ShipDefense>(entity);
    if (defense == nullptr || defense->tuning.hull <= 0.0f) {
        return 0.0;
    }
    return static_cast<double>(defense->state.hull / defense->tuning.hull);
}

void SpaceWorld::collectDuePilotThinks(double dt, std::vector<PilotThink>& out)
{
    static constexpr const char* kStateNames[] = {"idle", "patrol", "attack", "flee", "travel", "inspect"};
    constexpr float kThinkInterval = 0.5f; // 2 Hz strategy; steering runs at 60

    // Notice (Phase 36 stage B) rides the same pass and for the same reason:
    // it needs a dt, it needs every pilot visited once, and it is throttled.
    // ⚑ Stage C is what hangs a hail off this; stage B says the line and stops.
    // ⚑⚑ NOT ASKED WHILE A STOP IS ALREADY IN FLIGHT. `considerNotice` ages
    // its own cooldown, so skipping the call also FREEZES that clock for the
    // length of the hold - which is what `endInspection` then restarts, so the
    // quiet 90 s is measured from the end of the stop rather than from its
    // start. A hold that runs for a minute would otherwise eat most of it.
    if (!heldForInspection()) {
        if (const NoticeReason noticed = considerNotice(dt); noticed != NoticeReason::None) {
            SOL_LOG_INFO("notice: %s at %.0f km", noticeReasonName(noticed), m_lastNotice.distance / 1000.0);
            // ⚑⚑⚑ STAGE B SAID A LINE AND STOPPED, AND ITS OWN COMMENT SAID SO.
            // The demand, the hold and the scan hang off this call now, and the
            // placeholder sentence stage B spoke here is gone rather than kept
            // beside them: two patrol lines in one frame is how a stop that
            // works ends up reading like a stop that stuttered.
            //
            // ⚑ The stop can still be REFUSED - a hostile owner does not check
            // papers - and when it is, nothing is said at all. Notice is a
            // decision; whether it becomes an event is this call's answer.
            (void)beginInspection(m_lastNotice.patrolIndex, noticed);
        }
    }

    // ⚑⚑⚑ THIS PASS IS PLAYER-SCOPED AND STAYS SO (Phase 38 stage B), WHICH IS
    // A STATEMENT ABOUT THE COOLING BUBBLE AND NOT AN OMISSION. Every
    // `sol.pilot_*` binding Lua answers with resolves an entity index against
    // the player's registry, so handing `pilot_think` a pilot out of another
    // bubble would be the cross-registry reach the phase's ruling exists to
    // make unaskable. The consequence is the LOD statement: the system you have
    // left keeps flying, and nobody in it changes their mind. A raider in
    // Attack goes on attacking, guns keep firing, hulls keep taking damage -
    // the fight continues - but nothing re-targets, breaks off or picks a new
    // beat until the player is back to watch it.
    //
    // ⚑ `threatTimer` and `respondTimer` used to age in the loop below and now
    // age in `tickSystem`'s pilot pass instead: they are world clocks, and a
    // clock that only runs where the player is standing would pin a cooling
    // bubble open forever. `thinkTimer` stays here, because it throttles THIS
    // dispatch and nothing else.
    ecs::Pool<ShipPilot>& pilots = playerRegistry().storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        ShipPilot& pilot = pilots.values()[i];
        pilot.thinkTimer -= static_cast<float>(dt);
        if (pilot.thinkTimer > 0.0f) {
            continue;
        }
        pilot.thinkTimer = kThinkInterval;
        const char* attitude =
            pilot.factionIndex < m_factionTable.size() ? playerAttitudeName(pilot.factionIndex) : "none";
        out.push_back({
            .entity = playerRegistry().entityFromIndex(pilots.entityIndices()[i]),
            .role = pilotRoleName(pilot.role),
            .state = kStateNames[static_cast<std::uint32_t>(pilot.state) % std::size(kStateNames)],
            .attitude = attitude,
            .pirate =
                pilot.factionIndex < m_factionTable.size() && m_factionTable[pilot.factionIndex].pirate(),
        });
    }
}

sim::ShipState SpaceWorld::shipState() const
{
    const std::uint32_t shipIndex = playerEntityIndex();
    const Transform& transform = playerRegistry().storage<Transform>().get(shipIndex);
    const FlightBody& body = playerRegistry().storage<FlightBody>().get(shipIndex);
    return {
        .position = transform.position,
        .velocity = body.velocity,
        .orientation = transform.orientation,
        .angularVelocity = body.angularVelocity,
    };
}

const char* commandModeName(CommandMode mode)
{
    switch (mode) {
    case CommandMode::None:
        return "none";
    case CommandMode::Autopilot:
        return "Autopilot";
    case CommandMode::Orbit:
        return "Orbit";
    case CommandMode::MatchSpeed:
        return "Match Speed";
    case CommandMode::KeepDistance:
        return "Keep Distance";
    case CommandMode::Hold:
        return "Hold Station";
    case CommandMode::Follow:
        return "Follow";
    }
    return "none";
}

const char* commandModeChip(CommandMode mode)
{
    switch (mode) {
    case CommandMode::None:
        return "";
    case CommandMode::Autopilot:
        return "AUTO"; // unchanged: this is the chip the game has always shown
    case CommandMode::Orbit:
        return "ORBIT";
    case CommandMode::MatchSpeed:
        return "MATCH";
    case CommandMode::KeepDistance:
        return "KEEP";
    case CommandMode::Hold:
        return "HOLD";
    case CommandMode::Follow:
        return "FOLLOW";
    }
    return "";
}

bool SpaceWorld::engageAutopilot()
{
    return engageCommand(CommandMode::Autopilot);
}

bool SpaceWorld::engageCommand(CommandMode mode)
{
    if (mode == CommandMode::None) {
        clearCommand();
        return true;
    }
    // The docked guard, asked at the gate rather than only in tick(): a ship on
    // a pad may not be given a flying order in the first place.
    if (isDocked()) {
        return false;
    }
    if (commandNeedsTarget(mode) && m_targets.empty() && playerShips().empty()) {
        return false;
    }

    const sim::ShipState state = shipState();
    // Capture the geometry the order was given at, for the two modes whose
    // meaning IS that geometry. Done before the mode is set so a failure
    // leaves nothing half-applied.
    if (mode == CommandMode::Hold) {
        m_holdPosition = state.position;
    } else if (mode == CommandMode::MatchSpeed) {
        m_matchOffset = state.position - currentTargetInfo().nav.position;
    }

    m_commandMode = mode;
    if (mode == CommandMode::Autopilot) {
        const TargetInfo target = currentTargetInfo();
        SOL_LOG_INFO("Autopilot: flying to '%s' (arrive %.1f km out)",
                     target.nav.name.c_str(),
                     autopilotArrivalRange(target) / 1000.0);
    } else if (mode == CommandMode::Hold) {
        SOL_LOG_INFO("Command: %s here", commandModeName(mode));
    } else {
        // ⚑ The current range is in the line on purpose. These are all
        // CLOSE-QUARTERS manoeuvres — none of them travels, because none of
        // them may command the cruise drive — so ordering one against a target
        // 300,000 km away is a ship that creeps toward it at thruster speed for
        // the rest of the week. That is honest behaviour and it reads as a bug,
        // so the log says the distance and the player can see why nothing is
        // happening. Composing "fly there, THEN orbit" is a real question and
        // it belongs to the phase's checkpoint, not to a silent guess here.
        const TargetInfo target = currentTargetInfo();
        SOL_LOG_INFO("Command: %s '%s' (target %.1f km away)",
                     commandModeName(mode),
                     target.nav.name.c_str(),
                     length(target.nav.position - state.position) / 1000.0);
    }
    return true;
}

void SpaceWorld::setOrbitRange(double meters)
{
    m_orbitRange = core::clamp(meters, 200.0, 1.0e6);
}

void SpaceWorld::setKeepDistanceRange(double meters)
{
    m_keepDistanceRange = core::clamp(meters, 100.0, 1.0e6);
}

double SpaceWorld::autopilotArrivalRange(const TargetInfo& target) const
{
    // Stand off by the surface plus the arrival range; big bodies get at
    // least half a radius of clearance so the goal sits outside their
    // avoidance shell.
    double range = target.nav.surfaceRadius + std::max(m_autopilotRange, target.nav.surfaceRadius * 0.5);
    // A mission objective is the one target whose whole point is *arriving*
    // (Phase 8i). The general standoff is 1.5 km and a FlyTo radius is
    // typically 1.2 km, so autopilot would otherwise park just outside the
    // completion sphere and the objective would never tick over — which is the
    // original complaint wearing a different hat. Park well inside it instead.
    // An escort's marker carries no radius: there is no sphere to get inside,
    // only a ship to keep station on, so the general standoff is the right one
    // and the clamp is skipped rather than applied against a zero.
    ObjectiveMarker marker;
    if (m_targetIndex == objectiveTargetIndex() && objectiveMarker(&marker) && marker.radius > 0.0) {
        range = std::max(std::min(range, marker.radius * 0.5), 50.0);
    }
    // A cleared berth is the second target of that kind (Phase 8r) and the
    // same rule applies: the standoff has to put the ship INSIDE the capture
    // sphere or autopilot parks just outside the thing it was flying to. Half
    // the capture radius is 30 m, still 170 m from the station centre and so
    // comfortably outside the 130 m sphere the autopilot is steering around —
    // which is why the berth ring sits at 200 m in the first place.
    if (hasClearance() && m_targetIndex == berthTargetIndex()) {
        range = std::min(range, sim::kBerthCaptureRadius * 0.5);
    }
    // And a gate is the third (Phase 8v) — but NOT for the same reason, and
    // copying the berth's half-the-radius here was wrong in a way only flying
    // it revealed. A berth is a place to park; a gate is a doorway to cross.
    // steerTravel decelerates to a STOP at whatever range it is given, so any
    // positive standoff has the ship braking as it reaches the frame: at half
    // the radius it crept to 78 m and sat there at 0.1 m/s, eight metres short
    // of a jump, with autopilot still dutifully engaged.
    //
    // Zero is right, but only alongside autopilotDestination() below (Phase
    // 8w): the gate's CENTRE lies on the plane the ship has to cross, and
    // arriving at a plane is not crossing it, so aiming at the centre would
    // stall one epsilon short of a jump forever — 8v's 78 m failure in a new
    // costume. Autopilot aims at a point on the far side instead, and this
    // zero is what stops it standing off from THAT.
    if (navTargetKind(m_targetIndex) == NavKind::Gate) {
        range = 0.0;
    }
    return range;
}

void SpaceWorld::setAutopilotArrivalRange(double meters)
{
    m_autopilotRange = core::clamp(meters, 100.0, 1.0e7);
}

core::DVec3 SpaceWorld::autopilotDestination(const TargetInfo& target, const core::DVec3& from) const
{
    // Everything but a gate is a place to arrive AT (Phase 8w).
    if (navTargetKind(m_targetIndex) != NavKind::Gate) {
        return target.nav.position;
    }
    const GateInstance* gate = nullptr;
    for (const GateInstance& candidate : m_gates) {
        if (length(candidate.position - target.nav.position) < 1.0) {
            gate = &candidate;
            break;
        }
    }
    if (gate == nullptr) {
        return target.nav.position;
    }
    // A gate is a place to arrive THROUGH. Its centre sits on the plane the
    // ship has to cross, and steerTravel decelerates to a stop at its
    // destination — so aiming at the centre parks the ship ON the threshold
    // and the aperture test, which needs the segment to change sides, never
    // fires. Aim at a point beyond the opening, on the far side from wherever
    // the ship currently is, and flying there means going through.
    const double side = dot(from - gate->position, gate->axis);
    const double sign = side >= 0.0 ? -1.0 : 1.0;
    return gate->position + gate->axis * (sign * kGateApproachOvershoot);
}

sim::FlightInput SpaceWorld::commandInput()
{
    // GUARD 1 — manual deflection. Any real steering/thrust means the player
    // has reached for the controls (the mapper's assist/cruise toggles alone
    // don't, deliberately); the threshold ignores mouse-stick noise.
    //
    // ⚑⚑ This is the guard the phase had to DECIDE rather than inherit, and it
    // is where autopilot and a standing order part company. Autopilot is going
    // somewhere, so your input replaces its plan and it is cancelled. An orbit
    // is a frame you are flying inside, so your input is layered ON it: you fly
    // manually while the stick is deflected and the order picks up again the
    // moment you let go. Nothing is logged for the standing case because it is
    // not an event — it is the player flying.
    const auto deflected = [](const core::Vec3& v) {
        return std::fabs(v.x) > 0.25f || std::fabs(v.y) > 0.25f || std::fabs(v.z) > 0.25f;
    };
    if (deflected(m_shipInput.linear) || deflected(m_shipInput.angular) || m_shipInput.boost) {
        if (isStandingCommand(m_commandMode)) {
            return m_shipInput; // overridden while held; the mode survives
        }
        m_commandMode = CommandMode::None;
        SOL_LOG_INFO("Autopilot: cancelled by manual input");
        return m_shipInput;
    }

    // GUARD 3 — the target went away (2 is the docked guard, in tick()). Hold
    // is exempt: it is the one command that is about a place, not a thing, so
    // losing the target list must not end it.
    const TargetInfo target = currentTargetInfo();
    if (commandNeedsTarget(m_commandMode) && target.nav.name.empty()) {
        if (isStandingCommand(m_commandMode)) {
            SOL_LOG_INFO("Command: %s ended, target lost", commandModeName(m_commandMode));
        }
        m_commandMode = CommandMode::None;
        return m_shipInput;
    }

    if (isStandingCommand(m_commandMode)) {
        return standingCommandInput(target);
    }

    const double effectiveRange = autopilotArrivalRange(target);
    const core::DVec3 targetVelocity = target.isShip ? target.velocity : core::DVec3{};
    const sim::ShipState state = shipState();
    const core::DVec3 destination = autopilotDestination(target, state.position);
    const double remaining = length(destination - state.position) - effectiveRange;
    if (remaining <= 0.0 && length(state.velocity - targetVelocity) < 25.0) {
        m_commandMode = CommandMode::None;
        SOL_LOG_INFO("Autopilot: arrived at '%s'", target.nav.name.c_str());
        return m_shipInput;
    }

    // The destination's own sphere must not deflect the final approach — and
    // since Phase 8y it must not BRAKE it either, which is a stricter test:
    // the path query stops the ship at a sphere's edge, so a berth 200 m off a
    // station whose sphere reaches 230 m would be a place autopilot could
    // never arrive. The margin therefore carries the same clearance the query
    // itself adds.
    m_autopilotObstacles.clear();
    for (const sim::AvoidanceSphere& sphere : m_avoidance) {
        if (length(sphere.position - destination) > sphere.radius + sim::kPathClearance + effectiveRange) {
            m_autopilotObstacles.push_back(sphere);
        }
    }

    sim::FlightInput input = sim::steerTravel(state,
                                              shipTuning(),
                                              destination,
                                              targetVelocity,
                                              effectiveRange,
                                              m_autopilotObstacles,
                                              playerEntityIndex());
    input.assist = true;
    return input;
}

namespace {

// Where a follower sits relative to what it is following.
//
// ⚑ This is what makes Follow a different order from MatchSpeed rather than a
// synonym for it. MatchSpeed keeps whatever geometry you had when you gave the
// order; Follow takes up a station, so it needs a canonical one — and the only
// frame available from a TargetInfo is the target's own motion, since a nav
// target carries a position and a velocity but no orientation.
//
// Behind and off to one side: dead astern is the single place a follower cannot
// see past the ship it is following, and it is where the exhaust is. A target
// that is not moving has no "behind", so the offset falls back to holding the
// bearing the follower already has — which is the same answer MatchSpeed would
// give, and is right, because a stationary thing has no shoulder to sit off.
[[nodiscard]] core::DVec3 followOffset(const core::DVec3& shipPosition,
                                       const core::DVec3& targetPosition,
                                       const core::DVec3& targetVelocity,
                                       double range)
{
    const double speed = length(targetVelocity);
    if (speed < 1.0) {
        const core::DVec3 bearing = shipPosition - targetPosition;
        const double distance = length(bearing);
        return distance > 1.0e-6 ? bearing * (range / distance) : core::DVec3{0.0, 0.0, range};
    }
    const core::DVec3 forward = targetVelocity * (1.0 / speed);
    // Any axis not parallel to the travel direction spans a plane with it; the
    // 0.9 test keeps the cross product from collapsing when they coincide.
    const core::DVec3 axis =
        std::fabs(forward.y) < 0.9 ? core::DVec3{0.0, 1.0, 0.0} : core::DVec3{1.0, 0.0, 0.0};
    const core::DVec3 side = normalize(cross(forward, axis));
    return forward * (-range) + side * (range * 0.5);
}

} // namespace

sim::FlightInput SpaceWorld::standingCommandInput(const TargetInfo& target)
{
    const sim::ShipState state = shipState();
    const sim::ShipTuning& tuning = shipTuning();
    // A station has no velocity of its own worth matching; a ship does. Same
    // test autopilotInput has always used to decide whether to close on a
    // moving mark or a fixed one.
    const core::DVec3 targetVelocity = target.isShip ? target.velocity : core::DVec3{};

    sim::FlightInput input;
    switch (m_commandMode) {
    case CommandMode::Orbit:
        input = sim::steerOrbit(state, tuning, target.nav.position, targetVelocity, m_orbitRange);
        break;
    case CommandMode::MatchSpeed:
        // Keep the geometry the order was given at. steerFormation is exactly
        // "hold a world offset from a moving anchor, velocity-matched", which
        // is what match speed means once you say it precisely.
        input = sim::steerFormation(state, tuning, target.nav.position, targetVelocity, m_matchOffset);
        break;
    case CommandMode::KeepDistance:
        input = sim::steerPursue(state, tuning, target.nav.position, targetVelocity, m_keepDistanceRange);
        break;
    case CommandMode::Follow:
        // Off the shoulder rather than dead astern: a follower parked exactly
        // behind is the one place it cannot see past the ship it is following,
        // and it is also where that ship's exhaust is. The offset is built in
        // the target's own frame so it stays "off the left shoulder" however
        // the target is pointing.
        input = sim::steerFormation(
            state,
            tuning,
            target.nav.position,
            targetVelocity,
            followOffset(state.position, target.nav.position, targetVelocity, m_keepDistanceRange));
        break;
    case CommandMode::Hold:
        // Stand on the spot the order was given. A zero anchor velocity is the
        // whole difference between this and MatchSpeed.
        input = sim::steerFormation(state, tuning, m_holdPosition, core::DVec3{}, core::DVec3{});
        break;
    case CommandMode::None:
    case CommandMode::Autopilot:
        return m_shipInput; // unreachable: commandInput routes these away
    }
    // Standing orders fly inside the assist envelope, exactly as autopilot
    // does. Cruise is deliberately never commanded here: every one of these
    // modes is a close-quarters manoeuvre, and a cruise burn crosses the thing
    // being orbited in a single tick.
    input.assist = true;
    return input;
}

// The player's flight input for this tick: the docked pin, the commanded or
// manual input, the two cruise refusals, and the write into their own
// `ShipControl`. Split out of `tick` at Phase 38 stage B so that the one thing
// in the flight half that is about a PERSON rather than about a system can be
// called from inside the per-system loop exactly once - see `tickSystem`.
void SpaceWorld::tickPlayerFlightInput(double dt)
{
    const std::uint32_t playerIndex = playerEntityIndex();
    if (isDocked()) {
        // Parked: flight input is ignored and the ship stays pinned to the
        // pad (collision impulses must not drift a docked ship).
        //
        // GUARD 2 of commandInput's four. Docking ends every command, standing
        // ones included: a ship on a pad is not orbiting anything.
        clearCommand();
        m_appliedInput = sim::FlightInput{};
        playerRegistry().storage<ShipControl>().get(playerIndex).input = sim::FlightInput{};
        Transform& transform = playerRegistry().storage<Transform>().get(playerIndex);
        const core::DVec3 pad = dockPoint(m_dockedStation);
        transform.position = pad;
        transform.previousPosition = pad;
        playerRegistry().storage<FlightBody>().get(playerIndex) = FlightBody{};
    } else {
        m_appliedInput = m_commandMode != CommandMode::None ? commandInput() : m_shipInput;
        // ⚑⚑⚑⚑ THE HOLD HAS TEETH, AND THAT IS THE USER'S RULING FOR STAGE C
        // (2026-09-01). Cruise is 25,000x the assist cap - 220 m/s becomes
        // 5,500 km/s - so a ship on cruise crosses the 80 km notice envelope in
        // 0.029 s, and a hold anybody can light the drive out of is not a hold
        // at all, it is a message.
        //
        // ⚑⚑⚑ IT REACHES AS FAR AS THE SCAN DOES AND NO FURTHER, WHICH IS THE
        // MEASUREMENT TALKING. Locked on the HOLD rather than on the range, the
        // stop was unescapable: six flown manoeuvres, six compliances, because
        // an interceptor tracks a shuttle inside a 20 degree cone indefinitely.
        // See `inspectionScanRange`. Running is still `017`'s "interruptible by
        // flying away" - it is just that what you flee is the CLOSING, and how
        // much warning you get is how far off they spotted you.
        //
        // ⚑ Written as a refusal rather than as a dead key, which is stage A's
        // lesson: `guardManualCruise` below already owns the same lever
        // (`m_appliedInput.cruise = false`) for the proximity cut, and it says
        // so out loud when it uses it.
        if (driveLockedByInspection() && m_appliedInput.cruise) {
            m_appliedInput.cruise = false;
            if (m_holdRefusalTimer <= 0.0) {
                say(inspectorName(), "Cut that drive. You are not clear to run.");
                m_holdRefusalTimer = kCommsMessageSeconds;
            }
        }
        // The manual-cruise guard is about a cruise burn the PLAYER lit, so it
        // asks whether the ship is flying itself, not whether it is on
        // autopilot specifically.
        if (m_commandMode == CommandMode::None && m_appliedInput.cruise) {
            guardManualCruise(dt);
        } else {
            m_cruiseWarningTimer = 0.0;
        }
        playerRegistry().storage<ShipControl>().get(playerIndex).input = m_appliedInput;
    }
}

// ⚑⚑⚑⚑ ONE SYSTEM'S TICK (Phase 38 stage B). Eight of the profiler's zones
// live in here - avoidance, pilots, flight, the two collision halves,
// projectiles, weapons and the fine half of mining - and every one of them was
// written against `m_registry` and then against `playerRegistry()`. They take a
// bubble now, which is what `decisions/015`'s "the tick becomes a loop over
// instantiated systems" actually costs once the frame is structural: not a
// filter per query, but one parameter that the compiler checks 300 times.
//
// ⚑⚑⚑ WHAT STAYED OUTSIDE, AND IT IS NOT AN OVERSIGHT. The coarse sims
// (economy, factions, missions), scanning, docking, the gate crossing and the
// player's own feedback are all either galaxy-wide or about the one entity that
// is only ever in one bubble. A per-system loop around any of them would run
// them k times and be wrong k-1 of those times.
//
// ⚑⚑ THE SCRATCH BUFFERS ARE STILL SINGLE. `m_avoidance`, `m_collisionBodies`,
// `m_collisionShipIndices` and `m_contacts` are cleared and refilled inside
// this function, so nesting the tick costs no per-system copy of any of them -
// and `sim::resolveCollisions` is O(n^2) with no broadphase, so k systems cost
// k*n^2 here where one global list with a frame filter would cost (kn)^2.
void SpaceWorld::tickSystem(SystemBubble& bubble, double dt)
{
    ecs::Registry& registry = bubble.registry;
    // The player is in exactly one bubble and it is the front one - that is
    // what `playerRegistry()` means. Asked once here rather than per site,
    // because the interesting sites below are inside pool walks where the
    // answer is a property of the WALK and not of the entity.
    const bool playersBubble = &bubble == m_bubbles.front().get();
    // What nothing may fly into, this tick (Phase 8y). Before any steering,
    // the player's autopilot included.
    {
        const std::uint32_t zone = core::frameProfiler().beginZone("sim.avoidance");
        rebuildAvoidance(bubble);
        core::frameProfiler().addCounter(zone, m_avoidance.size());
        core::frameProfiler().endZone(zone);
    }
    // ⚑⚑ THE PLAYER'S OWN INPUT SITS BETWEEN THEIR SYSTEM'S AVOIDANCE AND
    // THEIR SYSTEM'S STEERING, AND THAT IS WHY IT IS CALLED FROM IN HERE. It
    // is player-scoped work - one entity, one autopilot, one cruise guard - so
    // it must run once rather than once per bubble; but 8y's rule is that
    // nothing steers before `m_avoidance` describes the sky it is steering
    // through, and `m_avoidance` describes exactly one system at a time now.
    if (playersBubble) {
        tickPlayerFlightInput(dt);
    }

    // NPC pilots: C++ steering flies whatever state Lua's pilot_think chose.
    {
        SOL_PROFILE_ZONE_NAMED(pilotZone, "sim.pilots");
        // The law's dispatch throttle ages with the pilots it throttles (Phase
        // 30 stage C), and per system since stage B - see the field comment.
        bubble.responseCooldown = std::max(0.0, bubble.responseCooldown - dt);
        ecs::Pool<ShipPilot>& pilots = registry.storage<ShipPilot>();
        SOL_PROFILE_COUNT(pilotZone, pilots.size());
        // ⚑ Two spans over one list (Phase 8y §C). A ship going somewhere
        // dodges everything, other ships included; a ship in a FIGHT sees only
        // the scenery, because ramming is a legitimate move and separation
        // logic would quietly forbid it — and because a hunter that treated
        // its own target as an obstacle could never close on it.
        const std::span<const sim::AvoidanceSphere> obstacles = m_avoidance;
        const std::span<const sim::AvoidanceSphere> scenery =
            obstacles.first(std::min(m_avoidStatics, obstacles.size()));
        for (std::size_t i = 0; i < pilots.size(); ++i) {
            ShipPilot& pilot = pilots.values()[i];
            const std::uint32_t entityIndex = pilots.entityIndices()[i];
            // ⚑⚑⚑⚑ THE TWO SIMULATION CLOCKS AGE HERE NOW, AND THAT MOVE IS
            // STAGE C'S WHOLE RETENTION POLICY WORKING (Phase 38 stage B).
            // Both aged in `collectDuePilotThinks` until this stage - the pass
            // that hands Lua its pilots - and that pass is PLAYER-SCOPED and
            // stays so, because `pilot_think` and every `sol.pilot_*` binding
            // resolve an entity index against the player's registry. A clock
            // that only ages where the player is standing is a clock that never
            // runs down anywhere else: `threatTimer` is exactly what stage C
            // reads to decide a fight is still live, so a bubble left behind
            // would have been pinned open forever by a threat nobody could
            // forget. This is the one thing in the pilot pass that is about the
            // WORLD rather than about a decision.
            if (pilot.threatTimer > 0.0f) {
                pilot.threatTimer = std::max(0.0f, pilot.threatTimer - static_cast<float>(dt));
            }
            // A dispatched responder gives up on the same terms and for the
            // same reason (Phase 30 stage C): a call you have been flying at
            // for three minutes without finding anything is a call that is
            // over. Going Idle hands it back to `pilot_think` - which in a
            // bubble the player is not in means it simply stands down, and
            // standing down is the right answer there too.
            if (pilot.respondTimer > 0.0f) {
                pilot.respondTimer = std::max(0.0f, pilot.respondTimer - static_cast<float>(dt));
                if (pilot.respondTimer == 0.0f && pilot.state == PilotState::Travel) {
                    pilot.state = PilotState::Idle;
                }
            }
            ShipControl* control = registry.storage<ShipControl>().tryGet(entityIndex);
            const Transform* transform = registry.storage<Transform>().tryGet(entityIndex);
            const FlightBody* body = registry.storage<FlightBody>().tryGet(entityIndex);
            if (control == nullptr || transform == nullptr || body == nullptr) {
                continue;
            }
            const sim::ShipState self = {
                .position = transform->position,
                .velocity = body->velocity,
                .orientation = transform->orientation,
                .angularVelocity = body->angularVelocity,
            };

            // A patrol only holds a grudge while the player is actually
            // hostile (Phase 8b live find: permanent aggro besieged the pad
            // after a standing recovered; raiders, by contrast, keep theirs).
            if (pilot.state == PilotState::Attack && pilot.role == PilotRole::Patrol &&
                pilot.hasTarget != 0 && isPlayerEntity(registry, pilot.targetIndex) &&
                pilot.factionIndex < m_factionTable.size() &&
                !m_factionSim.playerHostile(pilot.factionIndex)) {
                pilot.state = PilotState::Idle;
                pilot.hasTarget = 0;
            }

            sim::FlightInput input; // Idle default: assist-on station keeping
            switch (pilot.state) {
            case PilotState::Idle:
                break;
            case PilotState::Patrol:
                if (length(pilot.waypoint - self.position) < 120.0) {
                    pilot.state = PilotState::Idle; // arrived; Lua picks the next leg
                    break;
                }
                input = sim::steerPursue(self, control->tuning, pilot.waypoint, {}, 50.0);
                break;
            case PilotState::Travel:
                // A trade leg is hundreds of thousands of kilometres, so this
                // is the cruise drive and the same steering the player's
                // autopilot flies. Arriving does NOT end the leg — the coarse
                // record does that — so a puppet that beats its own clock
                // simply holds station off the pad it came to.
                //
                // ⚑ A RESPONDER IS THE EXCEPTION, AND `respondTimer` IS WHAT
                // TELLS THEM APART (Phase 30 stage C). It has no coarse record
                // to end its leg, so arriving at an incident that has since
                // dispersed would leave it holding station at an empty point in
                // space forever. Going Idle hands it back to `pilot_think`,
                // which puts a patrol on its next leg. It does NOT engage here:
                // the Lua patrol branch already calls `pilot_engage_enemy` on
                // every think while not attacking, so a responder that arrives
                // with a hostile inside sensor range picks it up through the
                // path that already exists.
                if (pilot.respondTimer > 0.0f &&
                    length(pilot.waypoint - self.position) < kTraderArrivalRange) {
                    pilot.respondTimer = 0.0f;
                    pilot.state = PilotState::Idle;
                    break;
                }
                input = sim::steerTravel(
                    self, control->tuning, pilot.waypoint, {}, kTraderArrivalRange, obstacles, entityIndex);
                break;
            case PilotState::Inspect: {
                // ⚑⚑ THE ATTACK CASE WITH THE GUNS TAKEN OUT (Phase 36 stage
                // C). `steerAimAndMove` is what holds a nose on something while
                // flying somewhere else, and holding the nose on you is the
                // whole mechanic: `tickInspection`'s cone rule is measured off
                // this pilot's orientation, so the scan only advances while the
                // steering below is succeeding. `input.trigger` is never
                // written, which is the difference between an inspection and an
                // execution.
                const Transform* held = registry.storage<Transform>().tryGet(pilot.targetIndex);
                const FlightBody* heldBody = registry.storage<FlightBody>().tryGet(pilot.targetIndex);
                if (pilot.hasTarget == 0 || held == nullptr || heldBody == nullptr) {
                    pilot.state = PilotState::Idle; // tickInspection ends the hold next frame
                    break;
                }
                const core::DVec3 toHeld = held->position - self.position;
                const double heldDistance = length(toHeld);
                const core::DVec3 heldDirection =
                    heldDistance > 1.0 ? toHeld * (1.0 / heldDistance) : core::DVec3{0.0, 0.0, -1.0};
                core::DVec3 closing =
                    heldBody->velocity + heldDirection * ((heldDistance - m_inspectionParams.standoff) * 0.5);
                sim::avoidObstacles(closing, self, scenery, 8.0);
                input = sim::steerAimAndMove(self, control->tuning, held->position, closing);
                break;
            }
            case PilotState::Attack: {
                const Transform* targetTransform = registry.storage<Transform>().tryGet(pilot.targetIndex);
                const FlightBody* targetBody = registry.storage<FlightBody>().tryGet(pilot.targetIndex);
                if (pilot.hasTarget == 0 || targetTransform == nullptr || targetBody == nullptr) {
                    pilot.state = PilotState::Idle;
                    break;
                }
                const core::DVec3 toTarget = targetTransform->position - self.position;
                const double distance = length(toTarget);
                const core::DVec3 direction =
                    distance > 1.0 ? toTarget * (1.0 / distance) : core::DVec3{0.0, 0.0, -1.0};
                core::DVec3 desiredVelocity = targetBody->velocity + direction * ((distance - 250.0) * 0.5);
                sim::avoidObstacles(desiredVelocity, self, scenery, 8.0);

                // The FIRST projectile gun in mount order supplies the lead, and
                // an all-hitscan fit supplies none - which reads as instant, the
                // meaning the single weapon gave a zero speed before it.
                const ArmamentSummary armament = armamentSummary(registry, entityIndex);
                const double projectileSpeed = armament.leadSpeed > 1.0f
                                                   ? static_cast<double>(armament.leadSpeed)
                                                   : 1.0e9; // hitscan: effectively instant
                core::DVec3 aimDirection;
                (void)sim::computeInterceptDirection(self.position,
                                                     self.velocity,
                                                     targetTransform->position,
                                                     targetBody->velocity,
                                                     projectileSpeed,
                                                     aimDirection);
                const core::DVec3 aimPoint =
                    self.position + aimDirection * (distance > 100.0 ? distance : 100.0);
                input = sim::steerAimAndMove(self, control->tuning, aimPoint, desiredVelocity);
                if (armament.armed) {
                    // One trigger, so the LONGEST gun decides when it is worth
                    // pulling: a shorter gun firing early wastes charge, which
                    // is a cost the capacitor already charges for.
                    input.trigger = sim::aimError(self, aimPoint) < 0.06 &&
                                    distance < static_cast<double>(armament.maxRange) * 0.9;
                    // ⚑ A hauler with a fighter inside weapon range is under
                    // threat whether or not a shot has connected yet, and this
                    // is the tick-rate place that knows it (Phase 8x §D).
                    // Arming it only on damage was not enough twice over: the
                    // record kept moving a hauler that was being shot at, and
                    // a six-second lull in the hits released the prey, expired
                    // the lock and sent the raider off after a better-ranked
                    // target 656,890 km away — abandoning a fight it was
                    // winning at one kilometre. Threat is proximity plus
                    // intent, not a hit counter.
                    // A miner counts as well (stage 6). Nothing paces it, so
                    // there is no clock to hold; what the threat buys there is
                    // the ship knowing which way to run, and the reconcile
                    // leaving it alone instead of sending it back to its rock
                    // the moment Lua stops flying the fight.
                    if (pilot.role == PilotRole::Fighter &&
                        distance < static_cast<double>(armament.maxRange) &&
                        (registry.storage<TraderPuppet>().tryGet(pilot.targetIndex) != nullptr ||
                         registry.storage<MinerPuppet>().tryGet(pilot.targetIndex) != nullptr)) {
                        if (ShipPilot* hunted = registry.storage<ShipPilot>().tryGet(pilot.targetIndex)) {
                            hunted->threatIndex = entityIndex;
                            hunted->threatTimer = static_cast<float>(kThreatMemorySeconds);
                        }
                    }
                }
                break;
            }
            case PilotState::Flee: {
                const Transform* threatTransform = registry.storage<Transform>().tryGet(pilot.targetIndex);
                const core::DVec3 threat = threatTransform != nullptr
                                               ? threatTransform->position
                                               : self.position + core::DVec3{0.0, 0.0, 1.0};
                pilot.weavePhase += static_cast<float>(dt) * 3.0f;
                input = sim::steerEvade(self, control->tuning, threat, pilot.weavePhase);
                break;
            }
            }
            control->input = input;
        }
    }

    // Step every flying ship with its own tuning and commanded input (NPC
    // input is written by pilots — zero/station-keeping until Phase 6 AI).
    // ENG pips scale the flight envelope; WEP pips recharge the capacitor;
    // SYS pips scale shield regen.
    ecs::Pool<ShipPower>& powers = registry.storage<ShipPower>();
    ecs::Pool<ShipDefense>& defenses = registry.storage<ShipDefense>();
    {
        SOL_PROFILE_ZONE("sim.flight");
        registry.view<Transform, FlightBody, ShipControl>().each(
            [&](ecs::Entity entity, Transform& transform, FlightBody& body, ShipControl& control) {
                transform.previousPosition = transform.position;
                transform.previousOrientation = transform.orientation;

                sim::ShipTuning tuning = control.tuning;
                if (ShipPower* power = powers.tryGet(entity.index)) {
                    sim::stepPower(power->state, power->tuning, dt);
                    tuning = sim::applyEnginePips(control.tuning, power->state.pips, power->tuning);
                }
                // ⚑⚑⚑ AND THEN THE DRIVE ITSELF (Phase 31 stage F2). This sits
                // exactly where the ENG pips already scale the envelope,
                // because it is the same kind of fact - how much push the ship
                // has this tick - and multiplying after the pips is what makes
                // a half-drive ship on full ENG still a half-drive ship.
                //
                // ⚑ THE SPEED CAP SCALES WITH THE ACCELERATION, and that is
                // deliberate rather than a double penalty: there is no drag out
                // here, so `maxSpeed` is not a balance of thrust against
                // resistance, it is the flight model's stand-in for what the
                // drive can hold. A drive at half holds half. It is also what
                // stops a freighter whose drive you shot off from CRUISING
                // away, because cruise is a multiple of this same cap - and a
                // ship that can still run is not one you have disabled.
                //
                // ⚑⚑ ANGULAR ACCELERATION AND TURN RATE ARE UNTOUCHED, ON
                // PURPOSE. Engines push and thrusters turn - `MountKind` has
                // both and gdd.md §11.5 separates them - so a hull with its
                // drive shot off is dead in the water and still able to point
                // itself, which is what lets a crippled freighter keep a turret
                // on you. Shooting the turning out of a ship is what a
                // `thruster` mount is for, and nothing in the game declares one
                // yet.
                const ShipMounts* condition = registry.tryGet<ShipMounts>(entity);
                if (condition != nullptr) {
                    const float drive = driveFraction(*condition);
                    if (drive < 1.0f) {
                        tuning.forwardAccel *= drive;
                        tuning.reverseAccel *= drive;
                        tuning.lateralAccel *= drive;
                        tuning.verticalAccel *= drive;
                        tuning.maxSpeed *= drive;
                    }
                }
                if (ShipDefense* defense = defenses.tryGet(entity.index)) {
                    const ShipPower* power = powers.tryGet(entity.index);
                    float regenScale =
                        power != nullptr ? sim::shieldRegenScale(power->state.pips, power->tuning) : 1.0f;
                    // ⚑⚑ A SHIELD GENERATOR THAT HAS BEEN SHOT OFF STOPS
                    // REGENERATING THE FACINGS, AND DOES NOT COLLAPSE THEM
                    // (Phase 31 stage F2). What is already in the envelope does
                    // not evaporate because the machine that put it there is
                    // gone; what stops is any more of it arriving. Collapsing
                    // both facings instead would make one lucky shot a
                    // 320-point swing on the shipped freighter - larger than
                    // anything else in the damage model can do in one hit, and
                    // a bigger effect than destroying the hull's own armour.
                    if (condition != nullptr && !shieldsArePowered(*condition)) {
                        regenScale = 0.0f;
                    }
                    sim::stepDefense(defense->state, defense->tuning, regenScale, dt);
                    if (defense->playerAssist > 0.0) {
                        defense->playerAssist = std::max(0.0, defense->playerAssist - dt);
                    }
                }

                sim::ShipState state = {
                    .position = transform.position,
                    .velocity = body.velocity,
                    .orientation = transform.orientation,
                    .angularVelocity = body.angularVelocity,
                };
                sim::stepShipFlight(state, tuning, control.input, dt);

                transform.position = state.position;
                transform.orientation = state.orientation;
                body.velocity = state.velocity;
                body.angularVelocity = state.angularVelocity;
            });
    }

    // Collision pass: ships (movers) vs each other, scenery, and celestials.
    // Swept spheres, so cruise speeds cannot tunnel through the planet.
    //
    // Zones here are opened and closed by hand rather than by the scope guard:
    // this function is one long sequence whose sections share their locals, so
    // there are no braces to hang an RAII zone on without restructuring it.
    core::Profiler& profiler = core::frameProfiler();
    const std::uint32_t buildZone = profiler.beginZone("sim.collision.build");
    m_collisionBodies.clear();
    m_collisionShipIndices.clear();

    // Ships first, so body slot i corresponds to m_collisionShipIndices[i];
    // statics (scenery without FlightBody, then celestials) follow.
    const ecs::Pool<FlightBody>& bodies = registry.storage<FlightBody>();
    const ecs::Pool<RenderShape>& shapes = registry.storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = registry.storage<Transform>();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (!bodies.contains(entityIndex)) {
            continue;
        }
        const RenderShape& shape = shapes.values()[i];
        const Transform& transform = transforms.get(entityIndex);
        const double scale = static_cast<double>(shape.scale.x);
        m_collisionShipIndices.push_back(entityIndex);
        m_collisionBodies.push_back({
            .previousPosition = transform.previousPosition,
            .position = transform.position,
            .velocity = bodies.get(entityIndex).velocity,
            .radius = modelBaseRadius(shape.model) * scale,
            .inverseMass = 1.0 / (scale * scale * scale), // mass ~ volume
        });
    }
    ecs::Pool<Projectile>& projectiles = registry.storage<Projectile>();
    const ecs::Pool<OreChunk>& oreChunks = registry.storage<OreChunk>();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (bodies.contains(entityIndex) || projectiles.contains(entityIndex) ||
            oreChunks.contains(entityIndex)) {
            // Ships were pushed above; bolts and loose ore never block
            // anything — you fly through your own ore to collect it.
            continue;
        }
        const RenderShape& shape = shapes.values()[i];
        // Gates are the third thing you fly through (Phase 8w), after the bolts
        // and ore above. A gate used to be a solid 70 m sphere, which stopped
        // the ship dead at 78 m and made "fly through the gate" impossible —
        // the aperture rule needs the doorway to be a doorway.
        //
        // Phase 9 made that the model's own `solid = false` rather than "the
        // only Cube left among statics", which was true only because the two
        // other Cube users were excluded by the test immediately above — and
        // would have silently un-solidified the next Cube-shaped static.
        if (!modelIsSolid(shape.model)) {
            continue;
        }
        const Transform& transform = transforms.get(entityIndex);
        m_collisionBodies.push_back(
            {.previousPosition = transform.position,
             .position = transform.position,
             .velocity = {},
             .radius = modelBaseRadius(shape.model) * static_cast<double>(shape.scale.x),
             .inverseMass = 0.0});
    }
    m_collisionBodies.push_back({.previousPosition = bubble.star.position,
                                 .position = bubble.star.position,
                                 .velocity = {},
                                 .radius = bubble.star.radius,
                                 .inverseMass = 0.0});
    for (const CelestialBody& planet : bubble.planets) {
        m_collisionBodies.push_back({.previousPosition = planet.position,
                                     .position = planet.position,
                                     .velocity = {},
                                     .radius = planet.radius,
                                     .inverseMass = 0.0});
    }

    // The counter is the body count: this pass is quadratic in it, so a time
    // without it says the pass is slow and never says why.
    profiler.addCounter(buildZone, m_collisionBodies.size());
    profiler.endZone(buildZone);

    m_contacts.clear();
    const std::uint32_t resolveZone = profiler.beginZone("sim.collision.resolve");
    // Pairs actually tested, stated the way resolveCollisions iterates them,
    // so the measured number can be compared against the arithmetic rather
    // than trusted alongside it.
    const std::size_t bodyCount = m_collisionBodies.size();
    profiler.addCounter(resolveZone, bodyCount > 1 ? bodyCount * (bodyCount - 1) / 2 : 0);
    sim::resolveCollisions(m_collisionBodies, kCollisionRestitution, m_contacts);
    profiler.endZone(resolveZone);

    for (std::size_t i = 0; i < m_collisionShipIndices.size(); ++i) {
        const sim::CollisionBody& body = m_collisionBodies[i];
        const std::uint32_t entityIndex = m_collisionShipIndices[i];
        registry.storage<Transform>().get(entityIndex).position = body.position;
        registry.storage<FlightBody>().get(entityIndex).velocity = body.velocity;
    }

    // Impact damage (k*v^2) through the facing the hit arrived on.
    const std::size_t shipCount = m_collisionShipIndices.size();
    std::vector<DestroyedShip> destroyedShips;
    auto applyImpact = [&](std::uint32_t bodySlot, core::DVec3 toSource, double impactSpeed) {
        if (bodySlot >= shipCount) {
            return;
        }
        const double damage = kImpactDamageFactor * impactSpeed * impactSpeed;
        if (damage < kImpactDamageMinimum) {
            return;
        }
        const std::uint32_t entityIndex = m_collisionShipIndices[bodySlot];
        ShipDefense* defense = defenses.tryGet(entityIndex);
        if (defense == nullptr || !defense->state.alive() || isDamageImmune(registry, entityIndex)) {
            return;
        }
        const core::Quat orientation = registry.storage<Transform>().get(entityIndex).orientation;
        const sim::ShieldFacing facing = sim::facingForHit(orientation, toSource);
        const sim::DamageResult result =
            sim::applyDamage(defense->state, defense->tuning, facing, static_cast<float>(damage));
        noteDamage(bubble,
                   entityIndex,
                   m_collisionBodies[bodySlot].position + toSource * m_collisionBodies[bodySlot].radius,
                   result);
        if (result.destroyed) {
            destroyedShips.push_back({.victim = entityIndex}); // rams credit no one
        }
    };
    for (const sim::Contact& contact : m_contacts) {
        applyImpact(contact.bodyA, -contact.normal, contact.impactSpeed);
        applyImpact(contact.bodyB, contact.normal, contact.impactSpeed);
    }
    for (const DestroyedShip& destroyed : destroyedShips) {
        handleShipDestroyed(bubble, destroyed.victim, destroyed.attacker);
    }
    destroyedShips.clear();

    // Projectiles: advance, expire, resolve hits. Ship spheres come from the
    // collision list built above (slot i <-> m_collisionShipIndices[i]);
    // remaining slots are statics that simply soak bolts.
    const std::uint32_t projectileZone = profiler.beginZone("sim.projectiles");
    // Every live bolt rescans the whole body list, so the cost is the product
    // and the counter has to be the product too.
    profiler.addCounter(projectileZone, projectiles.size() * m_collisionBodies.size());
    std::vector<std::uint32_t> deadProjectiles;
    for (std::size_t p = 0; p < projectiles.size(); ++p) {
        Projectile& projectile = projectiles.values()[p];
        const std::uint32_t projectileIndex = projectiles.entityIndices()[p];
        Transform& transform = registry.storage<Transform>().get(projectileIndex);
        transform.previousPosition = transform.position;
        transform.position += projectile.velocity * dt;
        projectile.lifetime -= dt;

        bool dead = projectile.lifetime <= 0.0;
        double bestT = 2.0;
        std::size_t bestSlot = m_collisionBodies.size();
        for (std::size_t slot = 0; slot < m_collisionBodies.size(); ++slot) {
            if (slot < shipCount && m_collisionShipIndices[slot] == projectile.shooterIndex) {
                continue;
            }
            double hitT = 0.0;
            if (sim::segmentHitsSphere(transform.previousPosition,
                                       transform.position,
                                       m_collisionBodies[slot].position,
                                       m_collisionBodies[slot].radius,
                                       hitT) &&
                hitT < bestT) {
                bestT = hitT;
                bestSlot = slot;
            }
        }
        if (bestSlot < m_collisionBodies.size()) {
            dead = true;
            if (bestSlot < shipCount) {
                const std::uint32_t targetIndex = m_collisionShipIndices[bestSlot];
                if (ShipDefense* defense = defenses.tryGet(targetIndex);
                    defense != nullptr && defense->state.alive() && !isDamageImmune(registry, targetIndex)) {
                    const core::DVec3 toSource = normalize(projectile.velocity) * -1.0;
                    const sim::ShieldFacing facing = sim::facingForHit(
                        registry.storage<Transform>().get(targetIndex).orientation, toSource);
                    const sim::DamageResult result =
                        sim::applyDamage(defense->state, defense->tuning, facing, projectile.damage);
                    noteDamage(bubble,
                               targetIndex,
                               transform.previousPosition +
                                   (transform.position - transform.previousPosition) * bestT,
                               result,
                               projectile.shooterIndex);
                    if (result.destroyed) {
                        destroyedShips.push_back(
                            {.victim = targetIndex, .attacker = projectile.shooterIndex});
                    }
                }
            }
        }
        if (dead) {
            deadProjectiles.push_back(projectileIndex);
        }
    }
    for (const std::uint32_t index : deadProjectiles) {
        registry.destroy(registry.entityFromIndex(index));
    }
    for (const DestroyedShip& destroyed : destroyedShips) {
        handleShipDestroyed(bubble, destroyed.victim, destroyed.attacker);
    }
    destroyedShips.clear();
    profiler.endZone(projectileZone);

    // Weapons: tick cooldowns; a held trigger fires when charged and ready.
    // Hitscan pulses and mining beams resolve in here, and both sweep the same
    // body list the projectile loop does.
    //
    // ⚑ TWO LOOPS SINCE PHASE 31 STAGE C1: ships on the outside, one ship's
    // guns on the inside. Everything that is a fact about the SHIP - where it
    // is, which way it points, whether the trigger is down, what charge is
    // left - is read once per ship, and the inner loop reads only what differs
    // gun to gun. The capacitor is why the nesting has to be this way round:
    // `drawWeaponCharge` spends one shared pool of energy, so a ship's guns
    // have to be walked together and in a defined order.
    const std::uint32_t weaponZone = profiler.beginZone("sim.weapons");
    ecs::Pool<ShipArmament>& armaments = registry.storage<ShipArmament>();

    struct PendingBolt
    {
        core::DVec3 position;
        core::Quat orientation;
        core::DVec3 velocity;
        double lifetime;
        float damage;
        std::uint32_t shooterIndex;
        ModelId model; // Phase 19: the firing weapon's, not one model for all
    };

    std::vector<PendingBolt> newBolts;

    // Mining beams land after the loop: cutting spawns ore chunks and can
    // break a rock up, and a structural change mid-iteration corrupts pools.
    struct PendingCut
    {
        std::uint32_t entityIndex;
        // Where the beam came FROM, which is what the chunks leave toward. A
        // field on the record rather than a question asked later, because
        // "later" is after the pool walk and the ship might be anybody's.
        core::DVec3 cutter;
        core::DVec3 impact;
        float units;
        bool wreck;
    };

    std::vector<PendingCut> pendingCuts;
    for (std::size_t a = 0; a < armaments.size(); ++a) {
        ShipArmament& armament = armaments.values()[a];
        const std::uint32_t entityIndex = armaments.entityIndices()[a];
        const ShipControl* control = registry.storage<ShipControl>().tryGet(entityIndex);
        // ⚑ The trigger is read out here and the cooldowns tick BELOW it, on
        // every gun, whether or not it is down. A cooldown that only ran while
        // the trigger was held would give every gun a free first shot after any
        // pause - a rate of fire no def names.
        const bool trigger = control != nullptr && control->input.trigger;
        // ⚑ ONE READ OF THE SHIP, INCLUDING WHAT IT HAS LAID ITS GUNS ON
        // (Phase 31 stage C2). Where the hull is, which way it points and who
        // it is shooting at are facts about the SHIP; `layGun` below turns
        // them into a bearing per gun.
        //
        // Only while the trigger is down, because nothing below the trigger
        // check reads it and this runs for every armed ship in the system
        // every tick - most of which are not shooting at any given moment.
        const GunneryFrame frame = trigger ? gunneryFrame(registry, entityIndex) : GunneryFrame{};
        ShipPower* power = powers.tryGet(entityIndex);
        // ⚑ ONE READ PER SHIP, on the same rule as the frame above: which
        // mounts are still there is a fact about the SHIP, and the inner loop
        // only needs to index it (Phase 31 stage F).
        const ShipMounts* mounts = registry.tryGet<ShipMounts>(registry.entityFromIndex(entityIndex));

        for (std::uint32_t g = 0; g < armament.count; ++g) {
            ShipWeapon& weapon = armament.weapons[g];
            if (weapon.cooldown > 0.0f) {
                weapon.cooldown -= static_cast<float>(dt);
            }
            // ⚑ THE FIRE GROUP IS CHECKED HERE AND NOT ONE LINE HIGHER (Phase
            // 31 stage C3), for exactly the reason the trigger is not: a gun
            // in an unselected group has to keep ticking its clock. A cooldown
            // that only ran while its group was live would give every group a
            // free first shot on the frame you switched to it, and a hull with
            // two groups would out-shoot the same guns in one.
            if (weapon.kind == WeaponKind::None || weapon.group != armament.selectedGroup ||
                weapon.cooldown > 0.0f || !trigger) {
                continue;
            }
            // ⚑⚑ A GUN WHOSE MOUNT HAS BEEN SHOT OFF DOES NOT FIRE (Phase 31
            // stage F) - "a destroyed turret that stops working", and the
            // whole of it, because a gun IS its ring. It is checked here,
            // below the cooldown tick and above everything that costs
            // something, for the reason the arc check below is: a gun that did
            // not fire must not pay the capacitor or reset its clock. A
            // destroyed mount does keep ticking its cooldown, which costs
            // nothing and means a repair does not hand back a free salvo.
            if (mounts != nullptr && weapon.mount < mounts->count &&
                mounts->mounts[weapon.mount].destroyed()) {
                continue;
            }
            // ⚑ THE MUZZLE IS THE MOUNT AND THE BEARING IS THE RING (Phase 31
            // stages C1 and C2). The muzzle used to be one invented point on
            // the boresight - the hull's collision radius plus six metres -
            // and the bearing used to BE the boresight, because there was one
            // gun, nowhere on the hull to say where it came from, and nothing
            // reading the `aim` and `arc` sat in the def file.
            //
            // ⚑⚑ AND THE ORDER MATTERS: a gun that cannot bear is refused
            // BEFORE it draws charge and before its cooldown resets, exactly
            // like a starved one. A turret whose target is round the far side
            // of its own hull has not fired, so it must not pay as if it had.
            core::DVec3 muzzle;
            core::DVec3 bearing;
            if (!layGun(frame, weapon, muzzle, bearing)) {
                continue;
            }

            // ⚑ PER-MOUNT CAPACITOR DRAW, and this `continue` is the whole of
            // it. Each gun pays its own cost as it comes up, so a salvo the
            // capacitor cannot cover fires the guns the author listed FIRST and
            // leaves the rest holding. A starved gun does not reset its
            // cooldown, so it goes off the moment the charge is back rather
            // than sitting out a cycle it never spent.
            if (power != nullptr && !sim::drawWeaponCharge(power->state, weapon.energyCost)) {
                continue;
            }
            weapon.cooldown = 1.0f / (weapon.rateOfFire > 0.01f ? weapon.rateOfFire : 0.01f);

            // A shot was definitely fired by here: the cooldown is reset and the
            // capacitor is paid. The player's own gun is at the listener; every
            // other ship's is out in the world.
            if (m_audio != nullptr) {
                if (isPlayerEntity(registry, entityIndex)) {
                    m_audio->play2D(m_audio->cues().weaponFire);
                } else {
                    m_audio->playAt(m_audio->cues().weaponFire, muzzle, bubble.system);
                }
            }

            if (weapon.kind == WeaponKind::Hitscan) {
                // Instant pulse along the gun's own bearing; first ship hit
                // takes it. That bearing was the hull's boresight for every gun
                // in the game until stage C2 gave the ring a say.
                const core::DVec3 beamEnd = muzzle + bearing * static_cast<double>(weapon.range);
                // A beam with mining_power cuts rock and hulls too (Phase 8f).
                // Whichever is nearer along the beam is what it lands on, so you
                // cannot mine through a fighter that flew into the line.
                double miningT = 2.0;
                std::uint32_t miningEntity = kNoIndex;
                bool miningWreck = false;
                if (weapon.miningPower > 0.0f && isPlayerEntity(registry, entityIndex)) {
                    const ecs::Pool<MineableRock>& rockPool = registry.storage<MineableRock>();
                    const ecs::Pool<WreckMarker>& wreckPool = registry.storage<WreckMarker>();
                    const auto sweepCuttable = [&](std::uint32_t candidate, bool isWreck) {
                        const RenderShape& candidateShape = registry.storage<RenderShape>().get(candidate);
                        const double radius = modelBaseRadius(candidateShape.model) *
                                              static_cast<double>(candidateShape.scale.x);
                        double hitT = 0.0;
                        if (sim::segmentHitsSphere(
                                muzzle, beamEnd, transforms.get(candidate).position, radius, hitT) &&
                            hitT < miningT) {
                            miningT = hitT;
                            miningEntity = candidate;
                            miningWreck = isWreck;
                        }
                    };
                    for (std::size_t r = 0; r < rockPool.size(); ++r) {
                        sweepCuttable(rockPool.entityIndices()[r], false);
                    }
                    for (std::size_t r = 0; r < wreckPool.size(); ++r) {
                        sweepCuttable(wreckPool.entityIndices()[r], true);
                    }
                }
                double bestT = 2.0;
                std::uint32_t bestTarget = 0;
                bool hit = false;
                for (std::size_t slot = 0; slot < shipCount; ++slot) {
                    const std::uint32_t targetIndex = m_collisionShipIndices[slot];
                    if (targetIndex == entityIndex) {
                        continue;
                    }
                    double hitT = 0.0;
                    if (sim::segmentHitsSphere(muzzle,
                                               beamEnd,
                                               m_collisionBodies[slot].position,
                                               m_collisionBodies[slot].radius,
                                               hitT) &&
                        hitT < bestT) {
                        bestT = hitT;
                        bestTarget = targetIndex;
                        hit = true;
                    }
                }
                if (miningEntity != kNoIndex && (!hit || miningT <= bestT)) {
                    // Per shot, so the yield works out to mining_power per second
                    // of held beam whatever the weapon's rate of fire is. The cut
                    // itself is deferred: it spawns chunk entities, and nothing
                    // may change a pool's shape while this loop walks one.
                    pendingCuts.push_back({.entityIndex = miningEntity,
                                           .cutter = frame.position,
                                           .impact = muzzle + (beamEnd - muzzle) * miningT,
                                           .units = weapon.miningPower /
                                                    (weapon.rateOfFire > 0.01f ? weapon.rateOfFire : 0.01f),
                                           .wreck = miningWreck});
                } else if (hit) {
                    if (ShipDefense* defense = defenses.tryGet(bestTarget);
                        defense != nullptr && defense->state.alive() &&
                        !isDamageImmune(registry, bestTarget)) {
                        // Which of the target's shields eats it: the arrival
                        // direction is the BEAM's, not the shooter's nose, so a
                        // turret firing aft off a fleeing freighter lands on the
                        // shield actually facing it.
                        const sim::ShieldFacing facing = sim::facingForHit(
                            registry.storage<Transform>().get(bestTarget).orientation, bearing * -1.0);
                        const sim::DamageResult result =
                            sim::applyDamage(defense->state, defense->tuning, facing, weapon.damage);
                        noteDamage(
                            bubble, bestTarget, muzzle + (beamEnd - muzzle) * bestT, result, entityIndex);
                        if (result.destroyed) { // deferred: mid-iteration
                            destroyedShips.push_back({.victim = bestTarget, .attacker = entityIndex});
                        }
                    }
                }
            } else {
                newBolts.push_back({
                    .position = muzzle,
                    // ⚑ DRAWN THE WAY IT WAS FIRED, not the way the hull faces
                    // (Phase 31 stage C2). A bolt is a long thin box, so while
                    // every gun shot down the boresight the hull's own
                    // orientation was indistinguishable from the right answer;
                    // the first shot to leave a ring at an angle would have
                    // been drawn sideways to its own flight.
                    .orientation = facingRotation(bearing),
                    .velocity = frame.velocity + bearing * static_cast<double>(weapon.projectileSpeed),
                    .lifetime =
                        static_cast<double>(weapon.range) /
                        static_cast<double>(weapon.projectileSpeed > 1.0f ? weapon.projectileSpeed : 1.0f),
                    .damage = weapon.damage,
                    .shooterIndex = entityIndex,
                    .model = weapon.boltModel,
                });
            }
        }
    }
    for (const DestroyedShip& destroyed : destroyedShips) {
        handleShipDestroyed(bubble, destroyed.victim, destroyed.attacker);
    }
    for (const PendingCut& cut : pendingCuts) {
        if (cut.wreck) {
            (void)cutWreck(bubble, cut.cutter, cut.entityIndex, cut.units);
            m_combatEffects.spawnImpact(bubble.system, cut.impact, false);
            if (m_audio != nullptr) {
                m_audio->playAt(m_audio->cues().miningCut, cut.impact, bubble.system);
            }
            continue;
        }
        const MineableRock* rock = registry.storage<MineableRock>().tryGet(cut.entityIndex);
        if (rock == nullptr) {
            continue; // two beams on one rock in a tick; the first broke it up
        }
        const std::uint32_t field = rock->field;
        const std::uint32_t index = rock->index;
        const std::uint32_t commodity = rock->commodity;
        const float total = rock->totalUnits;
        (void)cutRock(bubble, cut.cutter, cut.entityIndex, cut.units);
        // ⚑ A cut can only happen where the player is standing today - the
        // beam is gated on `isPlayerEntity` above - so this frame is always the
        // listener's. Named anyway, because "the site is unreachable" is not a
        // reason to write a line that would be wrong when it is not, and stage
        // B left the mining path per-system exactly so an NPC could cut.
        m_combatEffects.spawnImpact(bubble.system, cut.impact, false);
        if (m_audio != nullptr) {
            m_audio->playAt(m_audio->cues().miningCut, cut.impact, bubble.system);
        }
        if (m_mining.unitsLeft(bubble.system, field, index, total) <= 0.0f) {
            registry.destroy(registry.entityFromIndex(cut.entityIndex)); // it broke up
            m_rockEvents.push_back({.commodity = commodity, .units = total});
        }
    }
    for (const PendingBolt& bolt : newBolts) {
        const ecs::Entity e = registry.create();
        registry.emplace<Transform>(e,
                                    Transform{.position = bolt.position,
                                              .previousPosition = bolt.position,
                                              .orientation = bolt.orientation,
                                              .previousOrientation = bolt.orientation});
        registry.emplace<RenderShape>(e, RenderShape{.scale = {0.3f, 0.3f, 4.0f}, .model = bolt.model});
        registry.emplace<Projectile>(e,
                                     Projectile{.velocity = bolt.velocity,
                                                .lifetime = bolt.lifetime,
                                                .damage = bolt.damage,
                                                .shooterIndex = bolt.shooterIndex});
    }
    profiler.endZone(weaponZone);

    // Mining (Phase 8f): rock tumble, chunk drift and collection, and the
    // wreck reconcile. The eighth and last per-system zone.
    {
        const std::uint32_t zone = profiler.beginZone("sim.mining");
        tickSystemMining(bubble, dt);
        profiler.endZone(zone);
    }
}

void SpaceWorld::tick(double dt)
{
    core::Profiler& profiler = core::frameProfiler();
    // The run's own clock. Market intel is stamped against it, so it advances
    // whether the player is docked or flying — a price you read an hour ago
    // is an hour old either way.
    m_worldSeconds += dt;
    // Coarse-layer mining (Phase 8f): wrecks age and refinery orders cook
    // whether the player is watching them or three systems away
    // (decisions/005). ⚑ Hoisted out of `sim.mining` at Phase 38 stage B and
    // given its own zone: it is galaxy-wide work that must run ONCE, and the
    // zone it used to share is per-system now, so leaving it there would have
    // ticked the whole coarse layer k times and measured it as k systems'
    // worth of rock tumble.
    {
        const std::uint32_t zone = profiler.beginZone("sim.coarse.mining");
        m_mining.tick(dt);
        profiler.endZone(zone);
    }

    // ⚑⚑⚑⚑ THE TICK NESTS (Phase 38 stage B). One pass per instantiated
    // system, in bubble order, and the player's is the front one - so the
    // scratch buffers, `m_appliedInput` and the thruster/audio feedback below
    // all describe the player's system by the time anything reads them.
    //
    // ⚑⚑ INDEXED RATHER THAN RANGE-FOR ON PURPOSE. Stage C is what starts
    // opening and closing bubbles against a retention policy, and this is the
    // loop it will do it from; an iterator into `m_bubbles` would not survive
    // that, where an index and a size re-read each pass will.
    for (std::size_t bubbleSlot = 0; bubbleSlot < m_bubbles.size(); ++bubbleSlot) {
        tickSystem(*m_bubbles[bubbleSlot], dt);
    }
    // ⚑⚑⚑ AND THE HAZARD IN THE BUBBLES NOBODY IS WATCHING (the user's ruling
    // 12, stage D). Between the loop and the cooling sweep, because it can kill
    // a captain and killing one releases the bubble their order was holding.
    rollHeldBubbleHazard(dt);
    // ⚑⚑⚑ AND THE COOLING BUBBLES AGE (Phase 38 stage C). AFTER the loop
    // rather than before it: a bubble whose last second is this one is still
    // simulated for it, so what the player finds on jumping back is a system
    // that ran out the whole window rather than one short. The player's own
    // bubble is never a candidate - it is held open by the player being in it.
    releaseCooledBubbles(dt);

    // Feedback bookkeeping.
    m_combatEffects.tick(dt);
    // The collection ticker is a HUD readout, not state: it fades. ⚑ It sat at
    // the bottom of `tickMining` until stage B and moved here rather than into
    // the per-system half, because it counts what the PLAYER scooped up and
    // there is only one of those however many systems are running.
    if (m_collectTicker > 0.0f) {
        m_collectTickerAge += dt;
        if (m_collectTickerAge > 2.0) {
            m_collectTicker = 0.0f;
            m_collectTickerAge = 0.0;
        }
    }
    if (m_playerDamageTimer > 0.0f) {
        m_playerDamageTimer -= static_cast<float>(dt);
        if (m_playerDamageTimer < 0.0f) {
            m_playerDamageTimer = 0.0f;
        }
    }

    // Thruster visuals are player-only for now (NPC plumes: Phase 6 feedback).
    m_thrusters.tick(shipState(), shipTuning(), m_appliedInput, dt);

    // The engine loop follows the same input the plumes do, so what you hear
    // and what you see come from one number. Docked is silent: the drive is off.
    if (m_audio != nullptr) {
        float throttle = 0.0f;
        if (!isDocked()) {
            throttle = core::length(m_appliedInput.linear);
            if (m_appliedInput.boost || m_appliedInput.cruise) {
                throttle = std::max(throttle, 1.0f);
            }
        }
        m_audio->setEngineThrottle(throttle);
    }

    // Coarse-layer economy: galaxy-wide, same clock as everything else
    // (decisions/005 — no time compression). The feedstock source is what
    // makes a mining outpost's ore come out of its own system's rock
    // (Phase 8g) instead of out of nothing.
    //
    // This is the measurement Phase 8g's spec promised and could not take:
    // raising the trader fleet was known to spend time here and nothing could
    // say how much.
    {
        const std::uint32_t zone = profiler.beginZone("sim.coarse.economy");
        m_economy.tick(m_galaxy, dt, &m_feedstock);
        // Read inside the same zone the tick was taken in, because the list is
        // only valid until the next one (Phase 8x §E). A haul ending is the
        // one thing an escort contract is waiting for.
        for (const sim::TraderArrival& arrival : m_economy.arrivals()) {
            m_missions.notifyTraderArrived(arrival.trader, arrival.system);
        }
        profiler.endZone(zone);
    }

    // The player's own coarse layer (Phase 39 stage B): captains on a standing
    // order think, fly, arrive and are exposed to what their route runs
    // through. Immediately after the economy tick and before the reconcile
    // below, because a captain trades against the prices that tick just moved
    // and the reconcile has to see the leg they ended up on.
    {
        const std::uint32_t zone = profiler.beginZone("sim.coarse.captains");
        tickCaptains(dt);
        profiler.endZone(zone);
    }

    // Bodies for the coarse traders flying a leg here (Phase 8x). Immediately
    // after the economy tick, because that is the one place a trader's route
    // can change — this is the LOD promotion §2 committed to and the coarse
    // layer has been waiting for since Phase 7.
    //
    // ⚑⚑ "HERE" IS A SET NOW, NOT A COMPARISON (Phase 38 stage B). Both
    // reconciles asked `== m_currentSystem`; they ask each instantiated bubble
    // in turn instead, which is `decisions/015`'s policy said out loud. The
    // presence marks they keep are indexed by trader and by market - both
    // galaxy-wide - so they are cleared ONCE above the loop: clearing them per
    // bubble would let each pass unmark what the one before it had claimed and
    // spawn the whole fleet again.
    {
        const std::uint32_t zone = profiler.beginZone("sim.puppets");
        beginPuppetReconcile(dt);
        for (std::size_t bubbleSlot = 0; bubbleSlot < m_bubbles.size(); ++bubbleSlot) {
            SystemBubble& bubble = *m_bubbles[bubbleSlot];
            syncTraderPuppets(bubble);
            // And a hull for every captain of yours flying a leg through here
            // (Phase 39 stage B). The promotion the phase's split is built on:
            // the record goes on being the truth and the body is paced to it,
            // so there is never a moment when a captain is two things.
            syncCaptainPuppets(bubble);
            // ⚑⚑ AND THE STATIONARY HALF, WHICH IS NOT A PROMOTION AT ALL AND
            // IS RUN FROM THE SAME PLACE BECAUSE IT IS THE SAME QUESTION (Phase
            // 39 stage C). A mining captain's bubble is held open for as long as
            // the order stands, so there is no coarse record to promote out of
            // and no clock to be paced against: this ticks the work itself. It
            // sits after `syncCaptainPuppets` so that the two never argue over
            // one `CaptainPuppet` - each skips the other's order kinds, and the
            // order is what says which.
            tickStationaryCaptains(bubble, dt);
            // ⚑⚑ AND THE ESCORT, WHICH IS NEITHER OF THOSE TWO AND SO GETS ITS
            // OWN LINE (stage D). It is not a promotion, because there is no
            // coarse record to promote; and it does not hold a bubble, because
            // the bubble it wants is the player's and that one is never cooled.
            // It skips every bubble but the front one, and `escorting()` is what
            // keeps it from arguing with the two calls above over one puppet.
            tickEscortCaptains(bubble, dt);
            // And a ship at the rock for every outpost here that is digging
            // (Phase 8x stage 6). Same reconcile, same rule, a different coarse
            // actor: an extractor's draw is activity the sim has been
            // performing since Phase 8g with nothing in the sky to show for it.
            syncMinerPuppets(bubble, dt);
        }
        profiler.endZone(zone);
    }

    // Coarse-layer faction sim (Phase 8b): drift/decay here; due decisions
    // are dispatched by GameContent (Lua faction_think or the default rule).
    {
        const std::uint32_t zone = profiler.beginZone("sim.coarse.factions");
        // The economy goes in because a war now costs the traffic that flies
        // through it (Phase 8x §C), and the current system goes in because
        // losses happen where the player is NOT: here a hauler dies by being
        // shot, in the open, and leaves a wreck.
        m_factionSim.tick(dt, &m_economy, m_currentSystem);
        drainContestResolutions();
        drainTraderLosses();
        profiler.endZone(zone);
    }

    // Coarse-layer missions (Phase 8c): deadlines and position objectives
    // here; the board hook and campaign flavor run in GameContent against
    // the events this drains.
    const std::uint32_t missionZone = profiler.beginZone("sim.coarse.missions");
    m_missions.tick(dt);
    if (!isDocked()) {
        m_missions.notifyPosition(m_currentSystem, shipState().position);
    }
    processMissionEvents();
    // The objective's nav slot follows whatever the events above did to the
    // tracked mission (Phase 8i). Checked here rather than hooked onto each of
    // accept/track/abandon/complete because the slot is derived state and this
    // is a comparison of three fields, not a rebuild.
    syncObjectiveTarget();
    profiler.endZone(missionZone);

    // Scanning (Phase 8e): pulse recharge plus the held target scan.
    {
        const std::uint32_t zone = profiler.beginZone("sim.scanning");
        tickScanning(dt);
        profiler.endZone(zone);
    }

    // Docking clearance (Phase 8r): the countdown, the comms fade, and the
    // arrival test that turns flying into a berth into being docked.
    {
        const std::uint32_t zone = profiler.beginZone("sim.docking");
        tickDocking(dt);
        // The stop (Phase 36 stage C) rides the same zone and sits directly
        // after the clearance countdown because it is the same kind of thing:
        // a timed, revocable grant with an arrival test on the end of it. It
        // runs AFTER the flight step above, so the two positions its cone rule
        // compares are both this frame's.
        tickInspection(dt);
        profiler.endZone(zone);
    }

    // Flying through a gate is what jumps you (Phase 8v). Safe to run mid-tick
    // because it only ARMS the transition — the loadSystem it eventually
    // causes happens from the frame loop, in advanceJumpTransition.
    tickGateCrossing();

    // Deferred death respawn into the last-dock system (see member comment).
    if (m_pendingRespawnSystem != kNoIndex) {
        const std::uint32_t system = m_pendingRespawnSystem;
        m_pendingRespawnSystem = kNoIndex;
        m_jump.clear(); // dying outranks travelling
        loadSystem(system, kNoIndex);
        if (m_lastDockStation != kNoIndex && m_lastDockStation < m_galaxy.systems[system].stations.size()) {
            m_dockedStation = m_lastDockStation;
            const core::DVec3 pad = dockPoint(m_dockedStation);
            Transform& transform = playerRegistry().storage<Transform>().get(playerEntityIndex());
            transform.position = pad;
            transform.previousPosition = pad;
            // loadSystem faced the ship at station 0 a moment ago; this moves it
            // to whichever station it actually woke at, so the heading has to
            // move with it (Phase 10). Facing the port you are parked at is the
            // same rule a fresh start gets.
            transform.orientation =
                lookAlong(m_galaxy.systems[system].stations[m_dockedStation].position - pad);
            transform.previousOrientation = transform.orientation;
            m_playerSpawn = pad;
            m_dockEventPending = true; // fresh board at the respawn dock
            // You wake up inside it, so you know it (Phase 8z). This runs after
            // loadSystem's own identifyTouchedObjects, which saw no dock yet.
            identifyTouchedObjects(kNoIndex);
        }
    }
}

// ⚑⚑⚑ WHERE ON THE SHIP THE SHOT LANDED, AND WHAT IT COST THAT PLACE (Phase
// 31 stage F). Two rules, and neither invents a number:
//
//   1. WHAT REACHES A MOUNT is what got past the shields - `armorAbsorbed +
//      hullDamage`. A shield is a bubble around the whole hull, so a facing
//      that ate the shot ate it on behalf of everything bolted underneath;
//      that is `decisions/014`'s "external mounts resolve hits against `at`,
//      internal mounts are reachable only once armour and hull are
//      compromised", with the external half read off the damage layering the
//      game already has rather than off a new fraction.
//
//   2. WHICH MOUNT is the one whose BEARING from the hull's centre is nearest
//      the bearing the hit arrived on, within `kMountHitCosine`. A bearing and
//      not a distance because a hit position sits on the collision SPHERE,
//      which is bigger than the mesh inside it - so distance-to-mount is
//      mostly a fact about the sphere while direction is the question the
//      player is answering when they line up on a tail.
//
// ⚑ The mount's own damage is NOT taken off the hull. A mount is a separate
// pool bolted to the outside of the ship, which is the whole of what makes
// disabling a distinct act from killing: a freighter whose drive you shot off
// is still a whole freighter, and one you shot to pieces has an intact drive
// in the wreck. Spending one hit twice is the point.
void SpaceWorld::damageMounts(ecs::Registry& registry,
                              std::uint32_t system,
                              std::uint32_t targetIndex,
                              const core::DVec3& hitPosition,
                              const sim::DamageResult& result)
{
    const float reaching = result.armorAbsorbed + result.hullDamage;
    if (reaching <= 0.0f) {
        return; // the facing held; nothing under it was touched
    }
    const ecs::Entity entity = registry.entityFromIndex(targetIndex);
    ShipMounts* mounts = registry.tryGet<ShipMounts>(entity);
    const Transform* transform = registry.tryGet<Transform>(entity);
    if (mounts == nullptr || transform == nullptr || mounts->count == 0) {
        return;
    }
    const core::DVec3 offset = hitPosition - transform->position;
    // ⚑ A hit exactly at the hull's centre has no bearing to read, so no
    // EXTERNAL mount can be picked for it - but its hull damage still reaches
    // the internals below, which is why this zeroes the arrival rather than
    // returning. `normalize` gives a zero vector back, whose dot with every
    // mount is 0 and therefore under the cone: no external mount wins, and the
    // second pass still runs.
    const core::DVec3 bearing = dot(offset, offset) > 0.0 ? normalize(offset) : core::DVec3{};
    // Into the hull's own frame, where `at` is written. The SIM orientation
    // rather than an interpolated one: this is the pose the damage was
    // resolved against a tick ago, the same reason `layGun` reads it.
    const core::Vec3 arrival = rotate(conjugate(transform->orientation), toVec3(bearing));

    // ⚑⚑ A MOUNT AT THE HULL'S OWN CENTRE IS EXCLUDED BY THE ARITHMETIC AND
    // NOT BY A BRANCH, and that is worth a compile-time promise rather than a
    // comment: `normalize` returns a zero vector for one, whose dot with any
    // arrival is exactly 0, so it loses to every cone narrower than a
    // hemisphere.
    //
    // ⚑ WHICH MAKES THE `external` CHECK BELOW BELT-AND-BRACES, AND IT IS KEPT
    // ANYWAY. An internal mount is exactly one with no `at` (that is what the
    // parser means by the flag), so its `at` IS the origin and this assert
    // already excludes it - turning the flag off changes no test in the suite,
    // which was checked rather than assumed. It stays because it is
    // `decisions/014` rule 2 said where a reader looks for it, and because
    // widening the cone past a hemisphere would otherwise silently make every
    // internal mount shootable.
    static_assert(kMountHitCosine > 0.0f,
                  "a mount at the hull centre has a zero bearing, excluded only by a positive cone");
    std::uint32_t best = mounts->count;
    float bestAlignment = kMountHitCosine;
    for (std::uint32_t m = 0; m < mounts->count; ++m) {
        const MountCondition& condition = mounts->mounts[m];
        if (!condition.external || condition.destroyed()) {
            continue;
        }
        const core::Vec3 at{condition.at[0], condition.at[1], condition.at[2]};
        const float alignment = dot(arrival, normalize(at));
        if (alignment > bestAlignment) {
            bestAlignment = alignment;
            best = m;
        }
    }

    // Spending a hit on one place, and announcing it if that finishes it. A
    // lambda because there are two passes below and only one of them picks a
    // mount by where the shot came in.
    const RenderShape* shape = registry.tryGet<RenderShape>(entity);
    const float hullScale = shape != nullptr ? shape->scale.x : 1.0f;
    auto spendOn = [&](MountCondition& mount, float amount) {
        mount.hp -= amount;
        if (mount.hp > 0.0f) {
            return;
        }
        mount.hp = 0.0f;
        // A mount going is a visible event or it is nothing at all. The
        // fireball is scaled off the hull so a freighter's drive going reads
        // as bigger than a shuttle's nose gun, and it is spawned AT THE MOUNT
        // rather than at the impact - the shot arrived on the collision
        // sphere, the thing that blew up is on the hull. An INTERNAL mount is
        // at the hull's own centre, which is exactly where a fireball for
        // something deep inside the ship belongs.
        const core::Vec3 at{mount.at[0] * hullScale, mount.at[1] * hullScale, mount.at[2] * hullScale};
        const core::DVec3 where = transform->position + toDVec3(rotate(transform->orientation, at));
        m_combatEffects.spawnExplosion(system, where, hullScale * 0.5f);
        if (m_audio != nullptr) {
            if (isPlayerEntity(registry, targetIndex)) {
                m_audio->play2D(m_audio->cues().explosion);
                m_audio->play2D(m_audio->cues().alarm);
            } else {
                m_audio->playAt(m_audio->cues().explosion, where, system);
            }
        }
    };

    if (best < mounts->count) {
        spendOn(mounts->mounts[best], reaching);
    }

    // ⚑⚑⚑ AND THE OTHER HALF OF `decisions/014` RULE 2: AN INTERNAL MOUNT IS
    // REACHED ONCE THE ARMOUR IS GONE (Phase 31 stage F2). `hullDamage` is
    // non-zero only after `applyDamage` has spent the shield facing AND the
    // armour, so the doc's "reachable only once armour and hull are
    // compromised" needs no new condition of its own - it is already the name
    // of a field on the result.
    //
    // ⚑⚑ THE TWO PASSES ARE INDEPENDENT AND A HIT CAN COST BOTH. They are
    // different mechanisms on one shot: an external mount is hit because it is
    // physically in the way, an internal one because the plating over it has
    // failed. A hull breach that spared the sensor suite because a cargo pod
    // happened to be on the same bearing would be geometry deciding something
    // it knows nothing about.
    // ⚑ An early-out and not a second rule: what an internal mount is spent is
    // `result.hullDamage` below, so a hit that never reached the hull would
    // spend zero on it anyway. It is here because every hit in the game comes
    // through this function, most of them are stopped by a shield or by
    // plating, and walking the mount list to subtract nothing is a walk for
    // nothing.
    //
    // ⚑⚑ WHICH MEANS THE GUARD AND THE AMOUNT ARE ONE RULE WRITTEN TWICE, and
    // that is worth knowing before changing either: a counterfactual that
    // widens only one of them comes back GREEN, because whichever half is left
    // alone still enforces the rule. Both were tried, separately, and neither
    // moved a test until both were changed together.
    if (result.hullDamage <= 0.0f) {
        return;
    }
    // ⚑ THE ONE WITH THE MOST LEFT TAKES IT, WHICH SPREADS THE DAMAGE EVENLY
    // AND NEEDS NO RANDOMNESS. There is no geometry to tell one internal mount
    // from another - that is what internal MEANS - so any pick is arbitrary,
    // and the two arbitrary picks worth having are "always the same one" and
    // "share it out". Sharing it out is the one that leaves a ship degrading
    // rather than losing whole subsystems while others sit untouched, and it
    // is deterministic, which a save format and a test both want.
    std::uint32_t deepest = mounts->count;
    for (std::uint32_t m = 0; m < mounts->count; ++m) {
        const MountCondition& condition = mounts->mounts[m];
        if (condition.external || condition.destroyed()) {
            continue;
        }
        if (deepest >= mounts->count || condition.hp > mounts->mounts[deepest].hp) {
            deepest = m;
        }
    }
    if (deepest < mounts->count) {
        spendOn(mounts->mounts[deepest], result.hullDamage);
    }
}

void SpaceWorld::noteDamage(SystemBubble& bubble,
                            std::uint32_t targetIndex,
                            const core::DVec3& hitPosition,
                            const sim::DamageResult& result,
                            std::uint32_t attackerIndex)
{
    ecs::Registry& registry = bubble.registry;
    // ⚑⚑ FIRST, AND ABOVE THE PLAYER EARLY-OUT BELOW (Phase 31 stage F). It
    // is in this function because this is the one funnel every hit in the game
    // passes through carrying all three facts it needs - who was hit, where,
    // and how much got past what - and it is FIRST because the early return
    // twenty lines down is about who gets a bounty assist. The player's own
    // mounts are shot off exactly like everybody else's, and hanging this off
    // the bottom of the function is how they would not be.
    damageMounts(registry, bubble.system, targetIndex, hitPosition, result);

    const bool shieldHit = result.shieldAbsorbed >= result.armorAbsorbed + result.hullDamage;
    m_combatEffects.spawnImpact(bubble.system, hitPosition, shieldHit);
    if (m_audio != nullptr) {
        const sol::audio::SoundId cue = shieldHit ? m_audio->cues().hitShield : m_audio->cues().hitHull;
        // A hit on the player is a hit on the listener: 2D, because the sound
        // is your own hull and it has no direction to come from.
        if (isPlayerEntity(registry, targetIndex)) {
            m_audio->play2D(cue);
            // The alarm is for damage that is actually costing you something.
            // Every shield hit would make it a drone rather than a warning.
            if (result.hullDamage > 0.0f) {
                m_audio->play2D(m_audio->cues().alarm);
            }
        } else {
            m_audio->playAt(cue, hitPosition, bubble.system);
        }
    }
    if (isPlayerEntity(registry, targetIndex)) {
        m_playerDamageTimer = kDamageFlashSeconds;
        return; // the player assisting their own death is not a thing
    }
    // Phase 8l: re-arm the victim's assist window so a kill someone else
    // finishes still counts toward a bounty the player was fighting for.
    if (isPlayerEntity(registry, attackerIndex)) {
        if (ShipDefense* defense = registry.storage<ShipDefense>().tryGet(targetIndex)) {
            defense->playerAssist = kAssistSeconds;
        }
    }
    // Phase 8x §D: tell the victim who is shooting it. Damage is the one
    // place in the game that knows this for certain, and every hit already
    // funnels through here, so one line gives every pilot in the game the
    // fact it was missing - a hauler can run from the ship actually attacking
    // it rather than from whatever entity 0 happened to be, and a raider
    // cruising off after cargo can answer the patrol on its tail.
    if (attackerIndex != kNoIndex && attackerIndex != targetIndex) {
        if (ShipPilot* pilot = registry.storage<ShipPilot>().tryGet(targetIndex)) {
            pilot->threatIndex = attackerIndex;
            pilot->threatTimer = static_cast<float>(kThreatMemorySeconds);
        }
    }
    // Phase 30 stage C: and tell whoever polices this place. The same argument
    // the comment above makes is why the hook belongs here - damage is the one
    // site that knows who shot whom for certain, and every hit funnels through
    // it. `considerResponse` decides whether it is an incident and throttles a
    // burst of hits into one call.
    considerResponse(bubble, targetIndex, attackerIndex, hitPosition);
}

void SpaceWorld::handleShipDestroyed(SystemBubble& bubble,
                                     std::uint32_t entityIndex,
                                     std::uint32_t attackerIndex)
{
    // ⚑⚑⚑⚑ FOUR CONSEQUENCES USED TO BE KEYED ON `m_currentSystem` AND ARE
    // KEYED ON `bubble.system` NOW (Phase 38 stage B): the mission kill, the
    // contest pressure, the trader loss and the wreck. `m_currentSystem` is
    // where the PLAYER is standing, which was the same thing as where the
    // dying ship was for thirty phases and stops being so the moment a second
    // bubble ticks. The wreck is the one that shows: its `position` is a DVec3
    // in the dead ship's own frame, so charging it to the player's system puts
    // a derelict in open space beside the player's star.
    //
    // ⚑ This is Phase 37 stage E's finding one phase later - key a consequence
    // on the FACTS, not on the call site - and the fact was always in hand,
    // because it is the registry the victim was walked out of.
    ecs::Registry& registry = bubble.registry;
    // ⚑⚑⚑⚑ AND THE HULL IS BURIED, WHICH IS THE INTERIM STAGES B AND C BOTH
    // WROTE A DATE ON (stage D). Both took the cargo and left the ship, in the
    // same words - "danger takes the HOLD and not the hull, because the death
    // path is stage D's" - and both said what that cost: the body was respawned
    // at the dock on the next tick with the hold intact, so being killed was a
    // positive statement that the danger is free.
    //
    // `killCaptain` is the whole of it and it is called here, in the one
    // function every death in the game already routes through, because the fact
    // that decides it is the registry the victim was walked out of. See there
    // for what the player loses; what this site owes is only that the order of
    // operations is right - the captain is struck off BEFORE the wreck is made
    // below, so the wreck is composed from `bubble.spawnedShips`, which copied
    // the name at spawn and does not care that the person is gone.
    if (const CaptainPuppet* puppet = registry.storage<CaptainPuppet>().tryGet(entityIndex);
        puppet != nullptr && puppet->captainIndex < m_captains.size()) {
        killCaptain(puppet->captainIndex, bubble.system);
    }
    // Fireball at the wreck site, scaled by the hull.
    const core::DVec3 wreckPosition = registry.storage<Transform>().get(entityIndex).position;
    m_combatEffects.spawnExplosion(
        bubble.system, wreckPosition, registry.storage<RenderShape>().get(entityIndex).scale.x);
    if (m_audio != nullptr) {
        if (isPlayerEntity(registry, entityIndex)) {
            m_audio->play2D(m_audio->cues().explosion);
        } else {
            m_audio->playAt(m_audio->cues().explosion, wreckPosition, bubble.system);
        }
    }
    if (isPlayerEntity(registry, entityIndex)) {
        // Cargo is lost either way (decisions/007).
        std::fill(m_playerCargo.begin(), m_playerCargo.end(), 0.0f);
        if (m_hardcore) {
            // Hardcore: the run is over — the caller deletes the save (see
            // consumeHardcoreDeath) and a fresh run starts at the new-game
            // system in the starter ship. The world itself keeps running.
            SOL_LOG_WARN("ship destroyed - HARDCORE: run over; save will be deleted");
            m_hardcoreDeathPending = true;
            m_playerCredits = 1'000.0;
            // A hardcore death is a new run, so it gets a new run's switch -
            // the same reset the new-game path does, for the same reason.
            m_transponderOn = true;
            resetFleetToStarter();
            m_lastDockSystem = kNoIndex;
            m_lastDockStation = kNoIndex;
            m_dockedStation = kNoIndex;
            m_pendingRespawnSystem = m_startSystem;
            applyActiveLoadout();
        } else if (m_lastDockSystem != kNoIndex && m_lastDockSystem != m_currentSystem) {
            // Default death rule: wake at the last dock, insurance deductible
            // charged (never docked yet: the system spawn point stands in).
            // Cross-system respawn defers to end of tick.
            const double deductible = std::min(insuranceDeductible(), m_playerCredits);
            m_playerCredits -= deductible;
            SOL_LOG_WARN("ship destroyed - waking at last dock in '%s' (insurance %.0f cr)",
                         m_galaxy.systems[m_lastDockSystem].name.c_str(),
                         deductible);
            m_pendingRespawnSystem = m_lastDockSystem;
        } else if (m_lastDockSystem == m_currentSystem && m_lastDockStation != kNoIndex) {
            const double deductible = std::min(insuranceDeductible(), m_playerCredits);
            m_playerCredits -= deductible;
            SOL_LOG_WARN("ship destroyed - waking at the last dock (insurance %.0f cr)", deductible);
            m_dockedStation = m_lastDockStation;
            m_playerSpawn = dockPoint(m_dockedStation);
            m_dockEventPending = true; // fresh board at the respawn dock
        } else {
            const double deductible = std::min(insuranceDeductible(), m_playerCredits);
            m_playerCredits -= deductible;
            SOL_LOG_WARN(
                "ship destroyed - respawning in %s (insurance %.0f cr)", currentSystemName(), deductible);
        }
        Transform& transform = registry.storage<Transform>().get(entityIndex);
        transform = Transform{.position = m_playerSpawn, .previousPosition = m_playerSpawn};
        registry.storage<FlightBody>().get(entityIndex) = FlightBody{};
        ShipDefense& defense = registry.storage<ShipDefense>().get(entityIndex);
        sim::resetDefense(defense.state, defense.tuning);
        ShipPower& power = registry.storage<ShipPower>().get(entityIndex);
        power.state = sim::PowerState{.weaponCharge = power.tuning.weaponCapacitor};
        // ⚑⚑ AND THE MOUNTS (Phase 31 stage F2). `decisions/007` is that death
        // costs the cargo and an insurance deductible and puts you back in THE
        // SAME SHIP AND FIT - and a fit with a hole shot in it is not that fit.
        // Without this line a player pays the deductible and wakes up flying a
        // hull whose gun and shield generator are permanently gone, with no
        // repair anywhere in the game to put them back.
        //
        // ⚑ This block is the list of everything the old damage model could
        // leave broken - transform, body, defences, capacitor - and mount
        // condition is simply the newest member of it. That is why the omission
        // was invisible: nothing here was wrong, something was missing.
        if (ShipMounts* mounts = registry.tryGet<ShipMounts>(registry.entityFromIndex(entityIndex))) {
            repairMounts(*mounts);
        }
        return;
    }

    // Two rules that used to share one gate, and are not the same rule
    // (Phase 8l). Reputation (Phase 8b) is strictly the player's own kill:
    // a patrol's kill must not move the player's standing with the victim.
    // Mission credit is broader - a bounty asks whether the player was in
    // the fight, and a kill stolen by local security still leaves the raider
    // dead, which is what the contract paid for.
    //
    // ⚑⚑⚑⚑ AND NEITHER RULE APPLIES TO A HULL OF YOUR OWN, WHICH IS A LIVE
    // DEFECT STAGE D CLOSES RATHER THAN A CASE IT ADDS. A captain's freighter
    // wore the local owner's colours, so putting a stray shot into your own
    // ship called `recordShipKill` against a government you had never fought -
    // you lost standing, their enemies liked you better, a bounty contract
    // took credit, and a territory contest moved. Every one of those is
    // "whose ship is that" answered wrong, and the answer is the same one
    // `threatTier` and the hail now give: ask the thing.
    if (const ShipPilot* pilot = registry.storage<ShipPilot>().tryGet(entityIndex);
        pilot != nullptr && pilot->factionIndex < m_factionTable.size() &&
        !playerOwnedHull(registry, entityIndex)) {
        const bool playerKilled = isPlayerEntity(registry, attackerIndex);
        const ShipDefense* defense = registry.storage<ShipDefense>().tryGet(entityIndex);
        const bool playerAssisted = defense != nullptr && defense->playerAssist > 0.0;

        if (playerKilled) {
            m_factionSim.recordShipKill(pilot->factionIndex);
            SOL_LOG_INFO("kill vs %s: standing now %.1f (%s)",
                         m_factionTable[pilot->factionIndex].name.c_str(),
                         m_factionSim.standing(pilot->factionIndex),
                         playerAttitudeName(pilot->factionIndex));
        }
        if (playerKilled || playerAssisted) {
            m_missions.notifyKill(pilot->factionIndex, bubble.system);
            // Territory (Phase 8u): the same "was the player in this fight"
            // rule 8l defined decides whether a kill pushes a contest back.
            // Only the player's kills reach here - nothing simulates a war's
            // attrition, so crediting ambient dogfights would invent state
            // the sim does not have and make the meter a lie.
            const float before = m_factionSim.contestOf(bubble.system).pressure;
            m_factionSim.recordContestKill(bubble.system, pilot->factionIndex);
            const float after = m_factionSim.contestOf(bubble.system).pressure;
            if (after < before) {
                SOL_LOG_INFO("contest in system %u: pressure %.2f -> %.2f",
                             bubble.system,
                             static_cast<double>(before),
                             static_cast<double>(after));
            }
            if (!playerKilled) {
                SOL_LOG_INFO("assist vs %s: someone else finished it, bounty credited",
                             m_factionTable[pilot->factionIndex].name.c_str());
            }
        }
    }

    // A body dying is a haul failing (Phase 8x §B). The entity was only ever a
    // view; the record is what persists, so the loss goes to the coarse trader
    // through the index the puppet carries — its cargo is destroyed and it
    // returns to Idle at the market it left. The wreck and its loot fall out
    // of 8f's path below without a line of new code, which is what makes
    // raiding a hauler pay in the currency the game already has.
    //
    // despawnShip() is the no-consequence sibling of this, and stays that way:
    // a trader that merely flies out of the player's system has not been lost.
    if (const TraderPuppet* puppet = registry.storage<TraderPuppet>().tryGet(entityIndex);
        puppet != nullptr && m_economy.loseTrader(puppet->traderIndex)) {
        m_factionSim.recordTraderLoss(bubble.system, puppet->traderIndex);
        // Who fired is known here and nowhere else (Phase 8x §E). It matters
        // for exactly one thing: an escort contract on a hauler the player
        // shot themselves is a failure they are charged for, not a loss they
        // are excused. 8l's assist window counts, so finishing your own charge
        // off through local security is the same betrayal.
        if (isPlayerEntity(registry, attackerIndex)) {
            m_playerKilledTraders.push_back(puppet->traderIndex);
        } else if (const ShipDefense* defense = registry.storage<ShipDefense>().tryGet(entityIndex);
                   defense != nullptr && defense->playerAssist > 0.0) {
            m_playerKilledTraders.push_back(puppet->traderIndex);
        }
    }

    // A miner dying stops its outpost digging (Phase 8x stage 6), which is the
    // same idea one actor over: the entity is a view of a draw, so removing it
    // has to reach the draw or the ship was scenery. It is the only way the
    // player can reach into a station's production directly — and it is
    // temporary, because the outpost sends another ship out.
    if (const MinerPuppet* miner = registry.storage<MinerPuppet>().tryGet(entityIndex);
        miner != nullptr && miner->market < m_minerHold.size()) {
        m_minerHold[miner->market] = kMinerReplacementLegs * m_economy.params().traderLegSeconds;
        const sim::StationMarket& row = m_economy.markets()[miner->market];
        SOL_LOG_INFO("[mining] %s loses its miner: no draw for %.0f s",
                     m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name.c_str(),
                     m_minerHold[miner->market]);
    }

    for (std::size_t i = 0; i < bubble.spawnedShips.size(); ++i) {
        if (bubble.spawnedShips[i].entity.index != entityIndex) {
            continue;
        }
        SOL_LOG_INFO("'%s' destroyed", bubble.spawnedShips[i].defId.c_str());

        // A wreck stays where it fell (Phase 8f). The record is what persists
        // — the entity to cut is materialized by tickMining, here and after a
        // jump back or a reload — and its contents are composed now, from the
        // ship that actually died, so a later def edit cannot rewrite it.
        const core::DVec3 where = registry.storage<Transform>().get(entityIndex).position;
        const std::string defId = bubble.spawnedShips[i].defId;
        const std::string name = bubble.spawnedShips[i].name;
        const ShipPilot* pilot = registry.storage<ShipPilot>().tryGet(entityIndex);
        const std::uint32_t faction = pilot != nullptr ? pilot->factionIndex : kNoIndex;
        const std::uint64_t seed =
            core::Rng(
                m_universeSeed ^ (static_cast<std::uint64_t>(entityIndex + 1) * 0x9e37'79b9'7f4a'7c15ull), 13)
                .nextU64();
        const std::uint32_t wreckId = m_mining.addWreck(bubble.system, where, defId, name, seed);
        if (wreckId != 0) {
            const assets::ShipDef* def = m_defs != nullptr ? m_defs->findShip(defId.c_str()) : nullptr;
            // Scriptless default first, so a hull always holds something even
            // if no script answers; the Lua hook may replace it before it is
            // cut into.
            (void)m_mining.setWreckContents(wreckId, defaultWreckLoot(def, seed));
            m_wreckEvents.push_back({.id = wreckId,
                                     .system = bubble.system,
                                     .defId = defId,
                                     .factionName = faction < m_factionTable.size()
                                                        ? m_factionTable[faction].name
                                                        : std::string(),
                                     .seed = seed});
        }

        registry.destroy(bubble.spawnedShips[i].entity);
        bubble.spawnedShips.erase(bubble.spawnedShips.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

Transform SpaceWorld::shipRenderTransform(float alpha) const
{
    const std::uint32_t shipIndex = playerEntityIndex();
    const Transform& transform = playerRegistry().storage<Transform>().get(shipIndex);

    Transform blended = transform;
    blended.position = transform.previousPosition +
                       (transform.position - transform.previousPosition) * static_cast<double>(alpha);
    blended.orientation = nlerp(transform.previousOrientation, transform.orientation, alpha);
    return blended;
}

void SpaceWorld::buildRenderInstances(float alpha, bool includeShip, std::vector<RenderInstance>& out) const
{
    const ecs::Pool<RenderShape>& shapes = playerRegistry().storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = playerRegistry().storage<Transform>();
    const std::uint32_t shipIndex = playerEntityIndex();

    const std::uint32_t count = static_cast<std::uint32_t>(shapes.size());
    const std::uint32_t* entityIndices = shapes.entityIndices().data();
    const RenderShape* shape = shapes.values().data();
    const double alphaD = static_cast<double>(alpha);

    out.clear();
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!includeShip && entityIndices[i] == shipIndex) {
            continue;
        }
        const Transform& transform = transforms.get(entityIndices[i]);
        // ⚑ The identity was always here and was always dropped (Phase 18):
        // the pool is being walked by entity index already, and the renderer
        // needs it to remember what this instance drew last frame. Rebuilt to
        // the full handle so a recycled slot reads as a different object.
        const ecs::Entity entity = playerRegistry().entityFromIndex(entityIndices[i]);
        out.push_back(RenderInstance{
            .position =
                transform.previousPosition + (transform.position - transform.previousPosition) * alphaD,
            .rotation = nlerp(transform.previousOrientation, transform.orientation, alpha),
            .scale = shape[i].scale,
            .model = shape[i].model,
            .key = makeInstanceKey(entity.index, entity.generation),
        });
    }
    appendFittingInstances(alpha, includeShip, out);
}

// ⚑⚑⚑ WHAT IS BOLTED TO A HULL, DRAWN WHERE IT IS BOLTED (Phase 31 stage
// E). A fitting is not an entity and must not become one: `despawnSystem`
// destroys everything in the TRANSFORM pool, a gun has none, and
// `Registry::destroy` recycles indices - so an orphaned turret would survive a
// jump and re-attach itself to whatever spawned into its owner's old slot.
// That is the same trap `kMaxShipWeapons` was chosen to make inexpressible in
// stage C1, and it is why this is a draw-time append off the owner's transform
// rather than a second entity riding along.
//
// ⚑⚑ TWO FRAMES MEET HERE AND ONLY ONE OF THEM IS INTERPOLATED, WHICH IS THE
// WHOLE REASON THIS IS NOT THREE LINES INSIDE THE LOOP ABOVE. The hull is
// DRAWN at the nlerped render transform; a turret is LAID by `layGun` off the
// sim transform, because that is the pose the gunnery answer is about. Taking
// the world bearing straight from `layGun` and hanging it off the render pose
// would make every turret swim across its own ship by whatever the hull turned
// through this tick - visible at once on a hull that turns fast and mistakable
// for a loose mount. So the bearing is brought back into the HULL's frame,
// where it is a fact about the gun and not about the moment, and the render
// pose puts it back into the world.
//
// ⚑ A gun that CANNOT BEAR is still drawn, at its stop. `layGun` sets the
// bearing on a refusal too - the ring swung as far round as it goes - and a
// turret straining against its own limit is exactly the picture a pilot needs
// when their shots are not going off.
//
// ⚑⚑⚑ A FITTING IS DRAWN WHETHER OR NOT ITS HULL IS, AND THAT IS A RULED
// DECISION RATHER THAN AN OBVIOUS ONE. The hull is hidden from the seat because
// the eye sits INSIDE it; a fitting bolted to the outside is not inside
// anything, so the reason does not carry over. Hiding it with the hull was the
// tidier rule and it cost the thing the stage is for: the shuttle's `gun_nose`
// sits at z = -6.6 against an eye at z = -5.0, so it is 1.6 m AHEAD of the
// pilot and dead centre - your own cannon, firing, in the view this game is
// actually played in. The freighter loses nothing either way: both its rings
// are behind the eye, above and below the canopy.
//
// ⚑ WHAT IT COST was a hull nobody had authored yet - one with a fitting
// forward of `kCockpitOffset` and off the centreline, which would be drawn
// hanging in space with no hull behind it. E1 accepted that deliberately
// rather than take a third rule about which fittings are cockpit-visible.
// ⚑⚑ PHASE 32 STAGE B PAID IT, and it is a rule about the HULL rather than
// about a fitting, so "where a fitting is drawn" still has one answer: see
// `hideSeatFittings` in the body below.
//
// ⚑ `kNoInstanceKey`, so a fitting gets no LOD memory and answers statelessly.
// The cockpit was the only instance without an identity until now; a fitting is
// the second, and it costs nothing real for the same reason - both meshes are
// under the cooker's triangle floor and have no chain to remember a level in.
// The alternative would be packing a mount index into a key whose upper half
// is already the entity generation, which is a save-format-shaped change for a
// LOD chain that does not exist.
void SpaceWorld::appendFittingInstances(float alpha, bool includeShip, std::vector<RenderInstance>& out) const
{
    const ecs::Pool<ShipArmament>& armaments = playerRegistry().storage<ShipArmament>();
    const ecs::Pool<ShipFittings>& fittings = playerRegistry().storage<ShipFittings>();
    const ecs::Pool<Transform>& transforms = playerRegistry().storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = playerRegistry().storage<RenderShape>();
    const double alphaD = static_cast<double>(alpha);

    // ⚑⚑⚑ WHAT E1's COCKPIT RULING LEFT OPEN, ANSWERED (Phase 32 stage B):
    // FROM THE SEAT YOU SEE YOUR OWN FITTINGS ONLY WHILE THE CABIN IS AS BIG AS
    // THE SHIP. E1 ruled that a fitting is drawn whether or not its hull is,
    // and named the cost in the block above - a hull long enough that a fitting
    // ends up "drawn hanging in space with no hull behind it". Hanging in space
    // is precisely "with nothing drawn behind it", and in the cockpit exactly
    // one thing is drawn around the eye: `cockpit.forge`, whose authored radius
    // models.toml says is measured from the SHIP ORIGIN and "comes out just
    // inside the ship's own 8 m". So the cabin's reach against the hull's is
    // the whole question, and both numbers are already authored.
    //
    // ⚑⚑ ONE COMPARISON PER HULL RATHER THAN ONE PER FITTING, AND THE SHIPPED
    // FREIGHTER IS WHY. Cut fitting-by-fitting on the same 8 m, its three hold
    // pods land at 6.6, 8.1 and 16.5 m - so a player would watch one container
    // drawn and the next, 2.4 m further aft, gone, with the cut falling
    // 0.09 m from a boundary nothing in the file knows about. A hull is either
    // small enough for its kit to read as bolted to the cabin or it is not.
    //
    // ⚑ THE SHIP THE GAME IS PLAYED IN IS UNCHANGED, WHICH IS THE POINT: the
    // shuttle's hull radius is the 8 m the cockpit was authored around, so the
    // nose gun 1.6 m ahead of the pilot - the thing E1's ruling exists for - is
    // still drawn, and so is the interceptor's at 6.4 m. What the freighter
    // loses is what E1 measured it as gaining: "both its rings are behind the
    // eye, above and below the canopy", i.e. nothing. `game.unit` asserts that
    // covering relation against the committed defs, so re-measuring either mesh
    // fails a test instead of silently deleting a pilot's own gun.
    const std::uint32_t seatIndex = playerEntityIndex();
    const bool hideSeatFittings = !includeShip && hullRadius(seatIndex) > modelBaseRadius(cockpitModel());

    // ⚑⚑ AND NOTHING IS DRAWN AT A MOUNT THAT HAS BEEN SHOT OFF (Phase 31
    // stage F). Both loops below check it, because a gun and a pod go the same
    // way: the ring is gone, so what stood in it is gone. That absence IS the
    // stage's feedback - a freighter you have worked over reads as a freighter
    // missing its drive bell from across the fight, with no icon to consult.
    const auto mountAlive = [this](std::uint32_t entityIndex, std::uint32_t mount) {
        const ShipMounts* condition =
            playerRegistry().tryGet<ShipMounts>(playerRegistry().entityFromIndex(entityIndex));
        return condition == nullptr || mount >= condition->count || !condition->mounts[mount].destroyed();
    };

    // ⚑⚑ THE STILL HALF FIRST, AND IT NEEDS NO GUNNERY FRAME AT ALL. Nothing
    // in this loop asks where a target is, because nothing in it moves: a hold
    // pod is at its mount's `at` facing its mount's `aim` for the life of the
    // fit. That is the whole reason `ShipFittings` is a second component rather
    // than a wider `ShipArmament` - see its declaration.
    const std::uint32_t fittedCount = static_cast<std::uint32_t>(fittings.size());
    const std::uint32_t* fittedIndices = fittings.entityIndices().data();
    const ShipFittings* fitted = fittings.values().data();
    for (std::uint32_t i = 0; i < fittedCount; ++i) {
        if (fitted[i].count == 0) {
            continue;
        }
        const std::uint32_t entityIndex = fittedIndices[i];
        if (hideSeatFittings && entityIndex == seatIndex) {
            continue;
        }
        const Transform* transform = transforms.tryGet(entityIndex);
        const RenderShape* shape = shapes.tryGet(entityIndex);
        if (transform == nullptr || shape == nullptr) {
            continue;
        }
        const core::DVec3 hullPosition =
            transform->previousPosition + (transform->position - transform->previousPosition) * alphaD;
        const core::Quat hullRotation = nlerp(transform->previousOrientation, transform->orientation, alpha);
        const float hullScale = shape->scale.x;
        for (std::uint32_t f = 0; f < fitted[i].count; ++f) {
            const FittedPart& part = fitted[i].parts[f];
            if (!mountAlive(entityIndex, part.mount)) {
                continue;
            }
            const core::Vec3 offset{part.at[0] * hullScale, part.at[1] * hullScale, part.at[2] * hullScale};
            out.push_back(RenderInstance{
                .position = hullPosition + toDVec3(rotate(hullRotation, offset)),
                // ⚑ The hull's own nose is the roll reference, so a pod lies
                // fore-and-aft rather than across the belly at whatever angle
                // the shortest arc happened to leave it.
                .rotation = hullRotation *
                            mountRotation({part.aim[0], part.aim[1], part.aim[2]}, {0.0f, 0.0f, -1.0f}),
                .scale = {hullScale, hullScale, hullScale},
                .model = part.model,
                .key = kNoInstanceKey,
            });
        }
    }

    const std::uint32_t count = static_cast<std::uint32_t>(armaments.size());
    const std::uint32_t* entityIndices = armaments.entityIndices().data();
    const ShipArmament* armaments_ = armaments.values().data();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entityIndex = entityIndices[i];
        if (hideSeatFittings && entityIndex == seatIndex) {
            continue;
        }
        const ShipArmament& armament = armaments_[i];
        // ⚑ Asked before the frame is built, not after. `gunneryFrame` walks the
        // selection and a target's transform, and every NPC in this galaxy
        // whose weapon def names no mesh - which is every one of them until an
        // author writes a name in - would pay for an answer nothing reads.
        bool anyDrawn = false;
        for (std::uint32_t g = 0; g < armament.count && !anyDrawn; ++g) {
            anyDrawn = armament.weapons[g].fittingModel != kNoModel;
        }
        if (!anyDrawn) {
            continue;
        }
        const Transform* transform = transforms.tryGet(entityIndex);
        const RenderShape* shape = shapes.tryGet(entityIndex);
        if (transform == nullptr || shape == nullptr) {
            continue;
        }

        const GunneryFrame frame = gunneryFrame(playerRegistry(), entityIndex);
        const core::DVec3 hullPosition =
            transform->previousPosition + (transform->position - transform->previousPosition) * alphaD;
        const core::Quat hullRotation = nlerp(transform->previousOrientation, transform->orientation, alpha);
        // The sim pose `layGun` answered in, so its world bearing can be
        // brought back to the hull's own frame.
        const core::Quat intoHull = conjugate(transform->orientation);
        const float hullScale = shape->scale.x;

        for (std::uint32_t g = 0; g < armament.count; ++g) {
            const ShipWeapon& weapon = armament.weapons[g];
            if (weapon.fittingModel == kNoModel || !mountAlive(entityIndex, weapon.mount)) {
                continue;
            }
            core::DVec3 muzzle;
            core::DVec3 bearing;
            (void)layGun(frame, weapon, muzzle, bearing);
            // ⚑ The mount's `at` scaled by the hull and rotated by it - the same
            // arithmetic `layGun` does for the muzzle, deliberately repeated
            // against the RENDER pose rather than reusing `muzzle`, which is
            // the sim pose's answer.
            const core::Vec3 offset{
                weapon.at[0] * hullScale, weapon.at[1] * hullScale, weapon.at[2] * hullScale};
            out.push_back(RenderInstance{
                .position = hullPosition + toDVec3(rotate(hullRotation, offset)),
                .rotation =
                    hullRotation * fittingRotation(rotate(intoHull, toVec3(bearing)),
                                                   core::Vec3{weapon.aim[0], weapon.aim[1], weapon.aim[2]}),
                .scale = {hullScale, hullScale, hullScale},
                .model = weapon.fittingModel,
                .key = kNoInstanceKey,
            });
        }
    }
}

void SpaceWorld::resetForNewGame(std::uint64_t seed)
{
    // ⚑⚑⚑⚑ THE DEVICE IS CARRIED ACROSS THE RESET, AND IT WAS NOT (Phase 38
    // stage D). `*this = SpaceWorld{}` is a whole-object wipe, which is what
    // makes this correct about a RUN's state and wrong about the two things
    // that are not a run's: the def database and the audio device, both
    // borrowed from `main`. `m_defs` is put back by `applyDefs`, which
    // `GameContent::restartForNewGame` calls - what the comment on that
    // function means by "the other half of the reset". Nothing ever put the
    // device back, and there is no third borrowed pointer.
    //
    // ⚑⚑⚑⚑ WHAT IT COST IS EVERY SOUND IN THE GAME EXCEPT THE UI'S. This game
    // boots to a menu, so the first thing any player does is start a run,
    // which is this function - and from that moment `m_audio` was null at
    // every one of the guarded cue sites. No gunfire, no hits, no explosions,
    // no mining beam, no docking chime, no proximity alarm, for the whole of a
    // session. Every one of those sites is written `if (m_audio != nullptr)`,
    // because a machine with no output endpoint has to keep playing, and that
    // is exactly the shape that turns a dropped pointer into silence rather
    // than a crash.
    //
    // ⚑⚑⚑ FOUND BY STAGE D'S FLIGHT AND BY NOTHING ELSE. A six-ship firefight
    // moved `sol.audio()`'s play count by zero while a console
    // `sol.play_sound_at` moved it by one - and the difference between those
    // two is the pointer: the console reaches the device through
    // `GameContent`, whose copy no reset touches. Pre-existing since the reset
    // was written in Phase 27; the audio stage is simply the one that had a
    // reason to read that number.
    GameAudio* const device = m_audio;
    *this = SpaceWorld{};
    spawn(seed);
    // Through `setAudio` rather than by assigning the member, so the ear is
    // told which system it is in as well as which device it is - the reset
    // wiped `m_currentSystem` too.
    setAudio(device);
}

bool readSaveInfo(const char* path, SaveInfo& out)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path, bytes)) {
        return false;
    }
    core::BinaryReader reader(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!reader.read(magic) || magic != kSaveMagic || !reader.read(version) || version != kSaveVersion) {
        return false; // foreign or from another version - the same row either way
    }

    // The field order below IS the v17 header order in saveTo, and the two
    // have to be read together. `dockedStation` and the two last-dock indices
    // are skipped over rather than kept: the browser has nothing to say about
    // them, but the stream cannot be skipped past them without reading them.
    //
    // ⚑⚑ THE AUTHORED DIGEST IS READ AND DISCARDED HERE, AND THE ASYMMETRY IS
    // DELIBERATE. This function is free of any world, so it has nothing to
    // compare against - and it should not acquire one. A save whose authored
    // content has changed is still perfectly describable as a FILE, which is
    // all a browser row is, and hiding it would leave a player unable to see
    // that their campaign still exists. `loadFrom` is where the comparison
    // belongs, because that is where acting on the answer is possible.
    SaveInfo info;
    std::uint32_t systemIndex = 0;
    std::uint64_t authoredDigest = 0;
    std::uint32_t dockedStation = 0;
    std::uint32_t lastDockSystem = 0;
    std::uint32_t lastDockStation = 0;
    std::uint8_t hardcore = 0;
    if (!reader.readString(info.displayName) || !reader.read(info.savedAtUnix) ||
        !reader.readString(info.systemName) || !reader.read(info.universeSeed) ||
        !reader.read(authoredDigest) || !reader.read(systemIndex) || !reader.read(dockedStation) ||
        !reader.read(lastDockSystem) || !reader.read(lastDockStation) || !reader.read(hardcore) ||
        !reader.read(info.worldSeconds) || !reader.read(info.credits)) {
        return false; // truncated: a save being written when the game died
    }
    info.hardcore = hardcore != 0;
    out = std::move(info);
    return true;
}

bool SpaceWorld::saveTo(const char* path, std::string_view displayName)
{
    core::BinaryWriter writer;
    writer.write(kSaveMagic);
    writer.write(kSaveVersion);
    // v16 header. FIRST, and deliberately so: readSaveInfo below stops as soon
    // as it has these, and anything appended later cannot push them out of
    // reach. The three fields are the ones the browser cannot derive.
    writer.writeString(displayName);
    writer.write(platform::wallClockSeconds());
    // The system name resolved HERE, where the galaxy is already in hand.
    // Doing it at read time would mean regenerating a galaxy per listed save.
    // Through currentSystemName() rather than indexing m_galaxy directly: the
    // out-of-range fallback is that accessor's business and having a second
    // spelling of it here is how the two would drift apart.
    writer.writeString(currentSystemName());
    writer.write(m_universeSeed);
    // v17: beside the seed, because it answers the same question the seed
    // does - "is this the galaxy this save was made in?" - for the half of the
    // input the seed cannot see.
    writer.write(m_authoredDigest);
    writer.write(m_currentSystem);
    writer.write(m_dockedStation);
    writer.write(m_lastDockSystem);
    writer.write(m_lastDockStation);
    writer.write(static_cast<std::uint8_t>(m_hardcore ? 1 : 0));
    writer.write(m_worldSeconds); // v9: what market intel timestamps mean
    writer.write(m_playerCredits);
    writer.write(static_cast<std::uint32_t>(m_playerCargo.size()));
    for (const float units : m_playerCargo) {
        writer.write(units);
    }
    // Fleet (v4): fits and crew as def-id strings; stored location per ship.
    writer.write(static_cast<std::uint32_t>(m_fleet.size()));
    writer.write(static_cast<std::uint32_t>(m_activeShip));
    for (const OwnedShip& ship : m_fleet) {
        writer.writeString(ship.defId);
        // v18 (Phase 31 stage B): a fitting is named by the MOUNT it occupies,
        // never by index, so an author inserting a mount cannot rearrange an
        // existing player's ship. This is what replaced the weapon id and the
        // flat component list, and it is why v17 saves are refused.
        writer.write(static_cast<std::uint32_t>(ship.fittings.size()));
        for (const ShipFitting& fitting : ship.fittings) {
            writer.writeString(fitting.mountId);
            writer.writeString(fitting.defId);
            // v21: which trigger a gun in this mount answers to. Written for
            // every fitting rather than only for guns - a cargo pod's 1 costs
            // four bytes and a conditional field is a save format that has to
            // be read twice to know how long it is.
            writer.write(fitting.group);
        }
        writer.write(static_cast<std::uint32_t>(ship.crewIds.size()));
        for (const std::string& id : ship.crewIds) {
            writer.writeString(id);
        }
        writer.write(ship.storedSystem);
        writer.write(ship.storedStation);
    }
    // Captains (v39). The NAME is written rather than a def id, because a
    // captain's name exists in no def - it is drawn from the same syllable
    // tables the cast uses - and that is what makes "a person is a new kind of
    // save-format promise" a risk this stage does not have to carry.
    writer.write(static_cast<std::uint32_t>(m_captains.size()));
    for (const Captain& captain : m_captains) {
        writer.writeString(captain.name);
        writer.writeString(captain.trade);
        writer.write(captain.who);
        writer.write(captain.ship);
        writer.write(captain.cut);
        // The order (v40), then the haul. Two records because they answer two
        // questions - see `CaptainOrder`.
        //
        // ⚑⚑ v42 ADDS NO FIELD AND STILL BUMPS, AND THAT IS THE FORMAT BEING
        // HONEST RATHER THAN CAUTIOUS. `OrderKind` gained `Patrol` and `Escort`
        // (stage D), so this same `uint8_t` now has two values a v41 save could
        // never hold - and worse, a v41 reader would take them silently, since
        // 3 and 4 are perfectly good bytes. The version is the only thing that
        // can say the vocabulary changed.
        writer.write(static_cast<std::uint8_t>(captain.order.kind));
        writer.write(captain.order.marketA);
        writer.write(captain.order.marketB);
        writer.write(static_cast<std::uint8_t>(captain.order.stopping ? 1 : 0));
        // The sell floor (v43). Rides with the order because it is part of what
        // the player TOLD them, not part of what they are doing about it.
        writer.write(captain.order.floor);
        writer.write(static_cast<std::uint8_t>(captain.haul.leg.phase));
        writer.write(captain.haul.leg.origin);
        writer.write(captain.haul.leg.market);
        writer.write(captain.haul.leg.travelRemaining);
        writer.write(captain.haul.leg.legTotal);
        writer.write(captain.haul.leg.commodity);
        writer.write(captain.haul.leg.cargo);
        writer.write(captain.haul.outlay);
        // The mining record (v41). ⚑ THE FIELD AND THE HOLD, AND DELIBERATELY
        // NOT THE ROCK: an entity index is meaningless in a bubble that has not
        // been built yet, and a bubble is rebuilt from the galaxy every load. So
        // the durable half is which field is being worked and what is aboard,
        // and the body picks a rock out of the field again on its first tick.
        writer.write(static_cast<std::uint8_t>(captain.mine.phase));
        writer.write(captain.mine.field);
        writer.write(captain.mine.rockStep);
        writer.write(captain.mine.rockSeconds);
        writer.write(captain.mine.commodity);
        writer.write(captain.mine.units);
        // How long they have stood at a counter that will not take the load
        // (v44). Saved for the same reason `rockSeconds` beside it is: a clock
        // the captain is already partway through, and a reload that zeroed it
        // would repeat the announcement and restart the half-hour wait.
        writer.write(captain.mine.stalledSeconds);
        // And the ledger (v41, moved out of the haul), which is one person's
        // lifetime rather than one order kind's - see `CaptainLedger`.
        writer.write(captain.ledger.earned);
        writer.write(captain.ledger.paid);
        writer.write(captain.ledger.losses);
    }
    // ⚑⚑ WHO DIED (v45), AND IT IS A LIST OF KEYS RATHER THAN OF PEOPLE. A
    // crew hall's roster is a pure function of the dock's seed, so the only
    // thing a save has to carry about a dead captain is that they are dead;
    // their name, trade and cut are all re-derivable and none of them is the
    // identity. Same bargain as `CastMemory` above: a sparse record of what
    // happened, beside a composed world that does not know it happened.
    writer.write(static_cast<std::uint32_t>(m_lostCaptains.size()));
    for (const std::uint64_t who : m_lostCaptains) {
        writer.write(who);
    }
    m_economy.save(writer);
    m_factionSim.save(writer); // v5: relations, war flags, standings, raids
    m_missions.save(writer);   // v6: journal, board, campaign stage
    m_survey.save(writer);     // v7: knowledge, signal state, ledger, route
    m_mining.save(writer);     // v8: rock depletion, wrecks, refinery orders
    // The cast (v33): what the player DID, and nothing else. Who is in which
    // room is derived from the seed and is re-composed on load like every other
    // composed fact - a table of 62 seats would be a second copy of something
    // the generator already knows, and the two would eventually disagree.
    writer.write(static_cast<std::uint32_t>(m_castMemory.size()));
    for (const CastMemory& memory : m_castMemory) {
        writer.write(memory.who);
        writer.write(memory.visits);
        writer.write(memory.regard);
    }
    // The transponder (v34, Phase 36 stage A): a switch the player threw, so a
    // reload that quietly re-lit it would be the game undoing a decision. One
    // byte, unconditional - a bool written only when false is a save format you
    // have to read twice to know how long it is (v21's own rule, one phase on).
    writer.write(static_cast<std::uint8_t>(m_transponderOn ? 1 : 0));
    // ⚑⚑ EVERY INSTANTIATED BUBBLE, EACH TAGGED WITH THE SYSTEM IT IS THE SKY
    // OF (v37, Phase 38 stage A). Stage A always writes exactly one - the
    // player's - and the count is written anyway rather than implied, which is
    // v21's own rule: a bool written only when false is a save format you have
    // to read twice to know how long it is, and so is a list without a length.
    //
    // ⚑ Nothing here says which one holds the player. `PlayerShip` does, it is
    // in the snapshot already, and the load below finds it that way rather than
    // trusting an index written beside it - one fact, one place.
    const ecs::Snapshot schema = makeSnapshotSchema();
    writer.write(static_cast<std::uint32_t>(m_bubbles.size()));
    for (const std::unique_ptr<SystemBubble>& bubble : m_bubbles) {
        writer.write(bubble->system);
        // ⚑⚑⚑ AND HOW LONG IT HAS LEFT (v38, Phase 38 stage C). The bubbles
        // have been written since v37, so the cooling ones are in the file
        // whether or not their clocks are - and a file that carried five
        // retained systems and no clocks would load a world that dropped four
        // of them on its first tick. Saving in the middle of a fighting
        // retreat and finding the wounded raider gone is the phase's own exit
        // failing at a save point, which is exactly where a player would not
        // think to look for it. One double, beside the system it belongs to.
        writer.write(bubble->holdSeconds);
        schema.save(bubble->registry, writer);
    }
    return platform::writeFileBytes(path, writer.data().data(), writer.size());
}

bool SpaceWorld::loadFrom(const char* path)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path, bytes)) {
        return false;
    }
    core::BinaryReader reader(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t seed = 0;
    std::uint32_t systemIndex = 0;
    std::uint32_t dockedStation = 0;
    std::uint32_t lastDockSystem = 0;
    std::uint32_t lastDockStation = 0;
    std::uint8_t hardcore = 0;
    double worldSeconds = 0.0;
    // v16's header is read and DISCARDED here, and that is not an oversight.
    // The name, the stamp and the system name describe the FILE, not the
    // world: a run loaded from "Before the gate run" and saved again belongs
    // in whatever slot the player picks next, under whatever they call it
    // then. Reading them is still mandatory - the stream has to stay aligned.
    std::string savedName;
    std::uint64_t savedAtUnix = 0;
    std::string savedSystemName;
    std::uint64_t savedAuthoredDigest = 0;
    if (!reader.read(magic) || magic != kSaveMagic || !reader.read(version) || version != kSaveVersion ||
        !reader.readString(savedName) || !reader.read(savedAtUnix) || !reader.readString(savedSystemName) ||
        !reader.read(seed) || !reader.read(savedAuthoredDigest) || !reader.read(systemIndex) ||
        !reader.read(dockedStation) || !reader.read(lastDockSystem) || !reader.read(lastDockStation) ||
        !reader.read(hardcore) || !reader.read(worldSeconds)) {
        return false; // pre-fleet or foreign save: rejected cleanly
    }

    // ⚑⚑⚑ THE OTHER HALF OF "SAME INPUT, SAME GALAXY" (Phase 29 stage D,
    // decisions/018 decision 7). The seed below decides whether to regenerate;
    // this decides whether regenerating would produce the world this save was
    // written in at all. A mod installed or removed mid-campaign changes the
    // galaxy without changing either the seed or the build, and every index in
    // the rest of this file - the player's system, their fleet's berths, every
    // market - would then point somewhere else. Refused rather than migrated,
    // for the reason every other version mismatch here is: there is no honest
    // way to move a campaign into a different galaxy.
    if (savedAuthoredDigest != m_authoredDigest) {
        SOL_LOG_ERROR("save was made against different authored content (0x%016llX, this galaxy is "
                      "0x%016llX): a [[system]] or [[constellation]] was added, changed or removed, "
                      "in game/data or in a mod",
                      static_cast<unsigned long long>(savedAuthoredDigest),
                      static_cast<unsigned long long>(m_authoredDigest));
        return false;
    }

    // Same seed => same galaxy, so the galaxy itself regenerates instead of
    // being serialized (dynamic state — the economy — saves separately).
    const bool galaxyChanged = seed != m_universeSeed || m_galaxy.systems.empty();
    if (galaxyChanged) {
        m_universeSeed = seed;
        m_galaxyParams.seed = seed;
        // ⚑ The mining params are an INPUT to generation since Phase 13, so
        // they are rebuilt first. initializeMining() further down would also
        // build them, and that is far too late: without this the regenerated
        // galaxy sites stations under a different rule than the one that
        // created the save, and a load would silently describe another world.
        buildMiningParams();
        m_galaxy = sim::generateGalaxy(m_galaxyParams, &m_miningParams);
        // ⚑⚑ AND THE COMPOSITIONS WITH IT (Phase 34 stage B). They are derived
        // from the seed and never serialized - the same bargain the galaxy
        // itself makes - so a regenerated galaxy that skipped this would leave
        // every station on its archetype's rates and quietly describe a
        // DIFFERENT economy than the one the save was made in.
        composeStations();
    }
    if (systemIndex >= m_galaxy.systems.size()) {
        return false;
    }

    double credits = 0.0;
    std::uint32_t cargoCount = 0;
    if (!reader.read(credits) || !reader.read(cargoCount) || cargoCount != m_playerCargo.size()) {
        return false; // commodity roster changed since the save
    }
    std::vector<float> cargo(cargoCount, 0.0f);
    for (float& units : cargo) {
        if (!reader.read(units)) {
            return false;
        }
    }
    // Fleet (v4).
    std::uint32_t fleetCount = 0;
    std::uint32_t activeIndex = 0;
    if (!reader.read(fleetCount) || !reader.read(activeIndex) || fleetCount == 0 ||
        activeIndex >= fleetCount) {
        return false;
    }
    std::vector<OwnedShip> fleet(fleetCount);
    for (OwnedShip& ship : fleet) {
        std::uint32_t fittingCount = 0;
        std::uint32_t crewCount = 0;
        if (!reader.readString(ship.defId) || !reader.read(fittingCount)) {
            return false;
        }
        ship.fittings.resize(fittingCount);
        for (ShipFitting& fitting : ship.fittings) {
            if (!reader.readString(fitting.mountId) || !reader.readString(fitting.defId) ||
                !reader.read(fitting.group)) {
                return false;
            }
        }
        if (!reader.read(crewCount)) {
            return false;
        }
        ship.crewIds.resize(crewCount);
        for (std::string& id : ship.crewIds) {
            if (!reader.readString(id)) {
                return false;
            }
        }
        if (!reader.read(ship.storedSystem) || !reader.read(ship.storedStation)) {
            return false;
        }
    }
    // Captains (v39).
    //
    // ⚑⚑⚑ THE ONE INVARIANT WORTH REFUSING A FILE OVER IS THAT TWO
    // CAPTAINS CANNOT HOLD ONE HULL, and it is the same shape as the two-players
    // -in-one-sky check v37 added. `Captain::ship` is a fleet index; a file that
    // names the same one twice, or names one past the end, describes a fleet
    // this code cannot represent - and the failure it would otherwise produce is
    // silent, because both captains would simply appear to fly the same ship.
    std::uint32_t captainCount = 0;
    if (!reader.read(captainCount) || captainCount > fleetCount) {
        return false; // more captains than hulls: nobody could be holding them all
    }
    std::vector<Captain> captains(captainCount);
    std::vector<std::uint8_t> held(fleetCount, 0u);
    std::uint8_t escorts = 0u;
    for (Captain& captain : captains) {
        if (!reader.readString(captain.name) || !reader.readString(captain.trade) ||
            !reader.read(captain.who) || !reader.read(captain.ship) || !reader.read(captain.cut)) {
            return false;
        }
        std::uint8_t kind = 0;
        std::uint8_t phase = 0;
        std::uint8_t stopping = 0;
        if (!reader.read(kind) || !reader.read(captain.order.marketA) ||
            !reader.read(captain.order.marketB) || !reader.read(stopping) ||
            !reader.read(captain.order.floor) || !reader.read(phase) ||
            !reader.read(captain.haul.leg.origin) || !reader.read(captain.haul.leg.market) ||
            !reader.read(captain.haul.leg.travelRemaining) || !reader.read(captain.haul.leg.legTotal) ||
            !reader.read(captain.haul.leg.commodity) || !reader.read(captain.haul.leg.cargo) ||
            !reader.read(captain.haul.outlay)) {
            return false;
        }
        std::uint8_t minePhase = 0;
        if (!reader.read(minePhase) || !reader.read(captain.mine.field) ||
            !reader.read(captain.mine.rockStep) || !reader.read(captain.mine.rockSeconds) ||
            !reader.read(captain.mine.commodity) || !reader.read(captain.mine.units) ||
            !reader.read(captain.mine.stalledSeconds) || !reader.read(captain.ledger.earned) ||
            !reader.read(captain.ledger.paid) || !reader.read(captain.ledger.losses)) {
            return false;
        }
        // ⚑⚑⚑⚑ THE BOUND IS THE LAST ORDER KIND AND NOT THE LAST ONE THAT
        // EXISTED WHEN THIS LINE WAS WRITTEN (stage E, fixing stage D). This
        // read `OrderKind::Mine` while the writer twelve lines up was already
        // emitting `Patrol` (3) and `Escort` (4) - and the comment beside that
        // writer says v42 exists PRECISELY because those two values now ship.
        // So the game wrote a save with a posted captain and then refused to
        // open it: the third time this phase that the two halves of one format
        // disagreed, and the first that a full green gate could not see,
        // because every save round-trip in the suite predates the combat
        // orders. A bound named after a member is a bound that has to be
        // revisited every time the enum grows; naming the LAST one keeps it
        // honest, and the `static_assert` below is what makes a sixth kind fail
        // here rather than in a player's save file.
        static_assert(static_cast<std::uint8_t>(OrderKind::Escort) == 4,
                      "OrderKind gained a member: widen this bound, bump kSaveVersion, and add the "
                      "new kind to the save round-trip test - a reader that refuses a byte its own "
                      "writer emits is a save the game writes and then cannot open.");
        if (kind > static_cast<std::uint8_t>(OrderKind::Escort) ||
            phase > static_cast<std::uint8_t>(sim::TraderPhase::InTransit) ||
            minePhase > static_cast<std::uint8_t>(MinePhase::Selling)) {
            return false;
        }
        captain.order.kind = static_cast<OrderKind>(kind);
        captain.order.stopping = stopping != 0;
        captain.haul.leg.phase = static_cast<sim::TraderPhase>(phase);
        captain.mine.phase = static_cast<MinePhase>(minePhase);
        // THE SAME SHAPE `Economy::load` USES ON ITS OWN TRADERS, and for the
        // same reason: a clock read off disk that runs backwards or a hold that
        // is negative describes a haul this code cannot fly, and the failure it
        // would otherwise produce is a captain stuck in transit forever with
        // the player's money already spent.
        if (captain.haul.leg.legTotal < 0.0 || captain.haul.leg.travelRemaining < 0.0 ||
            captain.haul.leg.travelRemaining > captain.haul.leg.legTotal || captain.haul.leg.cargo < 0.0f ||
            captain.haul.outlay < 0.0 || captain.mine.units < 0.0f || captain.mine.rockSeconds < 0.0 ||
            captain.mine.stalledSeconds < 0.0) {
            return false;
        }
        // ⚑⚑ THE FLOOR HAS AN UPPER BOUND AS WELL AS A LOWER ONE, AND THE UPPER
        // ONE IS THE INTERESTING HALF (v43). Negative is nonsense - it would
        // mean insisting on a loss. But a floor read off disk as infinity, or
        // as some enormous multiple, is a captain who can never sell anything
        // ever again: the hold never clears, so no new load is ever bought, and
        // the hull flies its route empty for the rest of the save with the
        // player's money locked in it. `kMaxSellFloor` is what the UI can
        // actually offer, so a file outside it did not come from this game.
        if (!(captain.order.floor >= 0.0f) || captain.order.floor > kMaxSellFloor) {
            return false;
        }
        // ⚑⚑ A STATIONARY ORDER MUST NAME A MARKET, AND THAT IS NOT A TIDINESS
        // CHECK (stage C). `captainSystem` reads the system out of
        // `order.marketA` for a mining captain, and `bubbleHoldsPlayerAsset`
        // reads `captainSystem` - so a Mine order with no market on it is a
        // captain who is nowhere, whose bubble is therefore held nowhere, whose
        // hull is never spawned and never ticked. It is exactly the shape of
        // the two defects stage B's live drive found: a save the game writes
        // and then cannot make sense of, with nothing said about it.
        if (stationary(captain.order.kind) && captain.order.marketA == kNoIndex) {
            return false;
        }
        // ⚑⚑ AND AT MOST ONE ESCORT, WHICH IS THE SAME KIND OF CHECK FOR THE
        // SAME KIND OF REASON. `orderEscort` refuses a second one outright and
        // `space_world.hpp` states the rule where the two combat orders are
        // declared, so a file carrying two describes a fleet - and a fleet is
        // Phase 40, refused here by name exactly as stage D refused it at the
        // door it came in by. The loader's standard through this whole block is
        // that a state the game cannot PRODUCE is a state it will not ACCEPT.
        if (escorting(captain.order.kind)) {
            if (escorts != 0u) {
                return false;
            }
            escorts = 1u;
        }
        // A captain with no ship cannot be flying one, and an order pointing at
        // a hull that is not theirs is the two-truths defect stage A refused.
        if (captain.ship == kNoIndex) {
            if (captain.order.kind != OrderKind::None || captain.haul.leg.phase != sim::TraderPhase::Idle) {
                return false;
            }
            continue;
        }
        if (captain.ship >= fleetCount || captain.ship == activeIndex || held[captain.ship] != 0u) {
            return false;
        }
        held[captain.ship] = 1u;
    }
    // Who died (v45), read where the writer put it: after the roster and before
    // the economy. ⚑ The bound is the roster size a galaxy could ever offer -
    // `kCaptainsPerHall` at every station of every system - because a file
    // claiming more dead captains than the galaxy has SEATS did not come from
    // this game, and the allocation it would otherwise ask for is attacker
    // -chosen. Same standard as every other count in this loader.
    std::uint32_t lostCount = 0;
    if (!reader.read(lostCount)) {
        return false;
    }
    std::uint64_t seats = 0;
    for (const sim::SystemSpec& spec : m_galaxy.systems) {
        seats += static_cast<std::uint64_t>(spec.stations.size()) * kCaptainsPerHall;
    }
    if (static_cast<std::uint64_t>(lostCount) > seats) {
        return false;
    }
    std::vector<std::uint64_t> lost(lostCount);
    for (std::uint64_t& who : lost) {
        if (!reader.read(who)) {
            return false;
        }
    }
    // The economy layout is derived from galaxy+params; rebuild it against
    // the (possibly regenerated) galaxy, then restore its dynamic state.
    if (!m_economyParams.commodities.empty()) {
        if (galaxyChanged) {
            m_economy.initialize(m_galaxy, m_economyParams, seed);
        }
        if (!m_economy.load(reader)) {
            return false;
        }
    }
    // Faction layout re-derives from galaxy + defs (v5); dynamic state loads
    // over a fresh initialize, same rule as the economy.
    if (galaxyChanged) {
        initializeFactions(); // m_universeSeed already updated above
    }
    if (!m_factionSim.load(reader)) {
        return false;
    }
    // Missions (v6): same rule — layout re-derives, dynamic state loads.
    if (!m_missions.load(reader)) {
        return false;
    }
    // Survey (v7): knowledge, per-signal state, ledger, and the plotted route.
    if (galaxyChanged) {
        initializeSurvey();
    }
    if (!m_survey.load(reader)) {
        return false;
    }
    // Mining (v8): depletion, the wreck store, and outstanding refine jobs.
    // Fields and rocks themselves re-derive from the seed, as ever.
    if (galaxyChanged) {
        initializeMining();
    }
    if (!m_mining.load(reader)) {
        return false;
    }

    // The cast (v33). ⚑ NOT gated on `galaxyChanged`, unlike the sims above:
    // there is nothing to re-initialize, because the memory is keyed by a
    // person or a seat and not by an index into anything this load rebuilt.
    m_castMemory.clear();
    std::uint32_t castMemoryCount = 0;
    if (!reader.read(castMemoryCount)) {
        return false;
    }
    m_castMemory.reserve(castMemoryCount);
    for (std::uint32_t i = 0; i < castMemoryCount; ++i) {
        CastMemory memory;
        if (!reader.read(memory.who) || !reader.read(memory.visits) || !reader.read(memory.regard)) {
            return false;
        }
        m_castMemory.push_back(memory);
    }

    // The transponder (v34). Read into a local and applied at the bottom with
    // the other player fields, not assigned here: every `return false` between
    // this point and there would otherwise leave the live world half-loaded
    // with a switch from a save that was rejected.
    std::uint8_t transponderOn = 1;
    if (!reader.read(transponderOn)) {
        return false;
    }

    // The bubbles (v37). Read into locals and installed only once every one of
    // them has parsed, for the same reason the transponder byte above is read
    // into a local: a `return false` halfway through must not leave the live
    // world holding half of a save that was rejected.
    const ecs::Snapshot schema = makeSnapshotSchema();
    std::uint32_t bubbleCount = 0;
    if (!reader.read(bubbleCount) || bubbleCount == 0 || bubbleCount > kMaxBubbles) {
        return false;
    }
    std::vector<std::unique_ptr<SystemBubble>> fresh;
    std::size_t playerBubble = kMaxBubbles;
    for (std::uint32_t i = 0; i < bubbleCount; ++i) {
        auto bubble = std::make_unique<SystemBubble>();
        if (!reader.read(bubble->system) || !reader.read(bubble->holdSeconds) ||
            !schema.load(bubble->registry, reader)) {
            return false;
        }
        // A snapshot only carries the pools the save had in it, and it carries
        // no statics and no random stream at all, so a bubble read back off
        // disk needs the same furnishing a fresh one gets.
        furnishBubble(*bubble);
        const std::size_t players = bubble->registry.storage<PlayerShip>().size();
        if (players > 1) {
            return false; // two players in one sky is not a world this can load
        }
        if (players == 1) {
            if (playerBubble != kMaxBubbles) {
                return false; // and neither is one player in two skies at once
            }
            playerBubble = fresh.size();
        }
        fresh.push_back(std::move(bubble));
    }
    if (playerBubble == kMaxBubbles) {
        return false; // not a current-format save (or player identity lost)
    }
    // `playerRegistry()` is the front bubble, so the player's goes to the front.
    std::swap(fresh[0], fresh[playerBubble]);
    if (fresh[0]->system != systemIndex) {
        return false; // the header and the sky disagree about where the player is
    }
    m_bubbles = std::move(fresh);
    // ⚑⚑ THE PLAYER'S BUBBLE IS OFF THE CLOCK AND THE REST ARE HELD TO THE
    // POLICY (Phase 38 stage C). The first is a repair of the swap above
    // rather than a defensive line: whichever bubble held the player was
    // written with a zero, but a file naming a different one is a file that
    // would leave the player standing in a system on a countdown. The second
    // is `kMaxBubbles` versus `kMaxInstantiatedSystems` - the load's own guard
    // is the corruption limit, which is deliberately far looser than the
    // policy, so a save written before the cap changed cannot hand the
    // O(n^2) pass more systems than this build is willing to tick.
    m_bubbles.front()->holdSeconds = 0.0;
    enforceBubbleCap();
    // Def-spawned entities were replaced wholesale; their def association is
    // gone (visuals persist via the saved RenderShape).
    playerShips().clear();
    m_combatEffects.clear();
    m_thrusters.clear();
    m_playerCredits = credits;
    m_playerCargo = std::move(cargo);
    // ⚑ Straight to the member rather than through setTransponder: that one
    // speaks on the comms channel and revokes a clearance, and a LOAD is not a
    // player throwing a switch. Restoring state must never look like an event.
    m_transponderOn = transponderOn != 0;
    m_hardcore = hardcore != 0;
    m_worldSeconds = worldSeconds; // intel ages continue where they left off
    m_hardcoreDeathPending = false;
    m_fleet = std::move(fleet);
    m_activeShip = activeIndex;
    m_captains = std::move(captains);
    m_lostCaptains = std::move(lost);
    if (galaxyChanged) {
        // Recompute the new-game anchor (hardcore respawn) for this galaxy.
        m_startSystem = 0;
        for (std::uint32_t i = 0; i < m_galaxy.systems.size(); ++i) {
            if (m_galaxy.systems[i].region == sim::Region::Core && !m_galaxy.systems[i].stations.empty()) {
                m_startSystem = i;
                break;
            }
        }
    }
    // The ECS snapshot already carries the fitted tuning exactly; only the
    // non-ECS cargo capacity derives from the fit and must be recomputed.
    if (m_defs != nullptr && !m_fleet.empty()) {
        const assets::ShipDef resolved = resolvedShipDef(activeShip());
        m_playerCargoCapacity = resolved.cargoCapacity;
        m_scanRange = resolved.scanRange > 0.0f ? resolved.scanRange : 1.0f;
        m_scanSpeed = resolved.scanSpeed > 0.0f ? resolved.scanSpeed : 1.0f;
        m_collectorRange = resolved.collectorRange > 0.0f ? resolved.collectorRange : 1.0f;
        m_signature = std::max(resolved.signature, kMinSignature);
    }

    // The snapshot carries the system's statics; only the non-ECS side data
    // (celestials, targets, gates, spawn anchor) needs rebuilding.
    enterFrame(systemIndex);
    // ⚑ This path does NOT go through loadSystem, so transient state reset
    // there is not reset here (the rule Phase 8r wrote down after m_dockedBerth
    // survived a load). A jump is transient state: clearing it is what stops a
    // save written mid-lane from resuming inside a tunnel to nowhere.
    m_jump.clear();
    const sim::SystemSpec& spec = m_galaxy.systems[systemIndex];
    rebuildSystemSideData(spec);
    m_dockedStation = dockedStation < spec.stations.size() ? dockedStation : kNoIndex;
    m_lastDockSystem = lastDockSystem;
    m_lastDockStation = lastDockStation;
    // Which berth is not in the save (Phase 8r: the clearance is transient and
    // no format bump was needed for it), so a docked load parks on the pad above the
    // station. It has to be reset explicitly rather than left alone: the value
    // is live state from the run being replaced, and carrying it over would
    // park the loaded ship in a berth it was never assigned. The clearance and
    // the comms log go for the same reason.
    m_dockedBerth = kNoIndex;
    m_clearance = DockClearance{};
    // And a stop in flight, for exactly the reason the clearance above goes
    // (Phase 36 stage C): it names an entity index in the run being replaced.
    // Assigned rather than ended - there is no outcome to record for a stop
    // that belonged to a different game.
    m_inspection = InspectionHold{};
    m_holdRefusalTimer = 0.0;
    m_pendingDockRequest = kNoIndex;
    m_berthRefusalTimer = 0.0;
    m_comms.clear();
    // And who had been hailed (Phase 8s), for exactly the reason above: these
    // are pilots from the run being replaced. loadSave does NOT go through
    // loadSystem, so resetting it there is not resetting it here.
    m_hails.clear();
    // Same rule for an outpost's stopped draw (Phase 8x stage 6): it records a
    // ship shot in front of the player, and the player it belonged to has just
    // been replaced. Leaving it would import one run's kill into another's
    // economy — and it is not in the save precisely because it is transient.
    m_minerHold.assign(m_economy.markets().size(), 0.0);
    m_pendingHail = HailRequest{};
    m_answeringHail = HailMemory{};
    // And what the player had been told about the war over their head
    // (Phase 8u) - the same rule a third time. A loaded run has heard
    // nothing yet, so a live contest announces itself again.
    m_announcedContestSystem = kNoIndex;
    m_announcedContestAttacker = kNoIndex;
    m_contestResolutions.clear();
    m_pendingRespawnSystem = kNoIndex;
    // A scan in flight does not survive a load, and neither does a command:
    // the target list is rebuilt below, so an engaged one would wake up flying
    // at whatever now sits in slot 0. Phase 28 keeps the command mode OUT of
    // the save deliberately — it is per-session flight state like throttle and
    // pips, and loading into a ship already flying itself is a worse first
    // frame than loading into one that is not.
    clearCommand();
    m_scanActive = false;
    m_scanProgress = 0.0f;
    m_pulseCooldown = 0.0;
    m_surveyEvents.clear();
    // Board offers came back with the save; no re-roll on a docked load.
    m_dockEventPending = false;
    m_missionEvents.clear();
    m_playerSpawn = isDocked() ? dockPoint(m_dockedStation)
                    : !spec.stations.empty()
                        ? spec.stations[0].position + core::DVec3{0.0, 0.0, 800.0}
                        : spec.planets[spec.primaryPlanet].position + core::DVec3{0.0, 0.0, 2.0e5};
    return true;
}

} // namespace game
