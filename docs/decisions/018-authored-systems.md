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

## Fourth amendment — 2026-08-29, when Phase 29 stage D was built

Shipping the first authored content changed nothing about what an authored
system *is* and quite a lot about what the tests around it were actually
asserting. Nothing above is refuted; four things are added.

- **⚑⚑⚑ THE MOMENT `game/data` SHIPS AN AUTHORED SYSTEM, EVERY LITERAL INDEX IN
  THE SUITE BECOMES A STATEMENT ABOUT HOW MUCH CONTENT THE BASE GAME CARRIES.**
  Stages A–C could write *"the appended system is index 80"* because there was
  no authored content but the fixture's. One `[[system]]` in `game/data` made
  four of those literals wrong at once — and each of them was wrong in a way
  that read as the fixture being broken. They are now derived from **def
  order**: everything the seed produced, then every `anywhere` row in def order,
  then every constellation's members contiguously. That is the generator's own
  documented rule restated, and it is a better assertion than the numbers were.
- **⚑⚑⚑ THE GOLDEN NOW PHOTOGRAPHS A GALAXY THE GAME NO LONGER GENERATES, AND
  THE STRIP THAT MAKES IT DO SO NEEDS ITS OWN GUARD.** The exit criterion is
  *"with no authored systems present"*, so the golden takes the params
  `generateUniverse` built and empties `authoredSystems`/`constellations` before
  regenerating — the layer where `GalaxyParams` already says empty means the
  pre-29 galaxy. **A strip that strips nothing looks exactly like a strip that
  works**, which is the same species of invisible failure as an install EXCLUDE
  that matches nothing — the other half of this same stage — so it requires that
  something was removed and prints the count. Both counterfactuals were RUN: with
  the strip disabled the golden fails on three digests, and with
  `game/data/systems.toml` moved aside it refuses to certify anything.
- **⚑⚑ THE SAVE DIGEST OF DECISION 7 IS OVER THE RESOLVED *INPUT*, NOT OVER THE
  GALAXY AND NOT OVER THE FILE BYTES.** Digesting the generated galaxy answers
  the same question and costs a full generation at load time before the answer
  arrives; digesting the file would refuse a save because somebody reflowed a
  comment. The authored rows are already in hand in `GalaxyParams`, and they are
  the only thing that can differ at a fixed seed and a fixed build. ⚑ The counts
  are folded in beside the rows, so **removing the last authored system is a
  different digest from never having had one** — which is the case a player hits
  when they uninstall a mod. ⚑ And `readSaveInfo` reads the digest and
  **discards** it: a save whose content has moved is still perfectly describable
  as a *file*, and hiding it would leave a player unable to see that their
  campaign exists. The comparison belongs where acting on it is possible.
- **⚑⚑ THE SUB-HEADER HAZARD THAT COST PHASE 25 A SESSION DOES NOT APPLY HERE,
  AND THAT WAS CHECKED BEFORE THE FILE SHIPPED RATHER THAN AFTER.** `def_doc` —
  the comment-preserving document the Forge edits through — refuses a plain
  `[table]` header, and `systems.toml` is the first committed def file with a
  nested header in it. `[[system.planet]]` is still a `[[table]]`, so it becomes
  a row of its own with its header line kept raw and round-trips byte for byte;
  `systems.toml` is in the round-trip suite as of this stage. The Forge itself
  opens five named documents and never sees this file at all.
- **⚑ AND THE CONTENT THIS REPOSITORY SHIPS IS ITSELF A PER-SEED CLAIM.** The
  example mod places a system by `jumps_from`, `--seed N` is a command-line
  flag, and a ring satisfiable at 1701 and nowhere else would turn every other
  galaxy into a refusal at boot. The committed content is therefore placed at
  eight seeds by a test rather than at the one the game launches with.

## Third amendment — 2026-08-29, when Phase 29 stage C was built

Building constellations narrowed one sentence in this record and confirmed
another for a reason it did not give. The original wording is left above.

- **"The constellation as a whole takes one placement rule" is true, and the
  rule can only ever be `anywhere`.** The sentence reads, beside the four-row
  table above it, as *"any one of the four"*. It cannot be. Three of those four
  rules **replace** a system the generator already made, and a group cannot
  replace one node as a unit — so `random` would have to mean *"near a randomly
  chosen system"* for a group while it means *"become a randomly chosen system"*
  for a system, which is two rules wearing one word. `at_system` and
  `jumps_from` split the same way. **So `placement` on a `[[constellation]]` has
  exactly one legal value, and anything else is refused with that reason** —
  which is a better answer to an author than a key that is not there.
- **And the restriction is not a limitation, it is the same fact as the
  feature.** Only an insertion can carry internal lanes at all: a replacement
  inherits the neighbours the generator already chose and declares none of its
  own, which is precisely why a replacement contradicts no gate graph. A
  constellation declares lanes, so a constellation appends.
- **"Its member systems keep their links to each other" needed the adjacency
  list taught about them, not just the link list.** Lanes are seeded into
  `Galaxy::links` before Prim runs, and `buildGateGraph` now reads them into its
  adjacency list first. Without that second half the dedup cannot see them and
  Prim draws a **second copy** of every lane the MST would have drawn anyway —
  a galaxy that looks entirely correct and has doubled gates.
- **⚑ A CONSTELLATION CANNOT FAIL TO BE PLACED, WHICH GIVES `jumps_from` AN
  ANCHOR THAT DEF ORDER DOES NOT CONSTRAIN.** It makes its own nodes, so there
  is no "nowhere to go" for it to report and no ordering in which a member is
  not yet placed. A `[[system]]` anchoring on a member may therefore be written
  before the group it names, where anchoring on another `[[system]]` still
  requires the anchor to come first.
- **⚑⚑ AND A WARNING ABOUT MEASURING ANY OF THIS: MEMBERS SIT IN A TIGHT
  CLUSTER, SO THE MST AND THE EXTRA-LANE PASS BETWEEN THEM DRAW A NEAR-COMPLETE
  MESH OVER A SMALL GROUP BY ACCIDENT.** A three-member constellation asserting
  its own triangle passes with the lane seeding **removed entirely**. Only a
  shape proximity does not produce — a star, or a path through six members in
  declaration order — can tell an authored lane from a lucky one. *"Internal
  topology intact"* is therefore easy to believe and hard to check, and the
  check is a counterfactual rather than an assertion.

## Second amendment — 2026-08-29, when Phase 29 stage B was built

Building the placement rules refuted one more line of the table above, and the
user took the ruling that replaced it.

- **`at_system = "<id>"` had no legal argument, and the spec's own restriction
  is what removed the last one.** The table says *"Replace/occupy a specific
  named system"*. Naming a **procedural** system was already refused when this
  phase was spec'd — names are rolled *after* placement, so they are a fact
  about one seed at one system count — which left decision 4's *"authored ids
  only"*. But **every authored id belongs to a system that already occupies its
  own node**, so every remaining argument was a contradiction: the rule could
  only ever name a place that was taken. (A mod *replacing* a base game's
  authored system needs none of this and already worked: `mergeDef` has a later
  layer replace an earlier one wholesale, by id.)
- **So `at_system` names a FACTION, and means that faction's capital.**
  `at_system = "sol.navy"` is *"the Navy's home"* — which is the sentence the
  spec itself said a campaign wants to say, and which it had deliberately
  deferred to a later phase. It was pulled forward here because the alternative
  was shipping a key whose every use was an error.
- **The cost was an ordering change, and it was smaller than it looked.**
  `claimTerritory` *chose* the capitals and ran *after* placement, so there was
  no moment at which a capital was a thing a rule could point at. Selection is
  now its own function running before placement. It still takes the same single
  draw from the same faction stream, so a galaxy with no authored systems in it
  is unchanged in every field — which the shipped-seed golden holds rather than
  a comment.
- **Two behaviour changes ride along, both invisible to an unauthored galaxy.**
  Capital candidacy now reads the *procedural* region assignment, so an authored
  system declaring itself core does not add itself to the pool of places a
  faction might be capital of; and an **appended** (`anywhere`) node is not a
  candidate at all, which is also what keeps `at_system` resolvable.
- **`exclude_secret` still names nothing**, as the first amendment recorded.

⚑ **And one sharpening rather than a refutation: *"placement can fail, and
failure is loud"* is a per-SEED verdict, not a per-file one.** A `jumps_from`
ring is a claim about a gate graph and the gate graph is built from the seed, so
a ring that holds at the shipped seed can be empty at another one. There is no
load-time check that could have settled it once for every galaxy a player will
see — which is why the refusal lives where the graph does, and why it is a
return value rather than a validation pass.

## Amendment — 2026-08-29, when Phase 29 was spec'd against the code

The re-read this project requires before a phase starts refuted one sentence in
this record, and sharpened two others. The original wording is left above.

- **"Authored placement resolves in def order against the same seeded streams,
  before procedural placement runs" is not achievable for every rule, and the
  table above already says why.** `jumps_from` measures gate distance, and the
  gate graph does not exist until `buildGateGraph` has run over final positions
  (`universe.cpp:528`). But `at_system` is documented here as *"Replace/occupy a
  specific named system"* and `random` as *"Any ordinary system"* — **three of
  the four rules pick an existing node**, and only `anywhere` creates one. So
  placement has **two injection points**: `anywhere` and constellations append
  nodes just after `scatterSystems`, and the three replacement rules resolve in
  def order **after** the gate graph, overwriting a node's spec while inheriting
  its position, its region and its gates. Determinism is preserved exactly as
  rule 1 intends; only the *when* moves.
- **This is still not the "post-generation patching" rejected below.** That was
  refused because patching after generation *"produces gate graphs that
  contradict the authored layout"*. A replacement contradicts nothing, because
  an authored system declares no external links. What remains forbidden is
  patching after `populateSystem`, which is a later point in the pipeline.
- **`exclude_secret` names a concept that does not exist in the codebase.** No
  occurrence of "secret" outside this document and the arc's own text. Phase 29
  introduces it as a flag on an authored system and nothing more; the
  exploration payoff GDD §8 describes is a later phase reading that flag.
- **The save consequence below is understated.** It says a galaxy regenerates
  from seed + defs, so adding an authored system changes the galaxy under an
  existing save, handled by a content version bump. But `galaxyChanged` keys
  **only on the seed** (`space_world.cpp:7211`), and a mod is not the build — so
  a player who installs one mid-campaign gets a silently reshaped galaxy that no
  version bump can see. Phase 29 writes the authored input's digest into the save
  beside the seed and refuses a mismatch, which makes the phase a save format
  break rather than a free one.

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
