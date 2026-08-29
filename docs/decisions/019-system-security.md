# 019 — System security: a static baseline, a live modifier, and a signed scale whose negative half is somebody else's law

- **Date**: 2026-08-28
- **Status**: accepted

## Context

The Depth Arc was planned on 2026-08-28 from eleven asks. A twelfth arrived the
same day, in one sentence:

> security ratings for systems which will impact the strength of patrolling
> security forces and response times. systems with no security or negative
> security will be more dangerous to travel through and would be home of pirate
> factions for example.

Priced against the code before any of it was designed, per the standing rule
that a roadmap estimate is re-read against the tree before it is trusted. The
survey found the arc's usual split — **half of it is already built and unwired,
and the other half is zero** — but with an unusually sharp version of the first
half:

- **The situational half of system security already exists, under another name.**
  `FactionSim::danger(system)` (`faction_sim.cpp:362`) is a per-system 0..1
  scalar, `raidIntensity * dangerPerRaid + contestPressure * dangerPerContest`,
  clamped. It is tested (`faction_sim_danger_is_made_of_raids_and_contests`), it
  is serialised with the rest of `FactionSim`, it is bound to Lua as
  `sol.danger`, and it already has two consumers — escort mission pay
  (`missions.cpp:232`) and coarse trader attrition (`faction_sim.cpp:156`).
  **Phase 8x built this three phases before anybody asked for it.**
- **Patrol strength is already keyed to a per-system tier**, and the tier is
  three values wide: `kPatrolsPerRegion[3] = {3, 2, 1}` (`space_world.cpp:2405`),
  beside `kCiviliansPerRegion[3] = {4, 3, 1}`. The comment above them, from
  Phase 13, already reads *"patrol wings by region security"* — **the phrase the
  user used is already in the source.**
- **Negative-security systems already exist and are already pirate homes.**
  `fringeLawlessChance = 0.6` sends fringe systems to `kNoFaction`, and
  `spawnClans` gives every connected lawless component to a generated clan;
  `universe_pirate_clans_claim_lawless_neighborhoods` asserts no system stays
  unclaimed. Those systems already spawn raider wings rather than patrols.
- **There is no response of any kind.** No `reinforce`, no `distress`, no
  dispatch verb anywhere in `engine/sim` or `game/src`. Every ambient wing is
  spawned once inside `loadSystem` and never again, and `spawnWing` is a lambda
  *local to that function* — "send a wing later" is not currently callable.
- **But a diverted response is two shipped functions.** `pilotPatrolTo(entity,
  waypoint)` redirects a patrol to an arbitrary point, and `PilotState::Travel`
  is long-haul cruise-drive travel to a waypoint. Nothing has ever asked a patrol
  to go somewhere it was not already going: the Lua patrol branch only calls
  `nextLeg` around a fixed diamond at station 0.
- **The cheapest-looking route to a spawned response is a trap.**
  `spawnPilotFromDef` places a ship 150–250 m directly in front of the player
  (`space_world.cpp:4830`) — correct for the console it was written for. Both
  `sol.spawn_pilot` and `sol.spawn_pilot_faction` go through it, and the latter
  is one of the 69 never-called bindings, so it looks like a ready-made answer.
- **The player has never been shown the number that already exists.**
  `map_ui.cpp:400` composes a system row with region, owner, contested flag,
  survey state and bookmarks — and no danger.

## Decision

Three rulings, each put to the user with its counter-argument, and **all three
returned at the recommended end**. Consistent with the arc's own finding that a
recommendation backed by a measurement is taken and one backed by taste is not:
each of these was argued from something in the tree.

### 1. A static baseline plus a live modifier

`SystemSpec::security` is written at generation from owner kind, region and
gate-distance to the owner's capital. What a consumer reads is
**`baseline − danger(system)`**, clamped into the band.

Rejected — *purely static*: free (the galaxy regenerates from seed, so nothing
is added to the save format) but it means a system raided for the last hour
still reads as safe, which is the precise lie `danger` was built to avoid.

Rejected — *fully dynamic*: richest, and it would make "clean out a system" a
goal, but it costs a new saved array, a decay curve to tune, and a feedback loop
with the faction sim — on top of standing risk 5 below.

**Consequence: this phase breaks no save format.** The static half regenerates
from the seed; the live half is `danger`, which already serialises. That is now
true of exactly two phases in the fourteen.

### 2. The scale is signed, and negative means *policed by somebody else*

- **Positive** — a major holds it. Patrol strength and response scale with the
  number.
- **Zero** — nobody comes. You are on your own.
- **Negative** — a clan holds it, and it responds to intrusion the way a navy
  does. A wing is dispatched, and it is coming for you.

Rejected — *unsigned 0..1 with a separate pirate-presence number*: two different
things honestly getting two different numbers, but it drops the word the user
used and costs the map its single readable gradient.

Rejected — *negative means every resident force is hostile to the lawful,
pirate-held or not*: the strongest fiction, but it needs a "collapsed ownership"
state that does not exist — today every system is held by a major or a clan,
with nothing in between.

The accepted reading is the one that costs least, because **it names what the
generator is already doing** rather than adding a mechanism.

### 3. Divert first; spawn only when nobody is in range

`respondTo(position, cause)` redirects the nearest un-engaged patrol with
`pilotPatrolTo`. When there is none, a wing is spawned **at the nearest station
or gate** and flown in on `PilotState::Travel`.

**Response time is therefore a real transit across real distance, not a timer.**
A crime over the pad and a crime at a gate 600,000 km out differ without either
being scripted.

Rejected — *always spawn*: one code path, and the trader-puppet system is a
precedent for spawning mid-flight from coarse state. But the placement trap above
makes the naive version materialise a response wing in the player's face.

Rejected — *divert only*: purest, and nothing ever appears from nowhere. But
patrols today fly a fixed diamond around station 0, so a provocation at a distant
gate would draw a response measured in minutes of cruise, or none — making
"response time" a promise the code cannot keep.

## The ruling that was taken rather than discovered

**Patrol strength reads the baseline; danger, attrition and response *time* read
the live rating.**

The naive wiring — every consumer reads the live number — is a positive-feedback
spiral: a raid raises `danger`, which lowers live security, which thins the
patrols, which makes the next raid cheaper. A navy's garrison does not evaporate
because pirates turned up.

So the two halves answer two different questions. **The baseline is how much
force the owner keeps here; the live rating is how safe it actually is right
now.** Response time is allowed to degrade under load — busy patrols *are*
slower — and that is the single place the live number touches enforcement.

## Consequences

- **Phase 30** in the engine plan, sequenced third: it depends on nothing (A–D),
  it is visible from the cockpit the day it lands, and **Phase 36's *notice* rule
  is tuned against a number that has to exist first**.
- **Phase 36 (law) and Phase 37 (the shadow faction) gain a dependency on it.**
- **Standing risk 5 is created by this decision.**
  `FactionSimParams::traderLossPerSecond` carries a comment saying it is guarded
  by `economy_shipped_rates_hold_a_steady_state` and that moving it re-runs 8g's
  tuning of the galaxy's equilibrium. Security becomes a second input to that
  number, and Phase 33 separately multiplies the commodity count by ten.
  **Whichever lands second inherits the obligation to re-run the tuning, not just
  the test.**
- `kPatrolsPerRegion` and `kCiviliansPerRegion` retire into curves.
- `spawnWing` must be lifted out of `loadSystem` into a member function before
  any of stage C is reachable.
- The player-facing half is a field on a row that is already built, under the
  same `visited` knowledge rule the owner colour already obeys.

## Status of the sketch

**This is a sketch, and the fact that it was written by the session that read
the code does not make it a spec.** That is this project's own rule, learned when
Phase 28's sketch was refuted hours after being written by the same session —
the ninth consecutive refuted estimate. Phase 30 gets a full spec against a fresh
re-read immediately before it starts, like every phase since 8a.
