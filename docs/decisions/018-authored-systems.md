# 018 — A system can be written by hand: authored systems, constellations, and placement rules

- **Date**: 2026-08-28
- **Status**: accepted

## Context

The Depth Arc asks for pre-defined systems the generator spawns into the galaxy
"for narrative purposes when creating the campaign", grouped into constellations,
placed by rules, defined in TOML and exposed to modding.

Priced against the code on 2026-08-28:

- **`generateGalaxy` is purely procedural and reads nothing from disk.**
  `GalaxyParams` is seed, counts, radii, thresholds and weight tables; every
  `SystemSpec` — name, map position, region, faction, star, planets, stations,
  gates — is rolled from `core::Rng` streams. There is no authored input path at
  all.
- **The output shape is already exactly what an author would want to write.**
  `SystemSpec` is a plain struct of named fields and vectors of
  `PlanetSpec`/`StationSpec`/`GateSpec`. Authoring one is *filling in a struct
  this project already has*, not designing a format.
- **Determinism is a hard constraint and it is documented as one.**
  `universe.hpp`: *"Same seed + same params => same galaxy, so everything here
  draws from `sol::core::Rng` streams and iterates in index order; no unordered
  containers, no wall clock."*
- **`routeBetween` already answers "how many jumps from here to there"** by BFS
  over the gate graph — which is the primitive a "N jumps from X" placement rule
  needs, and it exists.
- **The mod layering system is mature.** `DefDatabase::mergeDirectory` merges
  every `*.toml` in layer order with replace-by-id semantics, and Phase 24 stage S
  extended layering to cooked assets and shaders. A new def kind is a worn path.
- **GDD §3 already promises this** — *"story anchors (unique stations, derelicts,
  questlines) are authored; the generator places them according to rules"* — and
  it was never built.

**And the sequencing finding**: the arc's own justification for this feature is
the campaign. Campaign Act 2 has been the sole remaining roadmap item for several
sessions. So authored systems are not a competitor to Act 2 — they are a
**dependency** of it, and building them first is what lets Act 2 be written
against fixed places instead of whatever the seed happened to produce.

## Decision

**`[[system]]` and `[[constellation]]` become def kinds, injected into generation
before the procedural pass fills the rest.**

An authored system carries the `SystemSpec` fields an author cares about and
leaves the rest to be filled procedurally — an authored system that names only a
station and a placement rule is legal and useful.

**Placement rules**, in the vocabulary the ask named:

| Rule | Meaning |
|---|---|
| `at_system = "<id>"` | Replace/occupy a specific named system. |
| `jumps_from = { system = "<id>", min = 2, max = 4 }` | Somewhere in a ring of gate distance. `routeBetween` already measures this. |
| `random`, with `exclude_secret = true` | Any ordinary system; never lands on something marked secret. |
| `anywhere` | A new position in galaxy map space, gated in like any other node. Side-quests and secrets. |

**A constellation is placed as a unit with its internal topology intact** — its
member systems keep their links to each other and the constellation as a whole
takes one placement rule.

**Three rules protect what already works:**

1. **Determinism is preserved.** Authored placement resolves in def order against
   the same seeded streams, before procedural placement runs. Same seed + same
   content ⇒ same galaxy, still.
2. **Placement can fail, and failure is loud.** A rule with no legal site is a
   reported error naming the file, not a silent omission — this project's
   `validateRoles` precedent (refuse rather than warn when there is no fallback).
3. **Authored systems are ordinary mod content.** A total-overhaul mod can
   replace the map, not just the rules — which is the strongest single answer to
   the "yours to break" pillar the arc contains.

## Alternatives considered

- **Post-generation patching** — generate procedurally, then overwrite chosen
  systems. Rejected: it produces gate graphs that contradict the authored layout
  (the generator has already decided the neighbours), and connectivity is an
  invariant `generateGalaxy` guarantees. Injecting before is the only order that
  keeps the guarantee cheap.
- **Hardcoded C++ special systems.** Rejected on the same grounds as every other
  name-wall this project has taken down since Phase 9 stage A: it would not be
  moddable, would not be authorable in the Forge, and would put content in the
  binary.
- **A separate non-TOML format for authored systems.** Rejected — `DefDatabase`
  merging, layering, strict schema and hot reload are free by staying in TOML,
  and every one of them would have to be reinvented.

## Consequences

- **Act 2 unblocks.** It can be written against named, fixed places, which is what
  an authored campaign needs and has never had.
- **Secrets get a home.** `exclude_secret` gives the exploration loop (GDD §8)
  something to actually find at the end of it — the payoff Phase 8e's fog was
  built toward.
- **`GalaxyParams` grows an authored-content input**, and `generateGalaxy` gains a
  pre-pass. The procedural path with no authored systems present must produce a
  byte-identical galaxy to today's, which is the phase's cleanest exit criterion
  and is directly testable against the shipped seed.
- **Save compatibility**: a galaxy is regenerated from seed + defs on load, so
  adding an authored system to a running campaign changes the galaxy under an
  existing save. This is the same hazard Phase 13 hit when world-gen changed, and
  it is handled the same way — a content version bump, not a migration.
- **The Forge has no authored-system editor** and does not gain one in this phase.
  Authoring is by hand in TOML, validated by the game's own schema. A visual
  system editor is a plausible later Forge stage and is deliberately not assumed
  here.
