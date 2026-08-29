#include "sol/sim/universe.hpp"

// Station placement asks whether a system has rock (Phase 13). The dependency
// is one-way and lives here in the .cpp: mining.hpp includes universe.hpp, and
// universe.hpp only forward-declares MiningParams, so there is no cycle.
#include "sol/core/assert.hpp"
#include "sol/core/random.hpp"
#include "sol/sim/mining.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace sol::sim {

namespace {

// One Rng stream per generation stage, so adding draws to one stage never
// perturbs another (the random.hpp contract).
constexpr std::uint32_t kInvalidIndex = 0xffff'ffffu;

enum Stream : std::uint64_t
{
    kStreamPositions = 1,
    kStreamNames,
    kStreamFactions,
    kStreamContents,
    kStreamClans,
};

// ⚑⚑⚑ THE ENUM'S TAIL IS NOT FREE SPACE, AND A NEW STAGE MUST NOT APPEND TO IT.
// Below, each system's own seed is drawn from `kStreamContents + i` - stream ids
// 4, 5, 6, ... - so the per-system range runs off the end of this enum and over
// whatever follows. `kStreamClans` is 5, which means system 1's seed stream and
// the clan stream ARE THE SAME STREAM today. That is deterministic and harmless
// (they are read at different times for different things) and it is not Phase
// 29's to fix, but it makes appending a member a collision rather than a
// no-op. An id chosen to sit above any plausible system index is not elegant;
// it is the only thing here that is actually true.
constexpr std::uint64_t kStreamAuthored = 1'000'000; // > kStreamContents + systemCount, always

constexpr const char* kNamePrefixes[] = {
    "Al",  "Be",  "Ca",  "Dra", "Er",  "Fen", "Gal", "Hel", "Ith", "Ka",
    "Lyr", "Mar", "Nor", "Oph", "Pra", "Que", "Rig", "Sar", "Tau", "Vel",
};
constexpr const char* kNameMiddles[] = {
    "an",
    "ar",
    "en",
    "es",
    "ia",
    "io",
    "or",
    "ub",
    "un",
    "yr",
};
constexpr const char* kNameSuffixes[] = {
    "a",
    "ea",
    "ia",
    "is",
    "os",
    "ra",
    "th",
    "um",
    "us",
    "yx",
};
constexpr const char* kStationOrdinals[] = {
    "Alpha",
    "Beta",
    "Gamma",
    "Delta",
    "Epsilon",
    "Zeta",
    "Eta",
    "Theta",
};
constexpr const char* kPlanetNumerals[] = {"I", "II", "III", "IV", "V", "VI"};
constexpr const char* kClanSuffixes[] = {
    "Raiders",
    "Corsairs",
    "Cartel",
    "Syndicate",
    "Reavers",
    "Wolves",
    "Marauders",
    "Talons",
};

template <std::size_t N>
[[nodiscard]] const char* pick(core::Rng& rng, const char* const (&table)[N])
{
    return table[rng.range(static_cast<std::uint32_t>(N))];
}

[[nodiscard]] std::string makeSystemName(core::Rng& rng)
{
    std::string name = pick(rng, kNamePrefixes);
    if (rng.nextFloat01() < 0.5f) {
        name += pick(rng, kNameMiddles);
    }
    name += pick(rng, kNameSuffixes);
    return name;
}

// Scatter systems in a thin disc with a minimum pairwise separation
// (best-effort: separation relaxes if rejection keeps failing, so the count
// is always honored).
void scatterSystems(const GalaxyParams& params, core::Rng& rng, std::vector<SystemSpec>& systems)
{
    const float radius = params.galaxyRadius;
    float separation = radius / std::sqrt(static_cast<float>(params.systemCount)) * 1.1f;
    constexpr float kTau = 6.28318530717958647692f;
    while (systems.size() < params.systemCount) {
        bool placed = false;
        for (std::uint32_t tries = 0; tries < 64 && !placed; ++tries) {
            const float r = radius * std::sqrt(rng.nextFloat01());
            const float theta = kTau * rng.nextFloat01();
            const core::Vec3 candidate{
                r * std::cos(theta), 0.08f * radius * (rng.nextFloat01() * 2.0f - 1.0f), r * std::sin(theta)};
            bool clear = true;
            for (const SystemSpec& other : systems) {
                if (core::length(candidate - other.mapPosition) < separation) {
                    clear = false;
                    break;
                }
            }
            if (clear) {
                SystemSpec spec;
                spec.mapPosition = candidate;
                systems.push_back(std::move(spec));
                placed = true;
            }
        }
        if (!placed) {
            separation *= 0.9f;
        }
    }
}

[[nodiscard]] float mapDistance(const SystemSpec& a, const SystemSpec& b)
{
    return core::length(a.mapPosition - b.mapPosition);
}

// Minimum spanning tree (Prim, O(n^2) — fine at <=150 systems) guarantees
// connectivity; then up to extraGatesPerSystem nearest-neighbor links per
// system add loops so the graph isn't a tree.
void buildGateGraph(const GalaxyParams& params,
                    std::vector<SystemSpec>& systems,
                    std::vector<GateLink>& links)
{
    const std::uint32_t count = static_cast<std::uint32_t>(systems.size());
    std::vector<bool> inTree(count, false);
    std::vector<float> bestCost(count, std::numeric_limits<float>::max());
    std::vector<std::uint32_t> bestFrom(count, 0);
    std::vector<std::vector<std::uint32_t>> adjacency(count);

    // ⚑⚑ LANES THAT ARE ALREADY HERE ARE ADOPTED RATHER THAN REDISCOVERED, AND
    // THAT IS THE WHOLE OF WHAT A CONSTELLATION COSTS THE GATE GRAPH (Phase 29
    // stage C). A constellation seeds its internal lanes into `links` before
    // this runs; reading them into the adjacency list is what makes the dedup
    // below see them, so Prim cannot add a second copy of one and the extra-lane
    // pass cannot either. An empty `links` - every caller before stage C - leaves
    // this loop doing nothing at all.
    for (const GateLink& seeded : links) {
        adjacency[seeded.a].push_back(seeded.b);
        adjacency[seeded.b].push_back(seeded.a);
    }

    const auto addLink = [&](std::uint32_t a, std::uint32_t b) {
        if (a > b) {
            std::swap(a, b);
        }
        for (const std::uint32_t neighbor : adjacency[a]) {
            if (neighbor == b) {
                return;
            }
        }
        adjacency[a].push_back(b);
        adjacency[b].push_back(a);
        links.push_back({a, b});
    };

    inTree[0] = true;
    for (std::uint32_t i = 1; i < count; ++i) {
        bestCost[i] = mapDistance(systems[0], systems[i]);
    }
    for (std::uint32_t added = 1; added < count; ++added) {
        std::uint32_t next = 0;
        float nextCost = std::numeric_limits<float>::max();
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!inTree[i] && bestCost[i] < nextCost) {
                nextCost = bestCost[i];
                next = i;
            }
        }
        inTree[next] = true;
        addLink(bestFrom[next], next);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!inTree[i]) {
                const float cost = mapDistance(systems[next], systems[i]);
                if (cost < bestCost[i]) {
                    bestCost[i] = cost;
                    bestFrom[i] = next;
                }
            }
        }
    }

    // Extra lanes: each system links to its nearest not-yet-linked neighbors,
    // capped by length so shortcuts stay local.
    const float maxExtraLength = params.galaxyRadius / std::sqrt(static_cast<float>(count)) * 3.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        for (std::uint32_t extra = 0; extra < params.extraGatesPerSystem; ++extra) {
            std::uint32_t nearest = kInvalidIndex;
            float nearestCost = maxExtraLength;
            for (std::uint32_t j = 0; j < count; ++j) {
                if (j == i) {
                    continue;
                }
                const bool linked =
                    std::find(adjacency[i].begin(), adjacency[i].end(), j) != adjacency[i].end();
                if (linked) {
                    continue;
                }
                const float cost = mapDistance(systems[i], systems[j]);
                if (cost < nearestCost) {
                    nearestCost = cost;
                    nearest = j;
                }
            }
            if (nearest != kInvalidIndex) {
                addLink(i, nearest);
            }
        }
    }
}

void assignRegions(const GalaxyParams& params, std::vector<SystemSpec>& systems)
{
    const float coreRadius = params.galaxyRadius * params.coreRadiusFraction;
    const float frontierRadius = params.galaxyRadius * params.frontierRadiusFraction;
    for (SystemSpec& system : systems) {
        const float r = core::length(system.mapPosition);
        system.region = r < coreRadius       ? Region::Core
                        : r < frontierRadius ? Region::Frontier
                                             : Region::Fringe;
    }
}

// ---------------------------------------------------------------------------
// Authored systems (Phase 29 stage A).
//
// ⚑⚑⚑⚑ THIS RUNS *AFTER* buildGateGraph, AND THAT IS FORCED BY THE PIPELINE
// RATHER THAN CHOSEN. decisions/018 sketched placement as resolving "before the
// procedural pass", but three of its four rules do not create a node at all:
// `at_system`, `jumps_from` and `random` all REPLACE an existing one, and
// `jumps_from` measures a gate graph that does not exist until `buildGateGraph`
// has run over final positions. Only `anywhere` (stage B) is an insertion.
//
// ⚑⚑ A REPLACEMENT IS NOT THE "POST-GENERATION PATCHING" 018 REFUSED. That
// alternative was refused because patching "produces gate graphs that
// contradict the authored layout - the generator has already decided the
// neighbours". An authored system declares no external links, so it contradicts
// nothing: it inherits the node's map position, its region and its gates, and
// the galaxy's shape is untouched. What stays forbidden is patching after
// `populateSystem`, which is a different point and is not this.
//
// ⚑ The cost of replacing is stated rather than hidden: a `random` system
// consumes an ordinary system's slot, so the galaxy does not grow. Growing it
// is what `anywhere` means, and that is stage B.
//
// ⚑⚑⚑ AND THE SPEC'S "FOUR SKIP POINTS, ONE PER STAGE" TURNED OUT TO BE THE
// WRONG SHAPE - THE COUNT WAS CLOSE AND THE PLACES WERE NOT. Two of the four
// stages it named need no guard at all for a REPLACEMENT, because the authored
// write lands after them: `assignRegions` runs before this function, and the
// name loop is left strictly alone and overwritten afterwards (guarding it
// changes how many times its uniqueness scan redraws, which renames the
// galaxy). The other two need FIVE guards between them rather than two:
// `claimTerritory` writes `factionIndex` at three separate points - the capital
// seed, the Dijkstra propagation and the lawless re-roll - and `spawnClans`
// writes it at two. Counting STAGES undercounts, because what has to be guarded
// is a WRITE.
// ---------------------------------------------------------------------------
// PASS 1 of placement (Phase 29 stage B): `anywhere` appends a node.
//
// ⚑⚑⚑ THIS RUNS BEFORE `assignRegions` AND BEFORE `buildGateGraph`, WHICH IS
// THE WHOLE POINT OF IT BEING A SEPARATE PASS. An appended node has to be in
// the vector before Prim runs or it would have no gates at all, and it has to
// be there before regions are assigned or it would have no region. Both are
// exactly what "gated in like any other node" means.
//
// ⚑⚑⚑ APPENDING AT THE END IS WHAT KEEPS THE PROCEDURAL GALAXY STILL. Three
// properties fall out of it and none of them survives pre-seeding:
//   - the procedural scatter is bit-identical, because this draws from the
//     AUTHORED stream and runs after `scatterSystems` has finished rejecting;
//   - every procedural system index is unmoved, so an `anywhere` system is
//     index `systemCount + k` and nothing that already existed shifted;
//   - the NAME stream is untouched for every procedural system. The name loop
//     runs over the grown vector, but the first `systemCount` draws - and the
//     uniqueness rescan behind them, which only ever looks at j < i - see
//     exactly what they saw before. The extra draws all land at the END.
//
// ⚑⚑ AND THE FIELDS AN AUTHOR WROTE ARE NOT APPLIED HERE. Only the position
// is. Identity, region, owner and the rest are applied in pass 2, which runs
// after `assignRegions` - so an `anywhere` system needs NO skip point in that
// pass, which is what stage A's own forward-looking comment predicted it would.
// One field-application pass, two ways of choosing an index.
void appendAnywhereSystems(const GalaxyParams& params, core::Rng& rng, std::vector<SystemSpec>& systems)
{
    const float radius = params.galaxyRadius;
    // The same separation rule `scatterSystems` uses, and best-effort for the
    // same documented reason. An authored node may end up closer to a
    // neighbour than the procedural rule would have allowed; choosing
    // `anywhere` is a statement about wanting a new place, not about wanting a
    // particular amount of elbow room.
    float separation = radius / std::sqrt(static_cast<float>(params.systemCount)) * 1.1f;
    constexpr float kTau = 6.28318530717958647692f;
    for (const AuthoredSystem& authored : params.authoredSystems) {
        if (authored.placement != Placement::Anywhere) {
            continue;
        }
        bool placed = false;
        while (!placed) {
            for (std::uint32_t tries = 0; tries < 64 && !placed; ++tries) {
                const float r = radius * std::sqrt(rng.nextFloat01());
                const float theta = kTau * rng.nextFloat01();
                const core::Vec3 candidate{r * std::cos(theta),
                                           0.08f * radius * (rng.nextFloat01() * 2.0f - 1.0f),
                                           r * std::sin(theta)};
                bool clear = true;
                for (const SystemSpec& other : systems) {
                    if (core::length(candidate - other.mapPosition) < separation) {
                        clear = false;
                        break;
                    }
                }
                if (clear) {
                    SystemSpec spec;
                    spec.mapPosition = candidate;
                    systems.push_back(std::move(spec));
                    placed = true;
                }
            }
            if (!placed) {
                separation *= 0.9f;
            }
        }
    }
}

// PASS 1, second half (Phase 29 stage C): a constellation appends its members
// together and hands the gate graph the lanes their author drew between them.
//
// ⚑⚑⚑⚑ THE INTERNAL LANES ARE SEEDED HERE, BEFORE `buildGateGraph`, AND THAT
// ORDERING IS THE ENTIRE FEATURE. decisions/018 says a constellation is "placed
// as a unit with its internal topology intact", and the only moment at which
// that topology can be stated is before Prim decides what the neighbours are.
// Seeded after, it would be exactly the post-generation patching 018 refused -
// lanes contradicting a graph that had already been settled. `addLink` dedups
// against the adjacency list, so a lane the MST would have drawn anyway is
// adopted rather than duplicated, and Prim does not mind an already-connected
// component.
//
// ⚑⚑ MEMBERS ARE APPENDED AFTER EVERY `anywhere` SYSTEM, WHICH IS WHAT KEEPS
// THE INDEX ARITHMETIC SAYABLE. `anywhere` systems are `proceduralCount + k`;
// a constellation's members follow them, contiguously, in def order. Nothing
// the seed produced moves, and the name stream is untouched for exactly the
// reason stage B wrote down: the extra draws all land after the procedural ones
// and the uniqueness rescan only ever looks backwards.
//
// ⚑ A CONSTELLATION CANNOT FAIL. It creates its own nodes, so there is no
// "nowhere to go" for it to report - which is also why a member is always
// available as a `jumps_from` anchor, whatever def order the file used.
void appendConstellations(const GalaxyParams& params,
                          core::Rng& rng,
                          std::vector<SystemSpec>& systems,
                          std::vector<GateLink>& links)
{
    const float radius = params.galaxyRadius;
    const float separation = radius / std::sqrt(static_cast<float>(params.systemCount)) * 1.1f;
    // A constellation has to READ as a cluster or it is only a list, so its
    // members sit closer together than the procedural scatter would ever put
    // them - and the group as a whole asks for more elbow room than one system
    // does, so it lands beside the galaxy's existing neighbourhoods rather than
    // through the middle of one.
    const float clusterRadius = separation * 0.9f;
    constexpr float kTau = 6.28318530717958647692f;

    const auto seedLink = [&links](std::uint32_t a, std::uint32_t b) {
        if (a == b) {
            return;
        }
        if (a > b) {
            std::swap(a, b);
        }
        for (const GateLink& existing : links) {
            if (existing.a == a && existing.b == b) {
                return; // an author wrote the same lane twice; one lane is one lane
            }
        }
        links.push_back({a, b});
    };

    for (const AuthoredConstellation& constellation : params.constellations) {
        if (constellation.members.empty()) {
            continue; // the def layer refuses this; the generator does not assume it
        }
        const std::uint32_t base = static_cast<std::uint32_t>(systems.size());

        // The group's centre, rejected against the whole galaxy so the cluster
        // has room for itself. Best-effort and relaxing, exactly as
        // `scatterSystems` is and for the same reason: the count is honoured.
        core::Vec3 centre{};
        float clearance = separation + clusterRadius;
        bool centred = false;
        while (!centred) {
            for (std::uint32_t tries = 0; tries < 64 && !centred; ++tries) {
                const float r = radius * std::sqrt(rng.nextFloat01());
                const float theta = kTau * rng.nextFloat01();
                const core::Vec3 candidate{r * std::cos(theta),
                                           0.08f * radius * (rng.nextFloat01() * 2.0f - 1.0f),
                                           r * std::sin(theta)};
                bool clear = true;
                for (const SystemSpec& other : systems) {
                    if (core::length(candidate - other.mapPosition) < clearance) {
                        clear = false;
                        break;
                    }
                }
                if (clear) {
                    centre = candidate;
                    centred = true;
                }
            }
            if (!centred) {
                clearance *= 0.9f;
            }
        }

        float memberSeparation = separation * 0.4f;
        for (std::size_t member = 0; member < constellation.members.size(); ++member) {
            bool placed = false;
            while (!placed) {
                for (std::uint32_t tries = 0; tries < 64 && !placed; ++tries) {
                    const float r = clusterRadius * std::sqrt(rng.nextFloat01());
                    const float theta = kTau * rng.nextFloat01();
                    const core::Vec3 candidate{centre.x + r * std::cos(theta),
                                               centre.y +
                                                   0.25f * clusterRadius * (rng.nextFloat01() * 2.0f - 1.0f),
                                               centre.z + r * std::sin(theta)};
                    bool clear = true;
                    for (const SystemSpec& other : systems) {
                        if (core::length(candidate - other.mapPosition) < memberSeparation) {
                            clear = false;
                            break;
                        }
                    }
                    if (clear) {
                        SystemSpec spec;
                        spec.mapPosition = candidate;
                        systems.push_back(std::move(spec));
                        placed = true;
                    }
                }
                if (!placed) {
                    memberSeparation *= 0.9f;
                }
            }
        }

        const std::uint32_t memberCount = static_cast<std::uint32_t>(constellation.members.size());
        if (constellation.links.empty()) {
            // ⚑ NO LANES WRITTEN MEANS A CHAIN, NOT AN ABSENCE. A group whose
            // members have no lanes between them is not a group - Prim would
            // wire each member to whatever happened to be nearest and the
            // author's "unit" would be three unrelated systems that happen to
            // sit close together. Declaration order is the only order there is,
            // so that is the chain.
            for (std::uint32_t k = 1; k < memberCount; ++k) {
                seedLink(base + k - 1, base + k);
            }
        } else {
            for (const AuthoredConstellationLink& link : constellation.links) {
                if (link.a >= memberCount || link.b >= memberCount) {
                    continue; // the def layer refuses this; the generator does not index on trust
                }
                seedLink(base + link.a, base + link.b);
            }
        }
    }
}

// Faction capitals: greedy farthest-point spread through the core.
//
// ⚑⚑⚑⚑ THIS WAS LIFTED OUT OF `claimTerritory` SO THAT `at_system` CAN NAME
// ONE, AND THE LIFT IS WHAT MAKES THE RULE BUILDABLE AT ALL. `claimTerritory`
// runs AFTER placement and is what CHOOSES the capitals, so before this move
// there was no moment at which "the Navy's home" was a thing a placement rule
// could point at.
//
// ⚑⚑⚑ THE FACTION STREAM IS UNDISTURBED, WHICH IS THE ONLY REASON THIS IS
// SAFE. This makes exactly one draw - the first capital - and it is still the
// first draw taken from `kStreamFactions`; `claimTerritory` keeps the same
// generator and takes the lawless rolls from it afterwards, in the same order.
// The golden is what holds that claim rather than this comment.
//
// ⚑⚑ TWO BEHAVIOUR CHANGES ARE REAL AND BOTH ARE DELIBERATE, AND NEITHER CAN
// BE SEEN BY A GALAXY WITH NO AUTHORED SYSTEMS IN IT:
//   - Candidacy reads the PROCEDURAL region assignment, because this now runs
//     before an authored `region` is applied. An authored system declaring
//     itself core therefore does not add itself to the pool of places a
//     faction might be capital of, which is the more predictable of the two
//     readings: who the galaxy's powers are is a fact about the seed.
//   - An APPENDED node is not a candidate. A place somebody put somewhere is
//     not a candidate to be silently promoted to a faction's homeworld - and
//     it is also what keeps `at_system` resolvable, since a capital that was
//     itself authored would already be occupied when `at_system` reached it.
[[nodiscard]] std::vector<std::uint32_t> chooseCapitals(const GalaxyParams& params,
                                                        core::Rng& rng,
                                                        const std::vector<SystemSpec>& systems,
                                                        std::uint32_t proceduralCount)
{
    std::vector<std::uint32_t> capitals;
    if (params.factionCount == 0) {
        return capitals;
    }
    std::vector<std::uint32_t> candidates;
    for (std::uint32_t i = 0; i < proceduralCount; ++i) {
        if (systems[i].region == Region::Core) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) { // degenerate params: claim from anywhere
        for (std::uint32_t i = 0; i < proceduralCount; ++i) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) {
        return capitals;
    }

    capitals.push_back(candidates[rng.range(static_cast<std::uint32_t>(candidates.size()))]);
    while (capitals.size() < params.factionCount && capitals.size() < candidates.size()) {
        std::uint32_t farthest = candidates[0];
        float farthestCost = -1.0f;
        for (const std::uint32_t candidate : candidates) {
            float nearestCapital = std::numeric_limits<float>::max();
            for (const std::uint32_t capital : capitals) {
                nearestCapital = std::min(nearestCapital, mapDistance(systems[candidate], systems[capital]));
            }
            if (nearestCapital > farthestCost) {
                farthestCost = nearestCapital;
                farthest = candidate;
            }
        }
        if (farthestCost <= 0.0f) {
            break; // candidate already a capital: core smaller than factionCount
        }
        capitals.push_back(farthest);
    }
    return capitals;
}

// The one place an author's fields are written onto a node, shared by both
// ways of getting one (Phase 29 stage C). A `[[system]]` chose its node with a
// rule; a constellation member was handed one by its group - and from here on
// there is no difference between them, which is what keeps the skip points at
// five rather than at ten.
void applyAuthoredFields(const AuthoredSystem& authored,
                         std::uint32_t index,
                         std::vector<SystemSpec>& systems,
                         std::vector<const AuthoredSystem*>& authoredFor)
{
    SystemSpec& system = systems[index];
    system.authoredId = authored.id;
    system.secret = authored.secret;
    // ⚑ The NAME is deliberately not written here. It is applied after the
    // procedural name loop has run untouched, because how many times that
    // loop redraws depends on which names it can already see - so writing
    // one in ahead of it changes what every other system is called.
    // ⚑⚑ The REGION is written here for every rule, replacement and
    // insertion alike, and that is what saves `assignRegions` from needing
    // a skip point: this pass runs after it, so this is the last word on
    // the field no matter which pass created the node.
    if (authored.hasRegion) {
        system.region = authored.region;
    }
    if (authored.hasFaction) {
        system.factionIndex = authored.factionIndex;
    }
    authoredFor[index] = &authored;
}

// PASS 2, first half (Phase 29 stage C): a constellation's members take the
// nodes pass 1 already made for them, in the same order it made them.
//
// ⚑⚑ THIS RUNS BEFORE THE RULES DO, AND THAT IS WHAT MAKES A MEMBER A LEGAL
// `jumps_from` ANCHOR REGARDLESS OF DEF ORDER. A constellation cannot fail to
// be placed - it creates its own nodes - so there is no ordering in which a
// member is "not placed yet", and pretending otherwise would be a refusal an
// author could not act on.
//
// Returns where each member landed, which seeds the anchor list the placement
// rules then extend.
[[nodiscard]] std::vector<std::pair<std::string, std::uint32_t>>
applyConstellations(const GalaxyParams& params,
                    std::uint32_t firstIndex,
                    std::vector<SystemSpec>& systems,
                    std::vector<const AuthoredSystem*>& authoredFor)
{
    std::vector<std::pair<std::string, std::uint32_t>> placedById;
    std::uint32_t index = firstIndex;
    for (const AuthoredConstellation& constellation : params.constellations) {
        for (const AuthoredSystem& member : constellation.members) {
            applyAuthoredFields(member, index, systems, authoredFor);
            placedById.emplace_back(member.id, index);
            ++index;
        }
    }
    return placedById;
}

// PASS 2, second half: every rule chooses an index, then the shared block above
// applies the fields the author wrote.
void placeAuthoredSystems(const GalaxyParams& params,
                          core::Rng& rng,
                          const Galaxy& galaxy,
                          const std::vector<std::uint32_t>& capitals,
                          std::uint32_t proceduralCount,
                          std::vector<SystemSpec>& systems,
                          std::vector<const AuthoredSystem*>& authoredFor,
                          std::vector<std::pair<std::string, std::uint32_t>>& placedById,
                          std::vector<AuthoredPlacementFailure>* outFailures)
{
    // Every node the seed produced is a candidate until an earlier authored
    // system has taken it; def order decides who chooses first (decision 4).
    // ⚑ APPENDED nodes are NOT in this list. An `anywhere` system already owns
    // the node it created, so a later `random` cannot be handed it.
    std::vector<std::uint32_t> free;
    free.reserve(proceduralCount);
    for (std::uint32_t i = 0; i < proceduralCount; ++i) {
        free.push_back(i);
    }
    // `placedById` arrives holding every constellation member and grows with
    // each `[[system]]` this loop places, so `jumps_from` can anchor on either.
    // Only successfully placed systems are ever in it, which is what makes a
    // ring anchored on a FAILED system fail by name too rather than silently
    // anchoring on nothing.

    const auto fail = [&](const AuthoredSystem& authored, const char* rule, std::string reason) {
        if (outFailures != nullptr) {
            outFailures->push_back({authored.id, rule, std::move(reason)});
        }
    };

    // `anywhere` nodes were appended in def order, so the k-th one is at
    // `proceduralCount + k`.
    std::uint32_t nextAppended = proceduralCount;

    for (const AuthoredSystem& authored : params.authoredSystems) {
        std::uint32_t index = kInvalidIndex;
        switch (authored.placement) {
        case Placement::Anywhere: {
            // Pass 1 already made the node and it cannot fail: the separation
            // rule relaxes until a position is accepted, exactly as the
            // procedural scatter's does.
            index = nextAppended++;
            break;
        }
        case Placement::Random: {
            if (free.empty()) {
                fail(authored, "random", "the galaxy has no unclaimed system left to become");
                break;
            }
            const std::uint32_t slot = rng.range(static_cast<std::uint32_t>(free.size()));
            index = free[slot];
            free.erase(free.begin() + slot);
            break;
        }
        case Placement::AtSystem: {
            if (authored.atFactionCapital >= capitals.size()) {
                // Reached when a mod removes the faction whose capital this
                // names, or when the core was too small for it to get one.
                fail(authored, "at_system", "the faction it names holds no capital in this galaxy");
                break;
            }
            const std::uint32_t capital = capitals[authored.atFactionCapital];
            const auto slot = std::find(free.begin(), free.end(), capital);
            if (slot == free.end()) {
                fail(authored,
                     "at_system",
                     "that capital was already taken by an authored system declared earlier");
                break;
            }
            index = capital;
            free.erase(slot);
            break;
        }
        case Placement::JumpsFrom: {
            std::uint32_t anchor = kInvalidIndex;
            for (const auto& [id, at] : placedById) {
                if (id == authored.anchorId) {
                    anchor = at;
                    break;
                }
            }
            if (anchor == kInvalidIndex) {
                fail(authored,
                     "jumps_from",
                     "'" + authored.anchorId +
                         "' is not an authored system placed before this one; an anchor must be "
                         "declared earlier in the file");
                break;
            }
            // ⚑⚑ `routeBetween` MEASURES THIS, WHICH IS ONLY TRUE BECAUSE THE
            // GATE LISTS ARE FILLED BEFORE PLACEMENT RUNS. It walks
            // `SystemSpec::gates`, and until stage B those were populated
            // fifty lines further down - so the primitive decisions/018 named
            // was real but unreachable at the moment it was needed.
            std::vector<std::uint32_t> ring;
            for (const std::uint32_t candidate : free) {
                const std::vector<std::uint32_t> route = routeBetween(galaxy, anchor, candidate);
                if (route.empty()) {
                    continue; // unreachable; cannot happen for a connected graph
                }
                const std::uint32_t jumps = static_cast<std::uint32_t>(route.size()) - 1;
                if (jumps >= authored.jumpsMin && jumps <= authored.jumpsMax) {
                    ring.push_back(candidate);
                }
            }
            if (ring.empty()) {
                fail(authored,
                     "jumps_from",
                     "no unclaimed system is between " + std::to_string(authored.jumpsMin) + " and " +
                         std::to_string(authored.jumpsMax) + " jumps from '" + authored.anchorId + "'");
                break;
            }
            index = ring[rng.range(static_cast<std::uint32_t>(ring.size()))];
            free.erase(std::find(free.begin(), free.end(), index));
            break;
        }
        }
        if (index == kInvalidIndex) {
            continue; // refused above, and named there
        }

        applyAuthoredFields(authored, index, systems, authoredFor);
        placedById.emplace_back(authored.id, index);
    }
}

// Did an author state who owns this system - including by stating that nobody
// does? The two are the same bit pattern in `factionIndex`, which is exactly
// why this asks the flag instead.
[[nodiscard]] bool hasAuthoredFaction(const std::vector<const AuthoredSystem*>& authoredFor,
                                      std::size_t index)
{
    return authoredFor[index] != nullptr && authoredFor[index]->hasFaction;
}

// Faction capitals spread greedily through the core (farthest-point), then
// multi-source Dijkstra over the gate graph claims territory; fringe systems
// roll to stay lawless.
void claimTerritory(const GalaxyParams& params,
                    core::Rng& rng,
                    std::vector<SystemSpec>& systems,
                    const std::vector<GateLink>& links,
                    const std::vector<const AuthoredSystem*>& authoredFor,
                    const std::vector<std::uint32_t>& capitals)
{
    if (params.factionCount == 0 || capitals.empty()) {
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(systems.size());

    std::vector<std::vector<std::uint32_t>> adjacency(count);
    for (const GateLink& link : links) {
        adjacency[link.a].push_back(link.b);
        adjacency[link.b].push_back(link.a);
    }

    // (distance, system) min-heap; ties break on system index for determinism.
    using Entry = std::pair<float, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    std::vector<float> bestCost(count, std::numeric_limits<float>::max());
    for (std::uint32_t f = 0; f < capitals.size(); ++f) {
        const std::uint32_t capital = capitals[f];
        // ⚑ GUARD 1 OF 5. An authored owner outranks a capital: the
        // capital still seeds the search from here, so territory still spreads
        // outward, but the author's system keeps the flag the author wrote on
        // it rather than becoming somebody's home by geometry.
        if (!hasAuthoredFaction(authoredFor, capital)) {
            systems[capital].factionIndex = f;
        }
        bestCost[capital] = 0.0f;
        queue.push({0.0f, capital});
    }
    while (!queue.empty()) {
        const auto [cost, index] = queue.top();
        queue.pop();
        if (cost > bestCost[index]) {
            continue;
        }
        for (const std::uint32_t neighbor : adjacency[index]) {
            const float next = cost + mapDistance(systems[index], systems[neighbor]);
            if (next < bestCost[neighbor]) {
                bestCost[neighbor] = next;
                // ⚑ GUARD 2 OF 5. The search still walks THROUGH an
                // authored system - its neighbours are claimed normally - it
                // just does not repaint it on the way past.
                if (!hasAuthoredFaction(authoredFor, neighbor)) {
                    systems[neighbor].factionIndex = systems[index].factionIndex;
                }
                queue.push({next, neighbor});
            }
        }
    }

    // Lawless fringe: one roll per system, in index order.
    //
    // ⚑ GUARD 3 OF 5, and note the roll still HAPPENS for an authored
    // system - only its result is discarded. Skipping the draw itself would
    // shift every later system's roll and reshape the galaxy around the
    // authored one, which is the opposite of what a replacement is for.
    for (std::uint32_t i = 0; i < systems.size(); ++i) {
        SystemSpec& system = systems[i];
        if (system.region == Region::Fringe && rng.nextFloat01() < params.fringeLawlessChance &&
            !hasAuthoredFaction(authoredFor, i)) {
            system.factionIndex = kNoFaction;
        }
    }
}

// Pirate clans (Phase 8b): every connected component of still-lawless
// systems becomes one generated clan; its members' factionIndex continues
// past the majors, so downstream tables (relations, reputation, catalogs)
// are just sized to factionCount + clans.size(). Component walk is in index
// order and all draws come from one dedicated stream => deterministic.
void spawnClans(const GalaxyParams& params,
                core::Rng& rng,
                std::vector<SystemSpec>& systems,
                const std::vector<GateLink>& links,
                const std::vector<const AuthoredSystem*>& authoredFor,
                std::vector<ClanSpec>& clans)
{
    if (params.factionCount == 0 || params.pirateTemplateCount == 0) {
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(systems.size());
    std::vector<std::vector<std::uint32_t>> adjacency(count);
    for (const GateLink& link : links) {
        adjacency[link.a].push_back(link.b);
        adjacency[link.b].push_back(link.a);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        // ⚑ GUARDS 4 AND 5 OF 5 (this one and the neighbour sweep below), and
        // this is the one a sentinel could never have
        // expressed. `factionIndex == kNoFaction` is BOTH "nobody has decided"
        // and "the author decided nobody owns this", and only the second must
        // survive a clan sweeping through. A pirate clan holding a system IS an
        // owner, so an authored lawless system splits a lawless neighbourhood
        // into two components rather than joining one - which is the author
        // getting what they asked for, not an accident.
        if (systems[i].factionIndex != kNoFaction || hasAuthoredFaction(authoredFor, i)) {
            continue;
        }
        const std::uint32_t clanFaction = params.factionCount + static_cast<std::uint32_t>(clans.size());
        std::vector<std::uint32_t> frontier{i};
        systems[i].factionIndex = clanFaction;
        while (!frontier.empty()) {
            const std::uint32_t index = frontier.back();
            frontier.pop_back();
            for (const std::uint32_t neighbor : adjacency[index]) {
                if (systems[neighbor].factionIndex == kNoFaction &&
                    !hasAuthoredFaction(authoredFor, neighbor)) {
                    systems[neighbor].factionIndex = clanFaction;
                    frontier.push_back(neighbor);
                }
            }
        }
        clans.push_back({
            .name = systems[i].name + " " + pick(rng, kClanSuffixes),
            .templateIndex = rng.range(params.pirateTemplateCount),
            .seed = rng.nextU64(),
            .homeSystem = i,
        });
    }
}

// The weight one archetype carries in one system: its region tuning, vetoed to
// zero when it needs rock the system does not have (Phase 13).
//
// ⚑ A veto, not a re-roll. Zeroing the weight and rolling over what is left
// means a rockless system builds something ELSE rather than building nothing,
// so no system loses a station it would have had — only the outpost that could
// never have produced anything moves aside for one that can.
// What the faction holding this system does to one archetype's odds
// (Phase 13). Anything unstated — no bias table, a faction with no character,
// a lawless system, a row that does not reach this archetype — is 1.0, which
// is the pre-Phase-13 galaxy exactly.
[[nodiscard]] float factionBias(const GalaxyParams& params, std::uint32_t faction, std::uint32_t archetype)
{
    if (faction >= params.factionStationBias.size()) {
        return 1.0f;
    }
    const std::vector<float>& row = params.factionStationBias[faction];
    return archetype < row.size() ? row[archetype] : 1.0f;
}

// The weight one archetype carries in one system: its region tuning, tilted by
// its owner's character, vetoed to zero when it needs rock the system lacks.
[[nodiscard]] float archetypeWeight(const GalaxyParams& params,
                                    std::uint32_t archetype,
                                    std::size_t tier,
                                    std::uint32_t faction,
                                    std::uint32_t systemFieldCount,
                                    bool enforceFields)
{
    const StationRule& rule = params.stationRules[archetype];
    if (enforceFields && rule.requiresField && systemFieldCount == 0) {
        return 0.0f;
    }
    // ⚑ Multiply, never replace: the region tuning decides the baseline (ore in
    // the fringe, factories in the core) and the faction only tilts it, so a
    // character cannot quietly undo the economy's geography.
    return rule.weight[tier] * factionBias(params, faction, archetype);
}

[[nodiscard]] std::uint32_t pickArchetype(const GalaxyParams& params,
                                          core::Rng& rng,
                                          Region region,
                                          std::uint32_t faction,
                                          std::uint32_t systemFieldCount,
                                          bool enforceFields)
{
    if (params.stationRules.empty()) {
        return 0;
    }
    const std::uint32_t ruleCount = static_cast<std::uint32_t>(params.stationRules.size());
    const std::size_t tier = static_cast<std::size_t>(region);
    float total = 0.0f;
    for (std::uint32_t i = 0; i < ruleCount; ++i) {
        total += archetypeWeight(params, i, tier, faction, systemFieldCount, enforceFields);
    }
    // Every candidate vetoed — by the rock rule, or by a faction that zeroed
    // everything it could build here. Fall back to the unfiltered roll rather
    // than refusing to build: a system with no stations at all is a worse
    // world than an out-of-character one, and a mod can produce this.
    if (total <= 0.0f) {
        return enforceFields || faction != kNoFaction
                   ? pickArchetype(params, rng, region, kNoFaction, 0, false)
                   : 0;
    }
    float roll = rng.nextFloat01() * total;
    for (std::uint32_t i = 0; i < ruleCount; ++i) {
        roll -= archetypeWeight(params, i, tier, faction, systemFieldCount, enforceFields);
        if (roll <= 0.0f) {
            return i;
        }
    }
    return ruleCount - 1;
}

// Star, planets (AU-scale scenery; primary planet hosts the playfield),
// stations near the primary planet, gates toward each linked neighbor.
void populateSystem(const GalaxyParams& params,
                    std::uint32_t index,
                    SystemSpec& system,
                    const std::vector<SystemSpec>& systems,
                    core::Rng& rng,
                    const AuthoredSystem* authored,
                    const MiningParams* mining)
{
    // Asked once per system, before any station is placed. A field is a pure
    // function of the system's seed and region and never depends on what was
    // built there, so this is a read of content that already exists rather
    // than a decision that has to be sequenced against station placement.
    const std::uint32_t fieldCount = mining != nullptr ? fieldCountFor(system, *mining) : 0;
    const bool enforceFields = mining != nullptr;

    system.starRadius = 7.0e8 * (0.6 + 0.9 * rng.nextDouble01());

    // ⚑⚑ AN AUTHOR NAMES PLANETS; THE GENERATOR STILL LAYS OUT THE ORBITS.
    // Writing a system by hand must not mean writing coordinates in metres, so
    // an authored planet supplies a name (and optionally a radius) and takes
    // the same orbit ladder a rolled one would have had. Authoring NO planets
    // is not the same as authoring zero of them: it means "roll them", which
    // is what keeps `spec.planets[spec.primaryPlanet]` legal at the six
    // call sites that index it with no guard.
    const bool authoredPlanets = authored != nullptr && !authored->planets.empty();
    const std::uint32_t planetCount =
        authoredPlanets ? static_cast<std::uint32_t>(authored->planets.size()) : 1 + rng.range(4);
    double orbit = 4.0e10 * (1.0 + rng.nextDouble01()); // innermost 0.27-0.53 AU
    for (std::uint32_t p = 0; p < planetCount; ++p) {
        PlanetSpec planet;
        planet.name =
            system.name + " " + kPlanetNumerals[std::min<std::size_t>(p, std::size(kPlanetNumerals) - 1)];
        planet.radius = 2.5e6 + 4.5e7 * rng.nextDouble01();
        planet.position = randomPlayfieldDirection(rng) * orbit;
        orbit *= 1.6 + 0.8 * rng.nextDouble01();
        if (authoredPlanets) {
            const AuthoredPlanet& row = authored->planets[p];
            planet.name = row.name;
            if (row.hasRadius) {
                planet.radius = row.radius;
            }
        }
        system.planets.push_back(std::move(planet));
    }
    // Hub: middle planet, so inner/outer scenery flanks the playfield.
    system.primaryPlanet = planetCount / 2;
    if (authored != nullptr && authored->hasPrimaryPlanet && authored->primaryPlanet < planetCount) {
        system.primaryPlanet = authored->primaryPlanet;
    }
    const core::DVec3 hub = system.planets[system.primaryPlanet].position;

    const std::size_t tier = static_cast<std::size_t>(system.region);
    const std::uint32_t minStations = params.stationCount[tier][0];
    const std::uint32_t maxStations = params.stationCount[tier][1];
    const bool authoredStations = authored != nullptr && !authored->stations.empty();
    const std::uint32_t stationCount =
        authoredStations
            ? static_cast<std::uint32_t>(authored->stations.size())
            : minStations + (maxStations > minStations ? rng.range(maxStations - minStations + 1) : 0);
    for (std::uint32_t s = 0; s < stationCount; ++s) {
        StationSpec station;
        station.archetype =
            pickArchetype(params, rng, system.region, system.factionIndex, fieldCount, enforceFields);
        station.name =
            system.name + " " + kStationOrdinals[std::min<std::size_t>(s, std::size(kStationOrdinals) - 1)];
        if (authoredStations) {
            // The archetype was resolved from a def id at the seam, the same
            // way `factionStationBias` already resolves one; `sol::sim`
            // never learns what a def is. The roll above still happened, so the
            // stream stays in step with a rolled system's.
            station.name = authored->stations[s].name;
            station.archetype = authored->stations[s].archetype;
        }
        const double distance = params.stationMinDistance +
                                (params.stationMaxDistance - params.stationMinDistance) * rng.nextDouble01();
        station.position = hub + randomPlayfieldDirection(rng) * distance;
        system.stations.push_back(std::move(station));
    }

    // Gates sit at the playfield edge in the map-space direction of their
    // destination, so the system layout reads like the galaxy map.
    for (GateSpec& gate : system.gates) {
        const core::Vec3 toNeighbor = systems[gate.toSystem].mapPosition - systems[index].mapPosition;
        core::DVec3 direction{toNeighbor.x, toNeighbor.y, toNeighbor.z};
        const double len = core::length(direction);
        direction = len > 0.0 ? direction * (1.0 / len) : core::DVec3{1.0, 0.0, 0.0};
        gate.position = hub + direction * params.gateDistance;
    }
}

} // namespace

core::DVec3 randomPlayfieldDirection(core::Rng& rng)
{
    constexpr double kTau = 6.283185307179586476925;
    const double theta = kTau * rng.nextDouble01();
    const double y = 0.25 * (rng.nextDouble01() * 2.0 - 1.0);
    return core::normalize(core::DVec3{std::cos(theta), y, std::sin(theta)});
}

core::DVec3 playfieldHub(const SystemSpec& spec)
{
    if (spec.planets.empty()) {
        return {};
    }
    const std::size_t index = std::min<std::size_t>(spec.primaryPlanet, spec.planets.size() - 1);
    return spec.planets[index].position;
}

Galaxy generateGalaxy(const GalaxyParams& params,
                      const MiningParams* mining,
                      std::vector<AuthoredPlacementFailure>* outFailures)
{
    SOL_ASSERT(params.systemCount >= 2);
    if (outFailures != nullptr) {
        outFailures->clear();
    }
    Galaxy galaxy;
    galaxy.seed = params.seed;
    galaxy.systems.reserve(params.systemCount);

    core::Rng positionRng(params.seed, kStreamPositions);
    scatterSystems(params, positionRng, galaxy.systems);

    // ⚑ How many systems the SEED produced. Everything that has to keep its
    // pre-Phase-29 meaning is expressed against this rather than against
    // `galaxy.systems.size()`, which `anywhere` grows.
    const std::uint32_t proceduralCount = static_cast<std::uint32_t>(galaxy.systems.size());

    // PASS 1: `anywhere` appends its nodes and then each constellation appends
    // its members, before regions and before the gate graph, so they are laid
    // out and woven in like any other system. Constellations come SECOND so
    // that an `anywhere` system keeps the index arithmetic stage B wrote down -
    // `proceduralCount + k` - whether or not the file also has a group in it.
    core::Rng authoredRng(params.seed, kStreamAuthored);
    appendAnywhereSystems(params, authoredRng, galaxy.systems);
    const std::uint32_t firstConstellationIndex = static_cast<std::uint32_t>(galaxy.systems.size());
    appendConstellations(params, authoredRng, galaxy.systems, galaxy.links);

    assignRegions(params, galaxy.systems);
    buildGateGraph(params, galaxy.systems, galaxy.links);

    // ⚑⚑⚑ THE GATE LISTS ARE FILLED HERE, AND MOVING THEM UP IS WHAT MAKES
    // `jumps_from` MEASURABLE. `routeBetween` walks `SystemSpec::gates`, not
    // `galaxy.links`, and until stage B this loop ran fifty lines further down
    // - after `spawnClans` - so at the moment placement needed to measure a
    // ring, every gate list in the galaxy was still empty. The move is pure
    // data with no draw in it, and nothing between here and where it used to
    // sit reads `gates`: `claimTerritory` and `spawnClans` both walk `links`,
    // and `populateSystem` (which fills in each gate's POSITION) still runs
    // last.
    for (const GateLink& link : galaxy.links) {
        galaxy.systems[link.a].gates.push_back({link.b, {}});
        galaxy.systems[link.b].gates.push_back({link.a, {}});
    }

    // Capitals are chosen before placement so that `at_system` has something
    // to name. This takes the first draw from the faction stream and
    // `claimTerritory` takes the rest, in the order it always did.
    core::Rng factionRng(params.seed, kStreamFactions);
    const std::vector<std::uint32_t> capitals =
        chooseCapitals(params, factionRng, galaxy.systems, proceduralCount);

    // Which node each authored system took, by index; null for the ones the
    // seed produced. Passed down rather than stamped onto SystemSpec because
    // what the later stages need is "did the AUTHOR write this field", and
    // only the def row knows that.
    std::vector<const AuthoredSystem*> authoredFor(galaxy.systems.size(), nullptr);
    // PASS 2: constellation members take the nodes pass 1 made for them, then
    // the three replacement rules choose theirs - and every one of them,
    // insertion included, applies its author's fields through the same block.
    // Members go first because a constellation cannot fail, so a member is an
    // anchor a `jumps_from` may name whatever order the file was written in.
    std::vector<std::pair<std::string, std::uint32_t>> placedById =
        applyConstellations(params, firstConstellationIndex, galaxy.systems, authoredFor);
    placeAuthoredSystems(params,
                         authoredRng,
                         galaxy,
                         capitals,
                         proceduralCount,
                         galaxy.systems,
                         authoredFor,
                         placedById,
                         outFailures);

    // ⚑⚑⚑⚑ THE NAME LOOP IS UNTOUCHED, AND THE FIRST ATTEMPT AT THIS WAS THE
    // BUG THE WHOLE PHASE EXISTS TO AVOID. Skipping the draw for an authored
    // index shifts every LATER system onto a different name - one authored
    // system renamed most of the galaxy, which a test caught and which would
    // have read to a player installing a mod as "the galaxy regenerated". A
    // widened uniqueness scan was the second attempt and is subtler but the
    // same species: how many times the scan redraws depends on which names are
    // already present, so letting it see an authored name changes the draw
    // COUNT and shifts the stream again.
    //
    // So the procedural naming runs exactly as it did before Phase 29 existed,
    // and an authored name is applied afterwards, over the top.
    core::Rng nameRng(params.seed, kStreamNames);
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        std::string name = makeSystemName(nameRng);
        for (std::uint32_t j = 0; j < i;) { // linear uniqueness scan; n <= 150
            if (galaxy.systems[j].name == name) {
                name = makeSystemName(nameRng);
                j = 0;
                continue;
            }
            ++j;
        }
        galaxy.systems[i].name = std::move(name);
        galaxy.systems[i].seed = core::Rng(params.seed, kStreamContents + i).nextU64();
    }
    // ⚑ The SEED above is deliberately still the node's rather than the
    // author's: populateSystem draws its contents from it, so an authored
    // system that kept a name but lost its seed would generate from zero.
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        if (authoredFor[i] != nullptr && authoredFor[i]->hasName) {
            galaxy.systems[i].name = authoredFor[i]->name;
        }
    }

    claimTerritory(params, factionRng, galaxy.systems, galaxy.links, authoredFor, capitals);

    core::Rng clanRng(params.seed, kStreamClans);
    spawnClans(params, clanRng, galaxy.systems, galaxy.links, authoredFor, galaxy.clans);

    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        core::Rng contentRng(galaxy.systems[i].seed, kStreamContents);
        populateSystem(params, i, galaxy.systems[i], galaxy.systems, contentRng, authoredFor[i], mining);
    }
    return galaxy;
}

std::vector<std::uint32_t> routeBetween(const Galaxy& galaxy, std::uint32_t from, std::uint32_t to)
{
    const std::uint32_t count = static_cast<std::uint32_t>(galaxy.systems.size());
    if (from >= count || to >= count) {
        return {};
    }
    if (from == to) {
        return {from};
    }
    std::vector<std::uint32_t> previous(count, kInvalidIndex);
    std::vector<std::uint32_t> frontier{from};
    previous[from] = from;
    while (!frontier.empty() && previous[to] == kInvalidIndex) {
        std::vector<std::uint32_t> next;
        for (const std::uint32_t index : frontier) {
            for (const GateSpec& gate : galaxy.systems[index].gates) {
                if (previous[gate.toSystem] == kInvalidIndex) {
                    previous[gate.toSystem] = index;
                    next.push_back(gate.toSystem);
                }
            }
        }
        frontier = std::move(next);
    }
    if (previous[to] == kInvalidIndex) {
        return {};
    }
    std::vector<std::uint32_t> route;
    for (std::uint32_t index = to; index != from; index = previous[index]) {
        route.push_back(index);
    }
    route.push_back(from);
    std::reverse(route.begin(), route.end());
    return route;
}

} // namespace sol::sim
