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
constexpr std::uint32_t kSaveVersion = 25;

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

// What Lua calls a pilot's job. Shared by the pilot_think hook and Phase 8s's
// pilot_hail, in one place so the two can never disagree about what a "trader"
// is - a hook that branches on the string would break silently if they did.
[[nodiscard]] const char* pilotRoleName(PilotRole role)
{
    switch (role) {
    case PilotRole::Fighter:
        return "fighter";
    case PilotRole::Trader:
        return "trader";
    case PilotRole::Patrol:
        return "patrol";
    }
    return "fighter";
}

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
    // Placeholder star so rendering before generateUniverse stays sane.
    m_star = {.name = "(void)", .position = {}, .radius = 6.96e8};

    const ecs::Entity e = m_registry.create();
    m_registry.emplace<Transform>(e);
    m_registry.emplace<FlightBody>(e);
    // No model yet: the def database does not exist at construction, and
    // applyShipDef gives the player the hull its def names before the first
    // frame. An unset model draws nothing rather than the wrong thing.
    m_registry.emplace<RenderShape>(e, RenderShape{});
    m_registry.emplace<PlayerShip>(e);
    m_registry.emplace<ShipControl>(e);
    m_registry.emplace<ShipPower>(e);
    m_registry.emplace<ShipDefense>(e);
    m_registry.emplace<ShipArmament>(e);
    m_registry.emplace<ShipFittings>(e);
    m_registry.emplace<ShipMounts>(e);
}

bool SpaceWorld::generateUniverse(const assets::DefDatabase& defs)
{
    m_galaxyParams = sim::GalaxyParams{};
    m_galaxyParams.seed = m_universeSeed;
    // Majors claim territory; pirate defs are clan templates (Phase 8b).
    for (const assets::FactionDef& faction : defs.factions()) {
        (faction.kind == assets::FactionKind::Pirate ? m_galaxyParams.pirateTemplateCount
                                                     : m_galaxyParams.factionCount) += 1;
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
    {
        const std::vector<assets::StationDef>& stationDefs = defs.stations();
        std::uint32_t majorIndex = 0;
        bool anyBias = false;
        std::vector<std::vector<float>> bias;
        for (const assets::FactionDef& faction : defs.factions()) {
            if (faction.kind == assets::FactionKind::Pirate) {
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
    const auto majorIndexOf = [&defs](const std::string& id) {
        std::uint32_t majorIndex = 0;
        for (const assets::FactionDef& faction : defs.factions()) {
            if (faction.kind == assets::FactionKind::Pirate) {
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
    for (const assets::CommodityDef& commodity : defs.commodities()) {
        m_economyParams.commodities.push_back({.basePrice = commodity.basePrice});
        m_commodityIds.push_back(commodity.id);
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
        archetype.stockCapacity = station.stockCapacity;
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

void SpaceWorld::initializeFactions()
{
    m_factionTable.clear();
    if (m_defs == nullptr) {
        return;
    }
    // Majors in def order (their generator indices), then clans.
    std::vector<const assets::FactionDef*> pirateTemplates;
    for (const assets::FactionDef& def : m_defs->factions()) {
        if (def.kind == assets::FactionKind::Pirate) {
            pirateTemplates.push_back(&def);
            continue;
        }
        m_factionTable.push_back({.defId = def.id,
                                  .name = def.name,
                                  .color = {def.color[0], def.color[1], def.color[2]},
                                  .pirate = false,
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
                                  .pirate = true,
                                  .aggression = jitterWeight(base.aggression),
                                  .forgiveness = jitterWeight(base.forgiveness),
                                  .shipsPatrol = base.shipsPatrol,
                                  .shipsRaider = base.shipsRaider,
                                  .shipsTrader = base.shipsTrader,
                                  .buildsNo = {base.buildsNo[0], base.buildsNo[1], base.buildsNo[2]}});
    }

    // FactionSim params: authored relations resolve def ids to table
    // indices (clans inherit their template's rows); unspecified
    // major-pirate pairs open at the default enmity.
    const std::uint32_t count = static_cast<std::uint32_t>(m_factionTable.size());
    sim::FactionSimParams params;
    params.agents.reserve(count);
    for (const GameFaction& faction : m_factionTable) {
        params.agents.push_back(
            {.aggression = faction.aggression, .forgiveness = faction.forgiveness, .pirate = faction.pirate});
        params.initialStandings.push_back(faction.pirate ? kClanInitialStanding : 0.0f);
    }
    params.baselineRelations.assign(static_cast<std::size_t>(count) * count, 0.0f);
    const auto setPair = [&](std::uint32_t a, std::uint32_t b, float value) {
        params.baselineRelations[static_cast<std::size_t>(a) * count + b] = value;
        params.baselineRelations[static_cast<std::size_t>(b) * count + a] = value;
    };
    for (std::uint32_t a = 0; a < count; ++a) {
        for (std::uint32_t b = a + 1; b < count; ++b) {
            if (m_factionTable[a].pirate != m_factionTable[b].pirate) {
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
            if (!found && a < majorCount) { // clans: silence per-clan repeats
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

bool SpaceWorld::stationSells(const assets::CatalogGate& gate) const
{
    if (!isDocked()) {
        return false;
    }
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner >= m_factionTable.size()) {
        return true; // ownerless station: open market
    }
    const GameFaction& faction = m_factionTable[owner];
    if (!gate.factions.empty() &&
        std::find(gate.factions.begin(), gate.factions.end(), faction.defId) == gate.factions.end()) {
        return false;
    }
    // Pirate stations fence anything their defs allow, standing be damned
    // (docking already required non-hostile standing).
    return faction.pirate || m_factionSim.standing(owner) >= gate.minRep;
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
    Transform& transform = m_registry.storage<Transform>().get(playerEntityIndex());
    const core::DVec3 position = spec.stations[station].position + offset;
    transform.position = position;
    transform.previousPosition = position;
    m_registry.storage<FlightBody>().get(playerEntityIndex()) = FlightBody{};
    SOL_LOG_WARN("dev warp: player moved to '%s' offset (%.0f, %.0f, %.0f)",
                 spec.stations[station].name.c_str(),
                 offset.x,
                 offset.y,
                 offset.z);
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
    const ecs::Pool<TraderPuppet>& puppets = m_registry.storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        if (puppets.values()[i].traderIndex != traderIndex) {
            continue;
        }
        const Transform* transform = m_registry.storage<Transform>().tryGet(puppets.entityIndices()[i]);
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
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
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
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    if (total == 0) {
        return kNoIndex;
    }
    const std::size_t index = m_targetIndex % total;
    if (index == m_starTargetIndex) {
        return 0; // body 0 is the star, matching SurveySim's numbering
    }
    if (index >= m_planetTargetBase && index < m_planetTargetBase + m_planets.size()) {
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
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
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
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
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
        loot.cargo.push_back(
            {.commodity = rng.range(commodityCount), .units = static_cast<float>(5 + rng.range(21))});
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

void SpaceWorld::ensureMiningPools()
{
    (void)m_registry.storage<MineableRock>();
    (void)m_registry.storage<WreckMarker>();
    (void)m_registry.storage<OreChunk>();
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
    ensureMiningPools();
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

void SpaceWorld::instantiateMiningEntities()
{
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

    std::vector<sim::RockSpec> rocks;
    for (std::uint32_t field = 0; field < m_fields.size(); ++field) {
        m_mining.rocksFor(m_galaxy, m_currentSystem, field, rocks);
        for (std::uint32_t index = 0; index < rocks.size(); ++index) {
            const sim::RockSpec& rock = rocks[index];
            if (m_mining.unitsLeft(m_currentSystem, field, index, rock.yieldUnits) <= 0.0f) {
                continue; // cut to nothing on an earlier visit; it broke up
            }
            const ecs::Entity entity = m_registry.create();
            m_registry.emplace<Transform>(
                entity, Transform{.position = rock.position, .previousPosition = rock.position});
            const float scale = static_cast<float>(rock.radius);
            m_registry.emplace<RenderShape>(entity,
                                            RenderShape{.scale = {scale, scale, scale},
                                                        .model = rock.commodity < rockModels.size()
                                                                     ? rockModels[rock.commodity]
                                                                     : roleModel(kRoleRock)});
            m_registry.emplace<MineableRock>(entity,
                                             MineableRock{.field = field,
                                                          .index = index,
                                                          .commodity = rock.commodity,
                                                          .totalUnits = rock.yieldUnits,
                                                          .tumbleAxis = rock.tumbleAxis,
                                                          .tumbleRate = rock.tumbleRate});
        }
    }
}

void SpaceWorld::spawnCutChunk(const core::DVec3& origin,
                               double surface,
                               std::uint32_t commodity,
                               float units)
{
    // Ore breaks off *toward the beam* — with a spread, but not at random.
    // Scattering it evenly means most of what you cut simply leaves, and the
    // loop becomes chasing debris rather than mining.
    const core::DVec3 toShip = shipState().position - origin;
    const double distance = length(toShip);
    core::DVec3 direction =
        distance > 1.0 ? toShip * (1.0 / distance) : sim::randomPlayfieldDirection(m_chunkRng);
    direction = direction + sim::randomPlayfieldDirection(m_chunkRng) * kChunkSpread;
    const double spread = length(direction);
    direction = spread > 1.0e-6 ? direction * (1.0 / spread) : core::DVec3{0.0, 0.0, 1.0};
    spawnOreChunk(origin + direction * surface,
                  direction * (kChunkDriftSpeed * (0.6 + 0.8 * m_chunkRng.nextDouble01())),
                  commodity,
                  units);
}

void SpaceWorld::spawnOreChunk(const core::DVec3& position,
                               const core::DVec3& velocity,
                               std::uint32_t commodity,
                               float units)
{
    const ecs::Entity entity = m_registry.create();
    m_registry.emplace<Transform>(entity, Transform{.position = position, .previousPosition = position});
    m_registry.emplace<RenderShape>(entity,
                                    RenderShape{.scale = {6.0f, 6.0f, 6.0f},
                                                .model = commodity < m_chunkModels.size()
                                                             ? m_chunkModels[commodity]
                                                             : roleModel(kRoleOreChunk)});
    m_registry.emplace<OreChunk>(
        entity,
        OreChunk{
            .velocity = velocity, .lifetime = kChunkLifetimeSeconds, .commodity = commodity, .units = units});
}

float SpaceWorld::cutRock(std::uint32_t entityIndex, float units)
{
    MineableRock* rock = m_registry.storage<MineableRock>().tryGet(entityIndex);
    if (rock == nullptr) {
        return 0.0f;
    }
    const float taken = m_mining.mineRock(m_currentSystem, rock->field, rock->index, rock->totalUnits, units);
    if (taken <= 0.0f) {
        return 0.0f;
    }
    // What comes off drifts: the beam breaks the rock, the ship still has to
    // go and get it. Chunks are capped so a fat bite arrives as several.
    const core::DVec3 origin = m_registry.storage<Transform>().get(entityIndex).position;
    const double surface = static_cast<double>(m_registry.storage<RenderShape>().get(entityIndex).scale.x);
    float remaining = taken;
    while (remaining > 0.0f) {
        const float chunk = std::min(remaining, kChunkUnitCeiling);
        remaining -= chunk;
        spawnCutChunk(origin, surface, rock->commodity, chunk);
    }
    return taken;
}

float SpaceWorld::cutWreck(std::uint32_t entityIndex, float units)
{
    const WreckMarker* marker = m_registry.storage<WreckMarker>().tryGet(entityIndex);
    if (marker == nullptr) {
        return 0.0f;
    }
    std::uint32_t commodity = 0;
    const float taken = m_mining.cutWreckCargo(marker->id, units, &commodity);
    const core::DVec3 origin = m_registry.storage<Transform>().get(entityIndex).position;
    if (taken > 0.0f) {
        float remaining = taken;
        while (remaining > 0.0f) {
            const float chunk = std::min(remaining, kChunkUnitCeiling);
            remaining -= chunk;
            spawnCutChunk(origin, 25.0, commodity, chunk);
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

    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    const ecs::Pool<MineableRock>& rocks = m_registry.storage<MineableRock>();
    const ecs::Pool<WreckMarker>& wrecks = m_registry.storage<WreckMarker>();
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
    info.distance = length(m_registry.storage<Transform>().get(entityIndex).position - shipState().position);
    // ⚑ The mining beams' reach, never the longest gun's (Phase 31 stage C1).
    // A ship carrying a 3 km cannon beside an 800 m laser can cut at 800 m,
    // and the readout used to be able to read the one gun there was.
    const ArmamentSummary armament = playerArmament();
    info.inRange = armament.canMine && info.distance <= static_cast<double>(armament.miningRange);
    if (isWreck) {
        const WreckMarker& marker = m_registry.storage<WreckMarker>().get(entityIndex);
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
    const MineableRock& rock = m_registry.storage<MineableRock>().get(entityIndex);
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
        (void)cutWreck(entityIndex, 1.0e6f);
        return true;
    }
    const MineableRock rock = m_registry.storage<MineableRock>().get(entityIndex);
    if (cutRock(entityIndex, 1.0e6f) <= 0.0f) {
        return false;
    }
    m_registry.destroy(m_registry.entityFromIndex(entityIndex));
    m_rockEvents.push_back({.commodity = rock.commodity, .units = rock.totalUnits});
    return true;
}

bool SpaceWorld::warpToNearestRock()
{
    if (isDocked()) {
        return false;
    }
    const core::DVec3 position = shipState().position;
    const ecs::Pool<MineableRock>& rocks = m_registry.storage<MineableRock>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
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
    Transform& transform = m_registry.storage<Transform>().get(playerEntityIndex());
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
    m_registry.storage<FlightBody>().get(playerEntityIndex()) = FlightBody{};
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

sim::SignalLoot SpaceWorld::defaultWreckLoot(const assets::ShipDef* def, std::uint64_t seed) const
{
    sim::SignalLoot loot;
    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());
    if (commodityCount == 0) {
        return loot;
    }
    core::Rng rng(seed, 11);
    // Scrap: the hull itself, as ore, scaled by how big the ship was.
    const float hullScrap = def != nullptr ? std::max(4.0f, def->mass * 0.0016f) : 8.0f;
    const std::uint32_t ore = commodityIndex("sol.ore");
    loot.cargo.push_back({.commodity = ore < commodityCount ? ore : 0,
                          .units = hullScrap * (0.7f + 0.6f * rng.nextFloat01())});
    // Whatever it was hauling, sometimes.
    if (rng.nextFloat01() < 0.5f) {
        loot.cargo.push_back(
            {.commodity = rng.range(commodityCount), .units = static_cast<float>(3 + rng.range(15))});
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
        return refuse("no ore aboard to refine", outError);
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

void SpaceWorld::tickMining(double dt)
{
    // Coarse layer first: wrecks age and refinery orders cook whether the
    // player is watching them or three systems away (decisions/005).
    m_mining.tick(dt);

    // Rocks tumble. It is the cheapest thing that makes a field read as a
    // place rather than a diagram.
    ecs::Pool<MineableRock>& rocks = m_registry.storage<MineableRock>();
    ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const MineableRock& rock = rocks.values()[i];
        Transform& transform = transforms.get(rocks.entityIndices()[i]);
        transform.previousOrientation = transform.orientation;
        transform.orientation =
            normalize(transform.orientation *
                      core::fromAxisAngle(rock.tumbleAxis, rock.tumbleRate * static_cast<float>(dt)));
    }

    // Chunks drift, are gathered, or are lost.
    const core::DVec3 shipPosition = shipState().position;
    const double collectRange = static_cast<double>(m_collectorRange);
    float space = m_playerCargoCapacity - playerCargoTotal();
    ecs::Pool<OreChunk>& chunks = m_registry.storage<OreChunk>();
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
        if (isDocked() || length(transform.position - shipPosition) > collectRange) {
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
        m_registry.destroy(m_registry.entityFromIndex(entityIndex));
    }

    // Reconcile wreck entities with the sim: newly killed ships get a hull to
    // cut, decayed and emptied ones stop being there. Done here rather than
    // in handleShipDestroyed so no pool changes shape mid-iteration.
    bool wrecksChanged = false;
    ecs::Pool<WreckMarker>& markers = m_registry.storage<WreckMarker>();
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
        m_registry.destroy(m_registry.entityFromIndex(entityIndex));
        wrecksChanged = true;
    }
    std::vector<std::uint32_t> here;
    m_mining.wrecksIn(m_currentSystem, here);
    for (const std::uint32_t id : here) {
        if (std::find(present.begin(), present.end(), id) != present.end()) {
            continue;
        }
        const sim::WreckRecord* wreck = m_mining.wreck(id);
        const ecs::Entity entity = m_registry.create();
        m_registry.emplace<Transform>(
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
        m_registry.emplace<RenderShape>(
            entity, RenderShape{.scale = {wreckScale, wreckScale, wreckScale}, .model = wreckModel});
        m_registry.emplace<WreckMarker>(entity, WreckMarker{.id = id});
        wrecksChanged = true;
    }
    if (wrecksChanged) {
        rebuildDynamicTargets();
    }

    // The collection ticker is a HUD readout, not state: it fades.
    if (m_collectTicker > 0.0f) {
        m_collectTickerAge += dt;
        if (m_collectTickerAge > 2.0) {
            m_collectTicker = 0.0f;
            m_collectTickerAge = 0.0;
        }
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

void SpaceWorld::spawnWing(std::uint32_t faction,
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
    const PilotRole role = pilotRoleFor(cell);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t pick =
            chooseWingHull(std::span<const std::uint32_t>(m_rosterClasses), baselineSecurity, i, count);
        const assets::ShipDef* def = m_defs->findShip(roster[pick].c_str());
        if (def == nullptr) {
            SOL_LOG_WARN("wing: no ship def '%s'", roster[pick].c_str());
            return;
        }
        const core::DVec3 position =
            anchor + core::DVec3{spread * (1.0 + i), 300.0 + 250.0 * i, -spread * 0.5 * i};
        const ecs::Entity entity = spawnShipAt(*def, *m_defs, position, m_factionTable[faction].name.c_str());
        ShipPilot pilot{.role = role, .factionIndex = faction};
        pilot.state = state;
        if (waypoint != nullptr) {
            pilot.waypoint = *waypoint;
            pilot.respondTimer = kResponseGiveUpSeconds;
        }
        m_registry.emplace<ShipPilot>(entity, pilot);
    }
}

void SpaceWorld::spawnAmbientPilots(std::uint32_t systemIndex, const sim::SystemSpec& spec)
{
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
    // still on `faction.pirate` rather than on the sign, because the roster is
    // what actually differs; the sign only decides how many.
    const float baselineSecurity = systemSecurityBaseline(systemIndex);
    if (owner < m_factionTable.size()) {
        const GameFaction& faction = m_factionTable[owner];
        if (faction.pirate) {
            // ⚑ `RosterCell::Count` as the fallback is this site saying it has
            // never had one, which is what reading `faction.shipsRaider` bare
            // used to say by omission.
            spawnWing(owner,
                      assets::RosterCell::Raider,
                      factionRoster(faction, assets::RosterCell::Raider, assets::RosterCell::Count),
                      baselineSecurity,
                      raidersFor(baselineSecurity),
                      anchor,
                      900.0);
        } else {
            spawnWing(owner,
                      assets::RosterCell::Patrol,
                      factionRoster(faction, assets::RosterCell::Patrol, assets::RosterCell::Count),
                      baselineSecurity,
                      patrolsFor(baselineSecurity),
                      anchor,
                      700.0);

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
            spawnWing(owner,
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
        spawnWing(contest.attacker,
                  assets::RosterCell::Raider,
                  roster,
                  baselineSecurity,
                  count,
                  anchor + core::DVec3{9'000.0, 1'500.0, 6'000.0},
                  1'200.0);
        if (owner < m_factionTable.size()) {
            spawnWing(
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
void SpaceWorld::despawnShip(std::uint32_t entityIndex)
{
    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        if (m_spawnedShips[i].entity.index == entityIndex) {
            m_spawnedShips.erase(m_spawnedShips.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    m_registry.destroy(m_registry.entityFromIndex(entityIndex));
}

bool SpaceWorld::traderLegSegment(std::uint32_t traderIndex, TraderLegPlacement& out) const
{
    core::DVec3& from = out.from;
    core::DVec3& to = out.to;
    float& progress = out.progress;
    const sim::TraderRoute route = m_economy.route(traderIndex);
    if (route.system != m_currentSystem || route.leg == sim::TraderLeg::None ||
        route.leg == sim::TraderLeg::Jump) {
        return false;
    }
    const auto stationOf = [&](std::uint32_t market) {
        const sim::StationMarket& row = m_economy.markets()[market];
        return m_galaxy.systems[row.systemIndex].stations[row.stationIndex].position;
    };
    const core::DVec3 origin = stationOf(route.fromMarket);
    const core::DVec3 destination = stationOf(route.toMarket);
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const auto hops = [&](std::uint32_t a, std::uint32_t b) {
        return static_cast<std::uint32_t>(m_economy.hopCount(a, b));
    };

    // Its slot in the lane, applied to BOTH ends: offsetting only the spawn
    // would let the whole convoy converge again on the way in and pile up at
    // the destination, which is the same stack one leg later.
    const auto inSlot = [&](const core::DVec3& start, const core::DVec3& end) {
        const core::DVec3 offset = sim::laneSlotOffset(traderIndex, end - start, kTraderLaneSpacing);
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
        sim::gateTowardSystem(std::span<const sim::GateSpec>(spec.gates), m_currentSystem, otherSystem, hops);
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

void SpaceWorld::syncTraderPuppets()
{
    if (m_defs == nullptr || m_factionTable.empty() || m_economy.markets().empty()) {
        return;
    }
    const std::size_t fleet = m_economy.traders().size();
    m_puppetPresent.assign(fleet, 0);
    // Re-asserted from scratch every tick, like m_puppetPresent: a hold on a
    // trader's clock is a fact about what is happening to its body right now,
    // and a body that is gone can hold nothing.
    m_economy.clearDetained();

    // Existing bodies first: anything whose record has moved on stops being
    // here, and anything whose leg has changed under it is rebuilt rather than
    // left flying at a destination nobody is going to.
    std::vector<ecs::Entity> doomed;
    ecs::Pool<TraderPuppet>& puppets = m_registry.storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        TraderPuppet& puppet = puppets.values()[i];
        const ecs::Entity entity = m_registry.entityFromIndex(puppets.entityIndices()[i]);
        TraderLegPlacement leg;
        if (puppet.traderIndex >= fleet || !traderLegSegment(puppet.traderIndex, leg) ||
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
        ShipPilot* pilot = m_registry.tryGet<ShipPilot>(entity);
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
        puppet.paced = keepTraderOnSchedule(entity, leg) ? 1u : 0u;
    }
    for (const ecs::Entity entity : doomed) {
        despawnShip(entity.index);
    }

    // Then the fleet: every trader flying a leg here that has no body yet gets
    // one, placed where the record says it already is rather than at the start
    // of its leg — the player arriving mid-haul must find traffic in progress.
    for (std::uint32_t t = 0; t < fleet; ++t) {
        if (m_puppetPresent[t] != 0) {
            continue;
        }
        TraderLegPlacement leg;
        if (!traderLegSegment(t, leg)) {
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
        const ecs::Entity entity = spawnShipAt(*def, *m_defs, position, owner.name.c_str());
        m_registry.emplace<ShipPilot>(entity,
                                      ShipPilot{.role = PilotRole::Trader,
                                                .state = PilotState::Travel,
                                                .waypoint = leg.to,
                                                .factionIndex = faction});
        m_registry.emplace<TraderPuppet>(entity, TraderPuppet{.traderIndex = t, .destination = leg.to});
        // Pointed down its lane on the frame it appears. steerTravel would
        // turn it anyway, but a system full of haulers facing nowhere is what
        // the player would see in the first second after a jump.
        Transform& transform = m_registry.storage<Transform>().get(entity.index);
        transform.orientation = lookAlong(leg.to - position);
        transform.previousOrientation = transform.orientation;
        // Appearing mid-leg is the normal case, so a new body has to answer
        // the same question the reconcile above answers: is the record flying
        // this one? Skipping it would leave a hauler briefly advertised as
        // huntable while being uncatchable, and hand it its lane speed a tick
        // late into the bargain.
        m_registry.storage<TraderPuppet>().get(entity.index).paced =
            keepTraderOnSchedule(entity, leg) ? 1u : 0u;
        m_puppetPresent[t] = 1;
    }
}

bool SpaceWorld::chooseMinerRock(MinerPuppet& miner, const core::DVec3& from, bool sameField) const
{
    // 8f's rocks are real entities with real depletion, so the miner works one
    // of those rather than a point in a field: a rock that has been cut to
    // nothing is not spawned, which means a miner can never be found working
    // ground that is already gone.
    const ecs::Pool<MineableRock>& rocks = m_registry.storage<MineableRock>();
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
        const Transform* transform = m_registry.storage<Transform>().tryGet(index);
        const RenderShape* shape = m_registry.storage<RenderShape>().tryGet(index);
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

bool SpaceWorld::minerWorkPoint(const MinerPuppet& miner, core::DVec3& rock, core::DVec3& hold) const
{
    const Transform* transform = m_registry.storage<Transform>().tryGet(miner.rock);
    if (transform == nullptr || m_registry.storage<MineableRock>().tryGet(miner.rock) == nullptr) {
        return false; // cut to nothing, or the system changed under it
    }
    rock = transform->position;
    const RenderShape* shape = m_registry.storage<RenderShape>().tryGet(miner.rock);
    const double radius = shape != nullptr ? static_cast<double>(shape->scale.x) : 0.0;
    // On the station's side of the rock, so a ship coming out from the dock
    // meets the miner rather than the rock it is hiding behind.
    const sim::StationMarket& market = m_economy.markets()[miner.market];
    const core::DVec3 station = m_galaxy.systems[market.systemIndex].stations[market.stationIndex].position;
    hold = sim::minerHoldPoint(rock, radius, station - rock, kMinerRockClearance);
    return true;
}

void SpaceWorld::syncMinerPuppets(double dt)
{
    if (m_defs == nullptr || m_factionTable.empty() || m_economy.markets().empty()) {
        return;
    }
    const std::size_t marketCount = m_economy.markets().size();
    if (m_minerHold.size() != marketCount) {
        m_minerHold.assign(marketCount, 0.0);
    }
    for (double& hold : m_minerHold) {
        hold = std::max(0.0, hold - dt);
    }
    m_minerPresent.assign(marketCount, 0);

    // Which outposts here are actually digging. Not "which are extractors":
    // satisfaction is the station's own answer to how much of its nominal
    // output it managed, so a mine whose warehouse is full, whose rock has run
    // out, or whose miner the player just shot reads zero and gets no body.
    // That is the promotion working in the honest direction — the sky follows
    // the books, and a still field means the books have stopped.
    const auto digs = [&](std::uint32_t market) {
        const sim::StationMarket& row = m_economy.markets()[market];
        if (row.systemIndex != m_currentSystem || row.archetype >= m_economyParams.archetypes.size()) {
            return false;
        }
        return m_economyParams.archetypes[row.archetype].extracts && m_economy.satisfaction(market) > 0.0f &&
               m_minerHold[market] <= 0.0;
    };

    std::vector<ecs::Entity> doomed;
    ecs::Pool<MinerPuppet>& miners = m_registry.storage<MinerPuppet>();
    for (std::size_t i = 0; i < miners.size(); ++i) {
        MinerPuppet& miner = miners.values()[i];
        const ecs::Entity entity = m_registry.entityFromIndex(miners.entityIndices()[i]);
        if (miner.market >= marketCount || !digs(miner.market) || m_minerPresent[miner.market] != 0) {
            doomed.push_back(entity);
            continue;
        }
        m_minerPresent[miner.market] = 1;
        ShipPilot* pilot = m_registry.tryGet<ShipPilot>(entity);
        Transform* transform = m_registry.tryGet<Transform>(entity);
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
        if (miner.rockSeconds <= 0.0f || !minerWorkPoint(miner, rock, hold)) {
            if (!chooseMinerRock(miner, transform->position, true)) {
                doomed.push_back(entity); // the field is worked out under it
                continue;
            }
            miner.rockSeconds = static_cast<float>(kMinerRockSeconds);
            if (!minerWorkPoint(miner, rock, hold)) {
                doomed.push_back(entity);
                continue;
            }
        }
        pilot->waypoint = hold;
    }
    for (const ecs::Entity entity : doomed) {
        despawnShip(entity.index);
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
        if (!chooseMinerRock(miner, station, false)) {
            continue; // no rock of that kind here; the draw is failing anyway
        }
        core::DVec3 rock;
        core::DVec3 hold;
        if (!minerWorkPoint(miner, rock, hold)) {
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
        const ecs::Entity entity = spawnShipAt(*def, *m_defs, hold, owner.name.c_str());
        m_registry.emplace<ShipPilot>(entity,
                                      ShipPilot{.role = PilotRole::Trader,
                                                .state = PilotState::Travel,
                                                .waypoint = hold,
                                                .factionIndex = faction});
        m_registry.emplace<MinerPuppet>(entity, miner);
        Transform& transform = m_registry.storage<Transform>().get(entity.index);
        transform.orientation = lookAlong(rock - hold); // nose on the rock it is cutting
        transform.previousOrientation = transform.orientation;
        m_minerPresent[market] = 1;
    }
}

void SpaceWorld::minerPuppetInfo(std::vector<MinerPuppetInfo>& out)
{
    out.clear();
    const core::DVec3 eye = m_registry.storage<Transform>().get(playerEntityIndex()).position;
    ecs::Pool<MinerPuppet>& miners = m_registry.storage<MinerPuppet>();
    for (std::size_t i = 0; i < miners.size(); ++i) {
        const std::uint32_t entityIndex = miners.entityIndices()[i];
        const Transform* transform = m_registry.storage<Transform>().tryGet(entityIndex);
        if (transform == nullptr) {
            continue;
        }
        const MinerPuppet& miner = miners.values()[i];
        MinerPuppetInfo info;
        info.market = miner.market;
        info.distance = length(transform->position - eye);
        const FlightBody* body = m_registry.storage<FlightBody>().tryGet(entityIndex);
        info.speed = body != nullptr ? length(body->velocity) : 0.0;
        if (const Transform* rock = m_registry.storage<Transform>().tryGet(miner.rock)) {
            info.working = true;
            info.rockDistance = length(rock->position - transform->position);
        }
        if (miner.market < m_economy.markets().size()) {
            const sim::StationMarket& row = m_economy.markets()[miner.market];
            info.station = m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name;
        }
        for (const SpawnedShip& spawned : m_spawnedShips) {
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
bool SpaceWorld::keepTraderOnSchedule(ecs::Entity entity, const TraderLegPlacement& leg)
{
    ShipPilot* pilot = m_registry.tryGet<ShipPilot>(entity);
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
    Transform* transform = m_registry.tryGet<Transform>(entity);
    FlightBody* body = m_registry.tryGet<FlightBody>(entity);
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
        const ShipControl* control = m_registry.tryGet<ShipControl>(entity);
        const double envelope =
            control != nullptr ? static_cast<double>(control->tuning.maxSpeed) * 2.0 : 200.0;
        body->velocity = lane * (envelope / distance);
    }
    return true;
}

void SpaceWorld::traderPuppetInfo(std::vector<TraderPuppetInfo>& out)
{
    static constexpr const char* kStateNames[] = {"idle", "patrol", "attack", "flee", "travel"};
    out.clear();
    const core::DVec3 eye = m_registry.storage<Transform>().get(playerEntityIndex()).position;
    ecs::Pool<TraderPuppet>& puppets = m_registry.storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        const std::uint32_t entityIndex = puppets.entityIndices()[i];
        const Transform* transform = m_registry.storage<Transform>().tryGet(entityIndex);
        if (transform == nullptr) {
            continue;
        }
        TraderPuppetInfo info;
        info.traderIndex = puppets.values()[i].traderIndex;
        info.distance = length(transform->position - eye);
        const FlightBody* body = m_registry.storage<FlightBody>().tryGet(entityIndex);
        info.speed = body != nullptr ? length(body->velocity) : 0.0;
        const ShipPilot* pilot = m_registry.storage<ShipPilot>().tryGet(entityIndex);
        info.state = pilot != nullptr
                         ? kStateNames[static_cast<std::uint32_t>(pilot->state) % std::size(kStateNames)]
                         : "none";
        for (const SpawnedShip& spawned : m_spawnedShips) {
            if (spawned.entity.index == entityIndex) {
                info.name = spawned.name;
                break;
            }
        }
        out.push_back(std::move(info));
    }
}

void SpaceWorld::responderInfo(std::vector<ResponderInfo>& out) const
{
    out.clear();
    const ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const ShipPilot& pilot = pilots.values()[i];
        if (pilot.respondTimer <= 0.0f) {
            continue;
        }
        const std::uint32_t index = pilots.entityIndices()[i];
        const Transform* transform = m_registry.storage<Transform>().tryGet(index);
        if (transform == nullptr) {
            continue;
        }
        ResponderInfo info;
        info.name = m_registry.storage<ShipPilot>().tryGet(index) != nullptr &&
                            pilot.factionIndex < m_factionTable.size()
                        ? m_factionTable[pilot.factionIndex].name
                        : std::string("unaffiliated");
        info.distanceToIncident = length(pilot.waypoint - transform->position);
        info.secondsLeft = static_cast<double>(pilot.respondTimer);
        info.state = pilot.state;
        info.position = transform->position;
        info.pirate = pilot.factionIndex < m_factionTable.size() && m_factionTable[pilot.factionIndex].pirate;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const ResponderInfo& a, const ResponderInfo& b) {
        return a.distanceToIncident < b.distanceToIncident;
    });
}

PilotState SpaceWorld::pilotStateOf(ecs::Entity entity) const
{
    const ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
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
    static constexpr const char* kStateNames[] = {"idle", "patrol", "attack", "flee", "travel"};
    out.clear();
    const ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
    const ecs::Pool<TraderPuppet>& puppets = m_registry.storage<TraderPuppet>();
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
        const Transform* from = m_registry.storage<Transform>().tryGet(entityIndex);
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
            pilot.hasTarget != 0 ? m_registry.storage<Transform>().tryGet(pilot.targetIndex) : nullptr;
        info.distance = to != nullptr ? length(to->position - from->position) : 0.0;
        for (const SpawnedShip& spawned : m_spawnedShips) {
            if (spawned.entity.index == entityIndex) {
                info.name = spawned.name;
            } else if (pilot.hasTarget != 0 && spawned.entity.index == pilot.targetIndex) {
                info.prey = spawned.name;
            }
        }
        if (info.prey.empty() && pilot.hasTarget != 0 && pilot.targetIndex == playerEntityIndex()) {
            info.prey = "the player";
        }
        out.push_back(std::move(info));
    }
}

void SpaceWorld::despawnSystem()
{
    const std::uint32_t playerIndex = playerEntityIndex();
    std::vector<ecs::Entity> doomed;
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        const std::uint32_t entityIndex = transforms.entityIndices()[i];
        if (entityIndex != playerIndex) {
            doomed.push_back(m_registry.entityFromIndex(entityIndex));
        }
    }
    for (const ecs::Entity entity : doomed) {
        m_registry.destroy(entity);
    }
    m_spawnedShips.clear();
    m_combatEffects.clear();
    m_thrusters.clear();
    if (m_audio != nullptr) {
        // Every voice in flight was positioned in the system being torn down,
        // and a one-shot does not track its emitter - so leaving them running
        // would play the old system's explosions in the new one's coordinates.
        m_audio->stopAll();
    }
}

void SpaceWorld::instantiateSystemEntities(const sim::SystemSpec& spec)
{
    auto addStatic = [&](core::DVec3 position,
                         core::Vec3 scale,
                         ModelId model,
                         core::Quat orientation = core::Quat::identity()) {
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(e,
                                      Transform{.position = position,
                                                .previousPosition = position,
                                                .orientation = orientation,
                                                .previousOrientation = orientation});
        m_registry.emplace<RenderShape>(e, RenderShape{.scale = scale, .model = model});
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
    m_star = {.name = spec.name, .position = {}, .radius = spec.starRadius};
    m_planets.clear();
    for (const sim::PlanetSpec& planet : spec.planets) {
        m_planets.push_back({.name = planet.name, .position = planet.position, .radius = planet.radius});
    }
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
    for (const CelestialBody& planet : m_planets) {
        m_targets.push_back(
            {.name = planet.name, .position = planet.position, .surfaceRadius = planet.radius});
    }
    m_targets.push_back({.name = m_star.name, .position = m_star.position, .surfaceRadius = m_star.radius});
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
    const RenderShape* shape = m_registry.tryGet<RenderShape>(m_registry.entityFromIndex(entityIndex));
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

void SpaceWorld::rebuildAvoidance()
{
    // ⚑ Built from the same pools and the same exclusions as the collision
    // bodies below, so "what I avoid" and "what I can hit" are one statement
    // (Phase 8y). Statics first, movers after, and m_avoidStatics is the seam:
    // a ship that must not dodge other ships takes the front of the list.
    m_avoidance.clear();
    const ecs::Pool<FlightBody>& bodies = m_registry.storage<FlightBody>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    const ecs::Pool<Projectile>& projectiles = m_registry.storage<Projectile>();
    const ecs::Pool<OreChunk>& oreChunks = m_registry.storage<OreChunk>();
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
    m_avoidance.push_back({.position = m_star.position, .radius = m_star.radius});
    for (const CelestialBody& planet : m_planets) {
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

void SpaceWorld::loadSystem(std::uint32_t systemIndex, std::uint32_t fromSystem)
{
    despawnSystem();
    m_currentSystem = systemIndex;
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
    instantiateSystemEntities(spec);
    rebuildSystemSideData(spec);
    // Rocks and wrecks (Phase 8f): rebuildSystemSideData has just refreshed
    // m_fields, and depletion decides which rocks are still there to spawn.
    instantiateMiningEntities();

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
    Transform& transform = m_registry.storage<Transform>().get(playerIndex);
    transform.position = arrival;
    transform.previousPosition = arrival;
    // lookAlong, not facingRotation: the latter takes the model's +Z onto an
    // axis, which is what turns the gate SLAB to its lane. A ship's nose is -Z,
    // so facingRotation here would arrive with the tail toward the playfield.
    transform.orientation = lookAlong(lookAt - arrival);
    // Both ends of the tick. The render nlerps previous->current, so writing
    // only one of the two swings the ship through the whole turn on screen.
    transform.previousOrientation = transform.orientation;
    m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
    m_playerDamageTimer = 0.0f;

    // You always know what you are touching (Phase 8z §B).
    identifyTouchedObjects(fromSystem);

    // Ambient faction presence (Phase 8b): owner wings + raid incursions.
    spawnAmbientPilots(systemIndex, spec);
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
    const Transform& transform = m_registry.storage<Transform>().get(playerEntityIndex());
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
        Transform& transform = m_registry.storage<Transform>().get(playerIndex);
        const core::DVec3 velocity = m_registry.storage<FlightBody>().get(playerIndex).velocity;
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
    Transform& transform = m_registry.storage<Transform>().get(playerIndex);
    const core::DVec3 pad = dockPoint(station);
    transform.position = pad;
    transform.previousPosition = pad;
    m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
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
    Transform& transform = m_registry.storage<Transform>().get(playerIndex);
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
    m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
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
    ecs::Pool<TraderPuppet>& puppets = m_registry.storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        if (puppets.values()[i].traderIndex == traderIndex) {
            handleShipDestroyed(puppets.entityIndices()[i]);
            return true;
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
    ecs::Pool<MinerPuppet>& miners = m_registry.storage<MinerPuppet>();
    for (std::size_t i = 0; i < miners.size(); ++i) {
        if (miners.values()[i].market == market) {
            handleShipDestroyed(miners.entityIndices()[i]);
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
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    // The same wrap currentTargetInfo() applies, because a targeted ship can
    // die out from under m_targetIndex and the hail must ask about whatever
    // the HUD is currently showing rather than about a stale slot.
    const std::size_t index = total > 0 ? m_targetIndex % total : 0;
    if (total == 0 || index < m_targets.size()) {
        say("Comms", "No ship selected to hail.");
        return false;
    }
    const SpawnedShip& ship = m_spawnedShips[index - m_targets.size()];
    const core::DVec3 position = m_registry.storage<Transform>().get(ship.entity.index).position;
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
    const ShipPilot* pilot = m_registry.tryGet<ShipPilot>(ship.entity);
    if (pilot == nullptr) {
        say("Comms", ship.name + " does not answer."); // an inert console spawn
        return false;
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
    const float unitPrice = m_economy.price(market, commodity);
    if (unitPrice > 0.0f) {
        units = std::min(units, static_cast<float>(m_playerCredits / unitPrice));
    }
    const sim::TradeResult result = m_economy.buy(market, commodity, units);
    m_playerCredits -= result.credits;
    m_playerCargo[commodity] += result.units;
    if (const std::uint32_t owner = systemOwnerFaction(m_currentSystem); owner < m_factionTable.size()) {
        m_factionSim.recordTrade(owner, result.credits); // commerce goodwill
    }
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
    if (const std::uint32_t owner = systemOwnerFaction(m_currentSystem); owner < m_factionTable.size()) {
        m_factionSim.recordTrade(owner, result.credits);
    }
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
    const core::DVec3 playerPosition = m_registry.storage<Transform>().get(playerEntityIndex()).position;
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
    const core::DVec3 playerPosition = m_registry.storage<Transform>().get(playerEntityIndex()).position;
    return length(gate->position - playerPosition);
}

const GateInstance* SpaceWorld::nearestGate() const
{
    if (m_gates.empty()) {
        return nullptr;
    }
    const core::DVec3 playerPosition = m_registry.storage<Transform>().get(playerEntityIndex()).position;
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
    ShipPower& power = m_registry.storage<ShipPower>().get(playerEntityIndex());
    sim::addPip(power.state.pips, system, power.tuning);
}

void SpaceWorld::playerBalancePips()
{
    ShipPower& power = m_registry.storage<ShipPower>().get(playerEntityIndex());
    sim::balancePips(power.state.pips, power.tuning);
}

void SpaceWorld::applyDefs(const assets::DefDatabase& defs)
{
    m_defs = &defs;
    if (!m_fleet.empty()) {
        applyActiveLoadout();
    } else if (const assets::ShipDef* playerDef = defs.findShip(kPlayerShipDefId)) {
        // Pre-universe (fleet not initialized yet): raw starter def.
        applyShipDef(playerEntityIndex(), *playerDef, defs);
        applyCockpitOf(*playerDef);
    } else {
        SOL_LOG_WARN("player ship def '%s' missing; keeping current tuning", kPlayerShipDefId);
    }

    for (const SpawnedShip& spawned : m_spawnedShips) {
        if (const assets::ShipDef* def = defs.findShip(spawned.defId.c_str())) {
            applyShipDef(spawned.entity.index, *def, defs);
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
    applyShipDef(playerEntityIndex(), def, *m_defs);
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
    ShipArmament* armament = m_registry.storage<ShipArmament>().tryGet(entityIndex);
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
    const ShipArmament* armament = m_registry.storage<ShipArmament>().tryGet(playerEntityIndex());
    return armament != nullptr ? armament->selectedGroup : 1;
}

std::uint32_t SpaceWorld::playerFireGroupsInUse() const
{
    const ShipArmament* armament = m_registry.storage<ShipArmament>().tryGet(playerEntityIndex());
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
    ShipArmament* armament = m_registry.storage<ShipArmament>().tryGet(playerEntityIndex());
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
    ShipArmament* armament = m_registry.storage<ShipArmament>().tryGet(playerEntityIndex());
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
    const double refund = kResaleRate * shipValue(ship);
    SOL_LOG_INFO("sold '%s' (+%.0f cr)", ship.defId.c_str(), refund);
    m_playerCredits += refund;
    m_fleet.erase(m_fleet.begin() + static_cast<std::ptrdiff_t>(fleetIndex));
    if (m_activeShip > fleetIndex) {
        --m_activeShip;
    }
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

void SpaceWorld::applyShipDef(std::uint32_t entityIndex,
                              const assets::ShipDef& def,
                              const assets::DefDatabase& defs)
{
    RenderShape& shape = m_registry.storage<RenderShape>().get(entityIndex);
    shape.scale = {def.scale, def.scale, def.scale};
    shape.model = modelIdFromName(defs, def.model, "ship def", kRoleShip);
    m_registry.storage<ShipControl>().get(entityIndex).tuning = toShipTuning(def.flight);
    if (entityIndex == playerEntityIndex()) {
        m_playerCargoCapacity = def.cargoCapacity;
        m_scanRange = def.scanRange > 0.0f ? def.scanRange : 1.0f;
        m_scanSpeed = def.scanSpeed > 0.0f ? def.scanSpeed : 1.0f;
        m_collectorRange = def.collectorRange > 0.0f ? def.collectorRange : 1.0f;
    }

    ShipPower& power = m_registry.storage<ShipPower>().get(entityIndex);
    power.tuning.weaponCapacitor = def.power.weaponCapacitor;
    power.tuning.weaponRechargeRate = def.power.weaponRecharge;
    if (power.state.weaponCharge > power.tuning.weaponCapacitor) {
        power.state.weaponCharge = power.tuning.weaponCapacitor;
    }

    // Def edits (and hot-reloads) refit the defenses at full strength.
    ShipDefense& defense = m_registry.storage<ShipDefense>().get(entityIndex);
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
    ShipMounts& mounts = m_registry.storage<ShipMounts>().get(entityIndex);
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

    ShipArmament& armament = m_registry.storage<ShipArmament>().get(entityIndex);
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
    ShipFittings& fittings = m_registry.storage<ShipFittings>().get(entityIndex);
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
    const ShipPilot* pilot = m_registry.tryGet<ShipPilot>(m_registry.entityFromIndex(entityIndex));
    if (pilot == nullptr) {
        return 2; // an inert console spawn: nobody is flying it, so it threatens nothing
    }
    if (pilot->state == PilotState::Attack && pilot->hasTarget != 0 &&
        pilot->targetIndex == playerEntityIndex()) {
        return 0;
    }
    // An unaffiliated console spawn has no faction to consult and Lua treats
    // it as unconditionally player-hostile (the pre-8b rule).
    if (pilot->factionIndex >= m_factionTable.size()) {
        return kHostileThreatTier;
    }
    return m_factionSim.playerHostile(pilot->factionIndex) ? kHostileThreatTier : 2;
}

GunneryFrame SpaceWorld::gunneryFrame(std::uint32_t entityIndex) const
{
    GunneryFrame frame;
    const Transform& transform = m_registry.storage<Transform>().get(entityIndex);
    frame.position = transform.position;
    frame.orientation = transform.orientation;
    frame.forward = toDVec3(rotate(transform.orientation, core::Vec3{0.0f, 0.0f, -1.0f}));
    if (const FlightBody* body = m_registry.storage<FlightBody>().tryGet(entityIndex); body != nullptr) {
        frame.velocity = body->velocity;
    }
    if (const RenderShape* shape = m_registry.storage<RenderShape>().tryGet(entityIndex); shape != nullptr) {
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
    if (entityIndex == playerEntityIndex()) {
        targetIndex = targetShipEntityIndex();
        if (targetIndex != kNoIndex && threatTier(targetIndex) > kHostileThreatTier) {
            targetIndex = kNoIndex;
        }
        // Through the registry rather than through the pool - see `threatTier`
        // for why a const reader of this pool must tolerate its absence.
    } else if (const ShipPilot* pilot = m_registry.tryGet<ShipPilot>(m_registry.entityFromIndex(entityIndex));
               pilot != nullptr && pilot->hasTarget != 0) {
        targetIndex = pilot->targetIndex;
    }
    if (targetIndex == kNoIndex) {
        return frame;
    }
    const Transform* targetTransform = m_registry.storage<Transform>().tryGet(targetIndex);
    if (targetTransform == nullptr) {
        return frame;
    }
    frame.hasTarget = true;
    frame.targetPosition = targetTransform->position;
    if (const FlightBody* body = m_registry.storage<FlightBody>().tryGet(targetIndex); body != nullptr) {
        frame.targetVelocity = body->velocity;
    }
    return frame;
}

std::uint32_t SpaceWorld::targetShipEntityIndex() const
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    if (total == 0) {
        return kNoIndex;
    }
    // The same wrap every other reader of the selection applies, because a
    // targeted ship can die out from under m_targetIndex.
    const std::size_t index = m_targetIndex % total;
    if (index < m_targets.size()) {
        return kNoIndex; // a station, a planet, a gate, a field: not a gunnery target
    }
    const ecs::Entity ship = m_spawnedShips[index - m_targets.size()].entity;
    return m_registry.isAlive(ship) ? ship.index : kNoIndex;
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
ArmamentSummary SpaceWorld::armamentSummary(std::uint32_t entityIndex) const
{
    ArmamentSummary summary;
    const ShipArmament* armament = m_registry.storage<ShipArmament>().tryGet(entityIndex);
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

ecs::Entity SpaceWorld::spawnShipAt(const assets::ShipDef& def,
                                    const assets::DefDatabase& defs,
                                    const core::DVec3& position,
                                    const char* factionName)
{
    const ecs::Entity e = m_registry.create();
    m_registry.emplace<Transform>(e, Transform{.position = position, .previousPosition = position});
    m_registry.emplace<RenderShape>(e, RenderShape{});
    m_registry.emplace<FlightBody>(e);
    // Default input is assist-on with zero commands = station-keeping until a
    // pilot (Phase 6 AI) writes real commands.
    m_registry.emplace<ShipControl>(e);
    m_registry.emplace<ShipPower>(e);
    m_registry.emplace<ShipDefense>(e);
    m_registry.emplace<ShipArmament>(e);
    m_registry.emplace<ShipFittings>(e);
    m_registry.emplace<ShipMounts>(e);
    applyShipDef(e.index, def, defs);
    std::string name = def.name;
    if (factionName != nullptr && factionName[0] != '\0') {
        name += std::string(" (") + factionName + ")";
    }
    m_spawnedShips.push_back({.entity = e, .defId = def.id, .name = std::move(name)});
    return e;
}

ecs::Entity SpaceWorld::spawnShipFromDef(const assets::ShipDef& def, const assets::DefDatabase& defs)
{
    const sim::ShipState player = shipState();
    const core::Vec3 forward = rotate(player.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
    const double distance = 150.0 + 100.0 * static_cast<double>(def.scale);
    const core::DVec3 position =
        player.position + core::DVec3{forward.x * distance, forward.y * distance, forward.z * distance};
    const ecs::Entity e = spawnShipAt(def, defs, position, nullptr);
    Transform& transform = m_registry.storage<Transform>().get(e.index);
    transform.orientation = player.orientation;
    transform.previousOrientation = player.orientation;
    return e;
}

TargetInfo SpaceWorld::currentTargetInfo() const
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
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
    if (shipSlot >= m_spawnedShips.size()) {
        return info;
    }
    const SpawnedShip& ship = m_spawnedShips[shipSlot];
    const Transform& transform = m_registry.storage<Transform>().get(ship.entity.index);
    info.nav = NavTarget{.name = ship.name, .position = transform.position, .surfaceRadius = 0.0};
    info.isShip = true;
    info.velocity = m_registry.storage<FlightBody>().get(ship.entity.index).velocity;
    if (const ShipPilot* pilot = m_registry.tryGet<ShipPilot>(ship.entity);
        pilot != nullptr && pilot->factionIndex < m_factionTable.size()) {
        info.factionName = m_factionTable[pilot->factionIndex].name;
        info.attitude = playerAttitudeName(pilot->factionIndex);
    }
    if (const ShipDefense* defense = m_registry.tryGet<ShipDefense>(ship.entity)) {
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
    if (index < m_planetTargetBase + m_planets.size()) {
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
    if (index >= m_planetTargetBase && index < m_planetTargetBase + m_planets.size()) {
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
    return addBookmarkAt(m_registry.storage<Transform>().get(playerEntityIndex()).position, name);
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
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    return total > 0 ? m_targetIndex % total : 0;
}

bool SpaceWorld::selectTarget(std::size_t index)
{
    if (index >= m_targets.size() + m_spawnedShips.size()) {
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
    if (m_spawnedShips.empty()) {
        return;
    }
    const core::DVec3 playerPosition = m_registry.storage<Transform>().get(playerEntityIndex()).position;

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
    ranked.reserve(m_spawnedShips.size());
    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        const SpawnedShip& ship = m_spawnedShips[i];
        const core::DVec3 offset =
            m_registry.storage<Transform>().get(ship.entity.index).position - playerPosition;
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
    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        if (m_spawnedShips[i].name.find(namePart) != std::string::npos) {
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
    m_registry.emplace<ShipPilot>(e, ShipPilot{.role = role, .factionIndex = factionIndex});
    if (factionIndex < m_factionTable.size()) {
        m_spawnedShips.back().name = def.name + " (" + m_factionTable[factionIndex].name + ")";
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
        break;
    }
    return {2, 2, 2};
}

} // namespace

bool SpaceWorld::pilotAttackPlayer(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Attack;
    pilot->targetIndex = playerEntityIndex();
    pilot->hasTarget = 1;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotEngageEnemy(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr || pilot->factionIndex >= m_factionTable.size()) {
        return false;
    }
    constexpr double kSensorRange = 8.0e4; // meters
    const core::DVec3 self = m_registry.storage<Transform>().get(entity.index).position;

    std::uint32_t bestTarget = kNoIndex;
    double bestDistance = kSensorRange;
    const auto consider = [&](std::uint32_t targetIndex) {
        const Transform* transform = m_registry.storage<Transform>().tryGet(targetIndex);
        const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(targetIndex);
        if (transform == nullptr || defense == nullptr || !defense->state.alive()) {
            return;
        }
        const double distance = length(transform->position - self);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestTarget = targetIndex;
        }
    };

    const ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
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
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotUnderFire(ecs::Entity entity) const
{
    const ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    return pilot != nullptr && pilot->threatTimer > 0.0f;
}

bool SpaceWorld::pilotEngageThreat(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr || pilot->threatTimer <= 0.0f) {
        return false;
    }
    // The threat is a remembered entity index rather than a search result, so
    // it has to be re-checked before it is flown at: the ship that shot us six
    // seconds ago may be dead, and an index outliving its entity is how a
    // pilot ends up attacking whatever was spawned into the slot next.
    const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(pilot->threatIndex);
    if (defense == nullptr || !defense->state.alive() ||
        m_registry.storage<Transform>().tryGet(pilot->threatIndex) == nullptr) {
        pilot->threatTimer = 0.0f;
        return false;
    }
    pilot->state = PilotState::Attack;
    pilot->targetIndex = pilot->threatIndex;
    pilot->hasTarget = 1;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotHuntTrader(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    const Transform* transform = pilot != nullptr ? m_registry.tryGet<Transform>(entity) : nullptr;
    const ShipControl* control = pilot != nullptr ? m_registry.tryGet<ShipControl>(entity) : nullptr;
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

    m_preyCandidates.clear();
    const ecs::Pool<TraderPuppet>& puppets = m_registry.storage<TraderPuppet>();
    for (std::size_t i = 0; i < puppets.size(); ++i) {
        const std::uint32_t index = puppets.entityIndices()[i];
        const Transform* body = m_registry.storage<Transform>().tryGet(index);
        const ShipPilot* hauler = m_registry.storage<ShipPilot>().tryGet(index);
        const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(index);
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
    const ecs::Pool<MinerPuppet>& miners = m_registry.storage<MinerPuppet>();
    for (std::size_t i = 0; i < miners.size(); ++i) {
        const std::uint32_t index = miners.entityIndices()[i];
        const Transform* body = m_registry.storage<Transform>().tryGet(index);
        const ShipPilot* crew = m_registry.storage<ShipPilot>().tryGet(index);
        const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(index);
        if (body == nullptr || crew == nullptr || defense == nullptr || !defense->state.alive()) {
            continue;
        }
        m_preyCandidates.push_back({.index = index,
                                    .position = body->position,
                                    .faction = crew->factionIndex,
                                    .paced = false,
                                    .inbound = true});
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

    const double distance = length(m_registry.storage<Transform>().get(prey).position - transform->position);
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
    const ArmamentSummary armament = armamentSummary(entity.index);
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
    const TraderPuppet* preyPuppet = m_registry.storage<TraderPuppet>().tryGet(prey);
    pilot->waypoint =
        preyPuppet != nullptr ? preyPuppet->destination : m_registry.storage<Transform>().get(prey).position;

    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotFlee(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    // Run from what is actually shooting. Flee steers away from targetIndex,
    // and before Phase 8x nothing guaranteed that field meant anything at all
    // — a hauler that had never picked a target fled from entity 0, which is
    // the player, so the one ship coming to help was the one it ran from.
    if (pilot->threatTimer > 0.0f && m_registry.storage<Transform>().tryGet(pilot->threatIndex) != nullptr) {
        pilot->targetIndex = pilot->threatIndex;
        pilot->hasTarget = 1;
    }
    pilot->state = PilotState::Flee;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotIdle(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Idle;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotPatrolTo(ecs::Entity entity, core::DVec3 waypoint)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Patrol;
    pilot->waypoint = waypoint;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

// The primitive decisions/019 §3 assumed was already there - see the header for
// why `pilotPatrolTo` above could not stand in for it.
bool SpaceWorld::pilotTravelTo(ecs::Entity entity, core::DVec3 waypoint)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Travel;
    pilot->waypoint = waypoint;
    pilot->respondTimer = kResponseGiveUpSeconds;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
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
    (void)cause; // one cause so far; Phase 36 is where this branches
    m_lastResponse = ResponseReport{};
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner >= m_factionTable.size() || m_defs == nullptr) {
        return 0;
    }
    const float live = systemSecurity(m_currentSystem);
    const float baseline = systemSecurityBaseline(m_currentSystem);
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
    const ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
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
        const Transform* transform = m_registry.storage<Transform>().tryGet(index);
        const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(index);
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
        if (pilotTravelTo(m_registry.entityFromIndex(candidate.index), position)) {
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
    if (m_lastResponse.diverted >= wanted) {
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
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
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
        for (const GateInstance& gate : m_gates) {
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
        faction.pirate ? factionRoster(faction, assets::RosterCell::Raider, assets::RosterCell::Count)
                       : factionRoster(faction, assets::RosterCell::Patrol, assets::RosterCell::Raider);
    const std::size_t before = m_registry.storage<ShipPilot>().size();
    spawnWing(owner,
              faction.pirate ? assets::RosterCell::Raider : assets::RosterCell::Patrol,
              roster,
              baseline,
              shortfall,
              origin,
              700.0,
              PilotState::Travel,
              &position);
    m_lastResponse.spawned = static_cast<std::uint32_t>(m_registry.storage<ShipPilot>().size() - before);
    if (m_lastResponse.spawned > 0) {
        SOL_LOG_INFO("response: %u launched from %.0f km out, live %+.3f, reach %.0f km",
                     m_lastResponse.spawned,
                     best / 1000.0,
                     static_cast<double>(live),
                     reach / 1000.0);
    }
    return m_lastResponse.diverted + m_lastResponse.spawned;
}

void SpaceWorld::considerResponse(std::uint32_t targetIndex, std::uint32_t attackerIndex, core::DVec3 at)
{
    if (m_responseCooldown > 0.0 || attackerIndex == kNoIndex || attackerIndex == targetIndex) {
        return;
    }
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner >= m_factionTable.size()) {
        return;
    }
    // Somebody the local law protects. There is no CRIME until Phase 36, so
    // the trigger is the thing that already exists: a hull belonging to the
    // faction that polices this system taking fire from one that does not.
    const ShipPilot* victim = m_registry.storage<ShipPilot>().tryGet(targetIndex);
    if (victim == nullptr || victim->factionIndex != owner) {
        return;
    }
    if (attackerIndex != playerEntityIndex()) {
        const ShipPilot* attacker = m_registry.storage<ShipPilot>().tryGet(attackerIndex);
        if (attacker == nullptr || attacker->factionIndex == owner) {
            return; // friendly fire is not an incident anybody is dispatched to
        }
    }
    m_responseCooldown = kResponseCooldownSeconds;
    (void)respondTo(at, attackerIndex, ResponseCause::WeaponsFire);
}

double SpaceWorld::shipHullFraction(ecs::Entity entity) const
{
    if (!m_registry.isAlive(entity)) {
        return 0.0;
    }
    const ShipDefense* defense = m_registry.tryGet<ShipDefense>(entity);
    if (defense == nullptr || defense->tuning.hull <= 0.0f) {
        return 0.0;
    }
    return static_cast<double>(defense->state.hull / defense->tuning.hull);
}

void SpaceWorld::collectDuePilotThinks(double dt, std::vector<PilotThink>& out)
{
    static constexpr const char* kStateNames[] = {"idle", "patrol", "attack", "flee", "travel"};
    constexpr float kThinkInterval = 0.5f; // 2 Hz strategy; steering runs at 60

    // The dispatch throttle ages with the pilots it throttles (Phase 30 stage
    // C): this is the one per-frame pass that already has a dt and runs whether
    // or not anything is shooting.
    m_responseCooldown = std::max(0.0, m_responseCooldown - dt);

    ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        ShipPilot& pilot = pilots.values()[i];
        // The threat window ages here rather than in the steering pass: this
        // is the one loop that visits every pilot exactly once with a dt, and
        // a pilot with no hull left to shoot at still has to forget.
        if (pilot.threatTimer > 0.0f) {
            pilot.threatTimer = std::max(0.0f, pilot.threatTimer - static_cast<float>(dt));
        }
        // A dispatched responder gives up on the same terms and for the same
        // reason (Phase 30 stage C): a call you have been flying at for three
        // minutes without finding anything is a call that is over.
        if (pilot.respondTimer > 0.0f) {
            pilot.respondTimer = std::max(0.0f, pilot.respondTimer - static_cast<float>(dt));
            if (pilot.respondTimer == 0.0f && pilot.state == PilotState::Travel) {
                pilot.state = PilotState::Idle; // Lua's next think puts it back on its beat
            }
        }
        pilot.thinkTimer -= static_cast<float>(dt);
        if (pilot.thinkTimer > 0.0f) {
            continue;
        }
        pilot.thinkTimer = kThinkInterval;
        const char* attitude =
            pilot.factionIndex < m_factionTable.size() ? playerAttitudeName(pilot.factionIndex) : "none";
        out.push_back({
            .entity = m_registry.entityFromIndex(pilots.entityIndices()[i]),
            .role = pilotRoleName(pilot.role),
            .state = kStateNames[static_cast<std::uint32_t>(pilot.state) % std::size(kStateNames)],
            .attitude = attitude,
            .pirate = pilot.factionIndex < m_factionTable.size() && m_factionTable[pilot.factionIndex].pirate,
        });
    }
}

sim::ShipState SpaceWorld::shipState() const
{
    const std::uint32_t shipIndex = playerEntityIndex();
    const Transform& transform = m_registry.storage<Transform>().get(shipIndex);
    const FlightBody& body = m_registry.storage<FlightBody>().get(shipIndex);
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
    if (commandNeedsTarget(mode) && m_targets.empty() && m_spawnedShips.empty()) {
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

void SpaceWorld::tick(double dt)
{
    // The run's own clock. Market intel is stamped against it, so it advances
    // whether the player is docked or flying — a price you read an hour ago
    // is an hour old either way.
    m_worldSeconds += dt;
    // What nothing may fly into, this tick (Phase 8y). Before any steering,
    // the player's autopilot included.
    {
        const std::uint32_t zone = core::frameProfiler().beginZone("sim.avoidance");
        rebuildAvoidance();
        core::frameProfiler().addCounter(zone, m_avoidance.size());
        core::frameProfiler().endZone(zone);
    }
    const std::uint32_t playerIndex = playerEntityIndex();
    if (isDocked()) {
        // Parked: flight input is ignored and the ship stays pinned to the
        // pad (collision impulses must not drift a docked ship).
        //
        // GUARD 2 of commandInput's four. Docking ends every command, standing
        // ones included: a ship on a pad is not orbiting anything.
        clearCommand();
        m_appliedInput = sim::FlightInput{};
        m_registry.storage<ShipControl>().get(playerIndex).input = sim::FlightInput{};
        Transform& transform = m_registry.storage<Transform>().get(playerIndex);
        const core::DVec3 pad = dockPoint(m_dockedStation);
        transform.position = pad;
        transform.previousPosition = pad;
        m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
    } else {
        m_appliedInput = m_commandMode != CommandMode::None ? commandInput() : m_shipInput;
        // The manual-cruise guard is about a cruise burn the PLAYER lit, so it
        // asks whether the ship is flying itself, not whether it is on
        // autopilot specifically.
        if (m_commandMode == CommandMode::None && m_appliedInput.cruise) {
            guardManualCruise(dt);
        } else {
            m_cruiseWarningTimer = 0.0;
        }
        m_registry.storage<ShipControl>().get(playerIndex).input = m_appliedInput;
    }

    // NPC pilots: C++ steering flies whatever state Lua's pilot_think chose.
    {
        SOL_PROFILE_ZONE_NAMED(pilotZone, "sim.pilots");
        ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
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
            ShipControl* control = m_registry.storage<ShipControl>().tryGet(entityIndex);
            const Transform* transform = m_registry.storage<Transform>().tryGet(entityIndex);
            const FlightBody* body = m_registry.storage<FlightBody>().tryGet(entityIndex);
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
                pilot.hasTarget != 0 && pilot.targetIndex == playerIndex &&
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
            case PilotState::Attack: {
                const Transform* targetTransform = m_registry.storage<Transform>().tryGet(pilot.targetIndex);
                const FlightBody* targetBody = m_registry.storage<FlightBody>().tryGet(pilot.targetIndex);
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
                const ArmamentSummary armament = armamentSummary(entityIndex);
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
                        (m_registry.storage<TraderPuppet>().tryGet(pilot.targetIndex) != nullptr ||
                         m_registry.storage<MinerPuppet>().tryGet(pilot.targetIndex) != nullptr)) {
                        if (ShipPilot* hunted = m_registry.storage<ShipPilot>().tryGet(pilot.targetIndex)) {
                            hunted->threatIndex = entityIndex;
                            hunted->threatTimer = static_cast<float>(kThreatMemorySeconds);
                        }
                    }
                }
                break;
            }
            case PilotState::Flee: {
                const Transform* threatTransform = m_registry.storage<Transform>().tryGet(pilot.targetIndex);
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
    ecs::Pool<ShipPower>& powers = m_registry.storage<ShipPower>();
    ecs::Pool<ShipDefense>& defenses = m_registry.storage<ShipDefense>();
    {
        SOL_PROFILE_ZONE("sim.flight");
        m_registry.view<Transform, FlightBody, ShipControl>().each(
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
                const ShipMounts* condition = m_registry.tryGet<ShipMounts>(entity);
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
    const ecs::Pool<FlightBody>& bodies = m_registry.storage<FlightBody>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
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
    ecs::Pool<Projectile>& projectiles = m_registry.storage<Projectile>();
    const ecs::Pool<OreChunk>& oreChunks = m_registry.storage<OreChunk>();
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
    m_collisionBodies.push_back({.previousPosition = m_star.position,
                                 .position = m_star.position,
                                 .velocity = {},
                                 .radius = m_star.radius,
                                 .inverseMass = 0.0});
    for (const CelestialBody& planet : m_planets) {
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
        m_registry.storage<Transform>().get(entityIndex).position = body.position;
        m_registry.storage<FlightBody>().get(entityIndex).velocity = body.velocity;
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
        if (defense == nullptr || !defense->state.alive() || isDamageImmune(entityIndex)) {
            return;
        }
        const core::Quat orientation = m_registry.storage<Transform>().get(entityIndex).orientation;
        const sim::ShieldFacing facing = sim::facingForHit(orientation, toSource);
        const sim::DamageResult result =
            sim::applyDamage(defense->state, defense->tuning, facing, static_cast<float>(damage));
        noteDamage(entityIndex,
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
        handleShipDestroyed(destroyed.victim, destroyed.attacker);
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
        Transform& transform = m_registry.storage<Transform>().get(projectileIndex);
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
                    defense != nullptr && defense->state.alive() && !isDamageImmune(targetIndex)) {
                    const core::DVec3 toSource = normalize(projectile.velocity) * -1.0;
                    const sim::ShieldFacing facing = sim::facingForHit(
                        m_registry.storage<Transform>().get(targetIndex).orientation, toSource);
                    const sim::DamageResult result =
                        sim::applyDamage(defense->state, defense->tuning, facing, projectile.damage);
                    noteDamage(targetIndex,
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
        m_registry.destroy(m_registry.entityFromIndex(index));
    }
    for (const DestroyedShip& destroyed : destroyedShips) {
        handleShipDestroyed(destroyed.victim, destroyed.attacker);
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
    ecs::Pool<ShipArmament>& armaments = m_registry.storage<ShipArmament>();

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
        core::DVec3 impact;
        float units;
        bool wreck;
    };

    std::vector<PendingCut> pendingCuts;
    for (std::size_t a = 0; a < armaments.size(); ++a) {
        ShipArmament& armament = armaments.values()[a];
        const std::uint32_t entityIndex = armaments.entityIndices()[a];
        const ShipControl* control = m_registry.storage<ShipControl>().tryGet(entityIndex);
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
        const GunneryFrame frame = trigger ? gunneryFrame(entityIndex) : GunneryFrame{};
        ShipPower* power = powers.tryGet(entityIndex);
        // ⚑ ONE READ PER SHIP, on the same rule as the frame above: which
        // mounts are still there is a fact about the SHIP, and the inner loop
        // only needs to index it (Phase 31 stage F).
        const ShipMounts* mounts = m_registry.tryGet<ShipMounts>(m_registry.entityFromIndex(entityIndex));

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
                if (entityIndex == playerEntityIndex()) {
                    m_audio->play2D(m_audio->cues().weaponFire);
                } else {
                    m_audio->playAt(m_audio->cues().weaponFire, muzzle);
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
                if (weapon.miningPower > 0.0f && entityIndex == playerEntityIndex()) {
                    const ecs::Pool<MineableRock>& rockPool = m_registry.storage<MineableRock>();
                    const ecs::Pool<WreckMarker>& wreckPool = m_registry.storage<WreckMarker>();
                    const auto sweepCuttable = [&](std::uint32_t candidate, bool isWreck) {
                        const RenderShape& candidateShape = m_registry.storage<RenderShape>().get(candidate);
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
                                           .impact = muzzle + (beamEnd - muzzle) * miningT,
                                           .units = weapon.miningPower /
                                                    (weapon.rateOfFire > 0.01f ? weapon.rateOfFire : 0.01f),
                                           .wreck = miningWreck});
                } else if (hit) {
                    if (ShipDefense* defense = defenses.tryGet(bestTarget);
                        defense != nullptr && defense->state.alive() && !isDamageImmune(bestTarget)) {
                        // Which of the target's shields eats it: the arrival
                        // direction is the BEAM's, not the shooter's nose, so a
                        // turret firing aft off a fleeing freighter lands on the
                        // shield actually facing it.
                        const sim::ShieldFacing facing = sim::facingForHit(
                            m_registry.storage<Transform>().get(bestTarget).orientation, bearing * -1.0);
                        const sim::DamageResult result =
                            sim::applyDamage(defense->state, defense->tuning, facing, weapon.damage);
                        noteDamage(bestTarget, muzzle + (beamEnd - muzzle) * bestT, result, entityIndex);
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
        handleShipDestroyed(destroyed.victim, destroyed.attacker);
    }
    for (const PendingCut& cut : pendingCuts) {
        if (cut.wreck) {
            (void)cutWreck(cut.entityIndex, cut.units);
            m_combatEffects.spawnImpact(cut.impact, false);
            if (m_audio != nullptr) {
                m_audio->playAt(m_audio->cues().miningCut, cut.impact);
            }
            continue;
        }
        const MineableRock* rock = m_registry.storage<MineableRock>().tryGet(cut.entityIndex);
        if (rock == nullptr) {
            continue; // two beams on one rock in a tick; the first broke it up
        }
        const std::uint32_t field = rock->field;
        const std::uint32_t index = rock->index;
        const std::uint32_t commodity = rock->commodity;
        const float total = rock->totalUnits;
        (void)cutRock(cut.entityIndex, cut.units);
        m_combatEffects.spawnImpact(cut.impact, false);
        if (m_audio != nullptr) {
            m_audio->playAt(m_audio->cues().miningCut, cut.impact);
        }
        if (m_mining.unitsLeft(m_currentSystem, field, index, total) <= 0.0f) {
            m_registry.destroy(m_registry.entityFromIndex(cut.entityIndex)); // it broke up
            m_rockEvents.push_back({.commodity = commodity, .units = total});
        }
    }
    for (const PendingBolt& bolt : newBolts) {
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(e,
                                      Transform{.position = bolt.position,
                                                .previousPosition = bolt.position,
                                                .orientation = bolt.orientation,
                                                .previousOrientation = bolt.orientation});
        m_registry.emplace<RenderShape>(e, RenderShape{.scale = {0.3f, 0.3f, 4.0f}, .model = bolt.model});
        m_registry.emplace<Projectile>(e,
                                       Projectile{.velocity = bolt.velocity,
                                                  .lifetime = bolt.lifetime,
                                                  .damage = bolt.damage,
                                                  .shooterIndex = bolt.shooterIndex});
    }
    profiler.endZone(weaponZone);

    // Feedback bookkeeping.
    m_combatEffects.tick(dt);
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

    // Mining (Phase 8f): rock tumble, chunk drift and collection, wreck decay
    // and reconciliation, refinery orders.
    {
        const std::uint32_t zone = profiler.beginZone("sim.mining");
        tickMining(dt);
        profiler.endZone(zone);
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

    // Bodies for the coarse traders flying a leg here (Phase 8x). Immediately
    // after the economy tick, because that is the one place a trader's route
    // can change — this is the LOD promotion §2 committed to and the coarse
    // layer has been waiting for since Phase 7.
    {
        const std::uint32_t zone = profiler.beginZone("sim.puppets");
        syncTraderPuppets();
        // And a ship at the rock for every outpost here that is digging
        // (Phase 8x stage 6). Same reconcile, same rule, a different coarse
        // actor: an extractor's draw is activity the sim has been performing
        // since Phase 8g with nothing in the sky to show for it.
        syncMinerPuppets(dt);
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
            Transform& transform = m_registry.storage<Transform>().get(playerEntityIndex());
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
void SpaceWorld::damageMounts(std::uint32_t targetIndex,
                              const core::DVec3& hitPosition,
                              const sim::DamageResult& result)
{
    const float reaching = result.armorAbsorbed + result.hullDamage;
    if (reaching <= 0.0f) {
        return; // the facing held; nothing under it was touched
    }
    const ecs::Entity entity = m_registry.entityFromIndex(targetIndex);
    ShipMounts* mounts = m_registry.tryGet<ShipMounts>(entity);
    const Transform* transform = m_registry.tryGet<Transform>(entity);
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
    const RenderShape* shape = m_registry.tryGet<RenderShape>(entity);
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
        m_combatEffects.spawnExplosion(where, hullScale * 0.5f);
        if (m_audio != nullptr) {
            if (targetIndex == playerEntityIndex()) {
                m_audio->play2D(m_audio->cues().explosion);
                m_audio->play2D(m_audio->cues().alarm);
            } else {
                m_audio->playAt(m_audio->cues().explosion, where);
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

void SpaceWorld::noteDamage(std::uint32_t targetIndex,
                            const core::DVec3& hitPosition,
                            const sim::DamageResult& result,
                            std::uint32_t attackerIndex)
{
    // ⚑⚑ FIRST, AND ABOVE THE PLAYER EARLY-OUT BELOW (Phase 31 stage F). It
    // is in this function because this is the one funnel every hit in the game
    // passes through carrying all three facts it needs - who was hit, where,
    // and how much got past what - and it is FIRST because the early return
    // twenty lines down is about who gets a bounty assist. The player's own
    // mounts are shot off exactly like everybody else's, and hanging this off
    // the bottom of the function is how they would not be.
    damageMounts(targetIndex, hitPosition, result);

    const bool shieldHit = result.shieldAbsorbed >= result.armorAbsorbed + result.hullDamage;
    m_combatEffects.spawnImpact(hitPosition, shieldHit);
    if (m_audio != nullptr) {
        const sol::audio::SoundId cue = shieldHit ? m_audio->cues().hitShield : m_audio->cues().hitHull;
        // A hit on the player is a hit on the listener: 2D, because the sound
        // is your own hull and it has no direction to come from.
        if (targetIndex == playerEntityIndex()) {
            m_audio->play2D(cue);
            // The alarm is for damage that is actually costing you something.
            // Every shield hit would make it a drone rather than a warning.
            if (result.hullDamage > 0.0f) {
                m_audio->play2D(m_audio->cues().alarm);
            }
        } else {
            m_audio->playAt(cue, hitPosition);
        }
    }
    if (targetIndex == playerEntityIndex()) {
        m_playerDamageTimer = kDamageFlashSeconds;
        return; // the player assisting their own death is not a thing
    }
    // Phase 8l: re-arm the victim's assist window so a kill someone else
    // finishes still counts toward a bounty the player was fighting for.
    if (attackerIndex == playerEntityIndex()) {
        if (ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(targetIndex)) {
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
        if (ShipPilot* pilot = m_registry.storage<ShipPilot>().tryGet(targetIndex)) {
            pilot->threatIndex = attackerIndex;
            pilot->threatTimer = static_cast<float>(kThreatMemorySeconds);
        }
    }
    // Phase 30 stage C: and tell whoever polices this place. The same argument
    // the comment above makes is why the hook belongs here - damage is the one
    // site that knows who shot whom for certain, and every hit funnels through
    // it. `considerResponse` decides whether it is an incident and throttles a
    // burst of hits into one call.
    considerResponse(targetIndex, attackerIndex, hitPosition);
}

void SpaceWorld::handleShipDestroyed(std::uint32_t entityIndex, std::uint32_t attackerIndex)
{
    // Fireball at the wreck site, scaled by the hull.
    const core::DVec3 wreckPosition = m_registry.storage<Transform>().get(entityIndex).position;
    m_combatEffects.spawnExplosion(wreckPosition, m_registry.storage<RenderShape>().get(entityIndex).scale.x);
    if (m_audio != nullptr) {
        if (entityIndex == playerEntityIndex()) {
            m_audio->play2D(m_audio->cues().explosion);
        } else {
            m_audio->playAt(m_audio->cues().explosion, wreckPosition);
        }
    }
    if (entityIndex == playerEntityIndex()) {
        // Cargo is lost either way (decisions/007).
        std::fill(m_playerCargo.begin(), m_playerCargo.end(), 0.0f);
        if (m_hardcore) {
            // Hardcore: the run is over — the caller deletes the save (see
            // consumeHardcoreDeath) and a fresh run starts at the new-game
            // system in the starter ship. The world itself keeps running.
            SOL_LOG_WARN("ship destroyed - HARDCORE: run over; save will be deleted");
            m_hardcoreDeathPending = true;
            m_playerCredits = 1'000.0;
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
        Transform& transform = m_registry.storage<Transform>().get(entityIndex);
        transform = Transform{.position = m_playerSpawn, .previousPosition = m_playerSpawn};
        m_registry.storage<FlightBody>().get(entityIndex) = FlightBody{};
        ShipDefense& defense = m_registry.storage<ShipDefense>().get(entityIndex);
        sim::resetDefense(defense.state, defense.tuning);
        ShipPower& power = m_registry.storage<ShipPower>().get(entityIndex);
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
        if (ShipMounts* mounts = m_registry.tryGet<ShipMounts>(m_registry.entityFromIndex(entityIndex))) {
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
    if (const ShipPilot* pilot = m_registry.storage<ShipPilot>().tryGet(entityIndex);
        pilot != nullptr && pilot->factionIndex < m_factionTable.size()) {
        const bool playerKilled = attackerIndex == playerEntityIndex();
        const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(entityIndex);
        const bool playerAssisted = defense != nullptr && defense->playerAssist > 0.0;

        if (playerKilled) {
            m_factionSim.recordShipKill(pilot->factionIndex);
            SOL_LOG_INFO("kill vs %s: standing now %.1f (%s)",
                         m_factionTable[pilot->factionIndex].name.c_str(),
                         m_factionSim.standing(pilot->factionIndex),
                         playerAttitudeName(pilot->factionIndex));
        }
        if (playerKilled || playerAssisted) {
            m_missions.notifyKill(pilot->factionIndex, m_currentSystem);
            // Territory (Phase 8u): the same "was the player in this fight"
            // rule 8l defined decides whether a kill pushes a contest back.
            // Only the player's kills reach here - nothing simulates a war's
            // attrition, so crediting ambient dogfights would invent state
            // the sim does not have and make the meter a lie.
            const float before = m_factionSim.contestOf(m_currentSystem).pressure;
            m_factionSim.recordContestKill(m_currentSystem, pilot->factionIndex);
            const float after = m_factionSim.contestOf(m_currentSystem).pressure;
            if (after < before) {
                SOL_LOG_INFO("contest in system %u: pressure %.2f -> %.2f",
                             m_currentSystem,
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
    if (const TraderPuppet* puppet = m_registry.storage<TraderPuppet>().tryGet(entityIndex);
        puppet != nullptr && m_economy.loseTrader(puppet->traderIndex)) {
        m_factionSim.recordTraderLoss(m_currentSystem, puppet->traderIndex);
        // Who fired is known here and nowhere else (Phase 8x §E). It matters
        // for exactly one thing: an escort contract on a hauler the player
        // shot themselves is a failure they are charged for, not a loss they
        // are excused. 8l's assist window counts, so finishing your own charge
        // off through local security is the same betrayal.
        if (attackerIndex == playerEntityIndex()) {
            m_playerKilledTraders.push_back(puppet->traderIndex);
        } else if (const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(entityIndex);
                   defense != nullptr && defense->playerAssist > 0.0) {
            m_playerKilledTraders.push_back(puppet->traderIndex);
        }
    }

    // A miner dying stops its outpost digging (Phase 8x stage 6), which is the
    // same idea one actor over: the entity is a view of a draw, so removing it
    // has to reach the draw or the ship was scenery. It is the only way the
    // player can reach into a station's production directly — and it is
    // temporary, because the outpost sends another ship out.
    if (const MinerPuppet* miner = m_registry.storage<MinerPuppet>().tryGet(entityIndex);
        miner != nullptr && miner->market < m_minerHold.size()) {
        m_minerHold[miner->market] = kMinerReplacementLegs * m_economy.params().traderLegSeconds;
        const sim::StationMarket& row = m_economy.markets()[miner->market];
        SOL_LOG_INFO("[mining] %s loses its miner: no draw for %.0f s",
                     m_galaxy.systems[row.systemIndex].stations[row.stationIndex].name.c_str(),
                     m_minerHold[miner->market]);
    }

    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        if (m_spawnedShips[i].entity.index != entityIndex) {
            continue;
        }
        SOL_LOG_INFO("'%s' destroyed", m_spawnedShips[i].defId.c_str());

        // A wreck stays where it fell (Phase 8f). The record is what persists
        // — the entity to cut is materialized by tickMining, here and after a
        // jump back or a reload — and its contents are composed now, from the
        // ship that actually died, so a later def edit cannot rewrite it.
        const core::DVec3 where = m_registry.storage<Transform>().get(entityIndex).position;
        const std::string defId = m_spawnedShips[i].defId;
        const std::string name = m_spawnedShips[i].name;
        const ShipPilot* pilot = m_registry.storage<ShipPilot>().tryGet(entityIndex);
        const std::uint32_t faction = pilot != nullptr ? pilot->factionIndex : kNoIndex;
        const std::uint64_t seed =
            core::Rng(
                m_universeSeed ^ (static_cast<std::uint64_t>(entityIndex + 1) * 0x9e37'79b9'7f4a'7c15ull), 13)
                .nextU64();
        const std::uint32_t wreckId = m_mining.addWreck(m_currentSystem, where, defId, name, seed);
        if (wreckId != 0) {
            const assets::ShipDef* def = m_defs != nullptr ? m_defs->findShip(defId.c_str()) : nullptr;
            // Scriptless default first, so a hull always holds something even
            // if no script answers; the Lua hook may replace it before it is
            // cut into.
            (void)m_mining.setWreckContents(wreckId, defaultWreckLoot(def, seed));
            m_wreckEvents.push_back({.id = wreckId,
                                     .system = m_currentSystem,
                                     .defId = defId,
                                     .factionName = faction < m_factionTable.size()
                                                        ? m_factionTable[faction].name
                                                        : std::string(),
                                     .seed = seed});
        }

        m_registry.destroy(m_spawnedShips[i].entity);
        m_spawnedShips.erase(m_spawnedShips.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

Transform SpaceWorld::shipRenderTransform(float alpha) const
{
    const std::uint32_t shipIndex = playerEntityIndex();
    const Transform& transform = m_registry.storage<Transform>().get(shipIndex);

    Transform blended = transform;
    blended.position = transform.previousPosition +
                       (transform.position - transform.previousPosition) * static_cast<double>(alpha);
    blended.orientation = nlerp(transform.previousOrientation, transform.orientation, alpha);
    return blended;
}

void SpaceWorld::buildRenderInstances(float alpha, bool includeShip, std::vector<RenderInstance>& out) const
{
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
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
        const ecs::Entity entity = m_registry.entityFromIndex(entityIndices[i]);
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
    const ecs::Pool<ShipArmament>& armaments = m_registry.storage<ShipArmament>();
    const ecs::Pool<ShipFittings>& fittings = m_registry.storage<ShipFittings>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
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
        const ShipMounts* condition = m_registry.tryGet<ShipMounts>(m_registry.entityFromIndex(entityIndex));
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

        const GunneryFrame frame = gunneryFrame(entityIndex);
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
    *this = SpaceWorld{};
    spawn(seed);
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
    m_economy.save(writer);
    m_factionSim.save(writer); // v5: relations, war flags, standings, raids
    m_missions.save(writer);   // v6: journal, board, campaign stage
    m_survey.save(writer);     // v7: knowledge, signal state, ledger, route
    m_mining.save(writer);     // v8: rock depletion, wrecks, refinery orders
    makeSnapshotSchema().save(m_registry, writer);
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

    ecs::Registry fresh;
    if (!makeSnapshotSchema().load(fresh, reader)) {
        return false;
    }
    if (fresh.storage<PlayerShip>().size() != 1) {
        return false; // not a current-format save (or player identity lost)
    }
    m_registry = std::move(fresh);
    ensureMiningPools(); // the snapshot only carries pools the save had in it
    // Def-spawned entities were replaced wholesale; their def association is
    // gone (visuals persist via the saved RenderShape).
    m_spawnedShips.clear();
    m_combatEffects.clear();
    m_thrusters.clear();
    m_playerCredits = credits;
    m_playerCargo = std::move(cargo);
    m_hardcore = hardcore != 0;
    m_worldSeconds = worldSeconds; // intel ages continue where they left off
    m_hardcoreDeathPending = false;
    m_fleet = std::move(fleet);
    m_activeShip = activeIndex;
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
    }

    // The snapshot carries the system's statics; only the non-ECS side data
    // (celestials, targets, gates, spawn anchor) needs rebuilding.
    m_currentSystem = systemIndex;
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
