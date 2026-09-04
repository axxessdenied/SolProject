# 015 — A ship you own exists whether or not you are looking at it

- **Date**: 2026-08-28
- **Status**: accepted; **amended three times** — twice on 2026-09-02 (the first
  bullet of the Decision by the Phase 38 spec, the second bullet by the Phase 39
  spec) and once on 2026-09-03 (the Phase 40 spec, which reopens the fine layer
  where the player has posted a fleet). See the “Amendment” sections below.
  The decision itself stands.

## Context

The v2 arc (GDD §14) puts captains and fleets on the board: ships you own, flown
by hired officers, working while you are elsewhere. The question underneath it is
whether such a ship *exists*.

Priced against the code on 2026-08-28:

- **`space_world.hpp:36` states the constraint in its own words**: *"a seeded
  procedural galaxy of which exactly one system — the player's — is instantiated
  at full fidelity (sim-LOD bubble); jumping through a gate demotes the old
  system to specs and promotes the destination."*
- **`despawnSystem()` destroys every entity of the current system except the
  player.** A jump is a teardown and a rebuild.
- **Positions are metres in the *current system's* barycentre frame.** This is
  the load-bearing sentence. It is not a rendering convention; it is what every
  `DVec3` in the ECS means.
- **132 references to `m_currentSystem`** in `space_world.cpp` (7,198 lines),
  **45 external callers of `currentSystemIndex()`**, and **15 component storages**
  (`Transform`, `FlightBody`, `Projectile`, `MineableRock`, `OreChunk`, …) all
  written against one frame.
- **There is already a coarse alternative that works.** `Economy` runs a fleet of
  a few hundred `EconomyTrader` agents across the whole galaxy with routes,
  phases, cargo and attrition, and `faction_sim` raids them — none of it
  instantiated. Phase 8x promoted coarse traders into real bubble ships on
  arrival, so the demote/promote seam is built and proven.

So the honest framing is: the cheap answer already exists and is running in
production, and the expensive answer is not a fleet feature at all — it is a
frame-of-reference change to the ECS.

## Decision

**Owned ships are full entities in whatever system they are in, and the
frame-of-reference change is its own phase, sequenced ahead of captains and
fleets.**

- An entity gains a **system index**, and a `Transform` means *metres in that
  entity's system's barycentre frame*. There is no galactic coordinate space;
  the large-world rule (`docs/engine-plan.md` §1) is preserved exactly, because
  the alternative — one global frame — puts a `double` under petametres of
  dynamic range and loses the precision that rule exists to protect.
- The sim ticks a **set** of systems: the player's, plus every system holding a
  player asset. Systems with nothing of the player's in them stay on the coarse
  layer they already use.
- **The bubble concept survives** and is generalised rather than deleted. What
  changes is that it stops being a singleton.

**This was put to the user with the counter-argument inside it and came back at
the fuller end**, which is the same pattern as Phase 27's four decisions. The
counter-argument is recorded below rather than discarded, because it stays valid
if the price proves worse than estimated.

### Amendment, 2026-09-02 (Phase 38 spec)

**The first bullet is amended: the frame is a property of the *registry*, not a
field on the entity.** One `sol::ecs::Registry` per instantiated system, rather
than a system index every query has to remember to filter on. Everything else
above is unchanged — no galactic frame, a *set* of ticked systems, the bubble
generalised rather than deleted.

The re-read that produced the Phase 38 spec found three reasons:

- **A frame filter is a thing you can forget, and nothing notices.** Phase 37
  shipped two stages whose entire point was invisible to every guard they wrote
  (361 of 361 green, then 366 of 366). Per-registry makes a cross-system query
  *unaskable* instead of merely wrong, and turns ~20 silent judgement calls into
  313 `m_registry` sites the compiler asks about.
- **`resolveCollisions` is `O(n²)` with no broadphase** — `sim/collision.hpp:37`
  says so in its own words. One global body list with a frame filter is
  `O((kn)²)`; a registry per system is `O(k·n²)` by construction.
- **"Is this the player" is written 17 times as an index comparison** against
  `playerEntityIndex()`, and indices are per-registry. The `PlayerShip`
  component that answers it unambiguously has existed since Phase 7 and is read
  at exactly three places, none of which is an identity test.

⚑ **Two consequences below need correcting rather than amending.** *"Rendering
and UI are unaffected in kind"* holds, but **audio is a third output path and is
affected**: five `playAt` sites hand a system-frame `DVec3` to a mixer with one
listener at the player's ear. And *"the largest single item in the v2 arc"* is
now larger — `space_world.cpp` grew from 7,198 lines to **12,063** in the four
phases between this decision and the spec written against it.

### Amendment, 2026-09-02 (Phase 39 spec) — the second bullet, split by order

**The second bullet is amended.** *“The player’s system, plus every system
holding a player asset”* holds for a **stationary** asset and does not hold for
an **itinerant** one. An owned ship working a field or standing a patrol keeps
its system instantiated, exactly as written. An owned ship hauling between two
markets rides the coarse layer while it is between systems and promotes to a
full entity whenever the player is co-located.

Three measurements forced it, all taken against the code Phase 38 shipped:

- **`kMaxInstantiatedSystems` is 6 and `instantiateSystem` returns `false` past
  it.** Under the unamended bullet, the sixth captain is the last one that
  exists, and the seventh is not refused — it is silently never simulated.
- **`releaseCooledBubbles` counts `holdSeconds` down unconditionally**, so an
  indefinite hold needs the refresh `kCoolingSeconds`’ own comment forbids; and
  **`enforceBubbleCap`’s safety argument** — *“nothing dangles: every reference
  that outlives a bubble is coarse-layer state keyed by SYSTEM”* — stops being
  true the moment an `OwnedShip` names an entity inside one. Evicting an
  ambient bubble is a degradation; evicting a captain’s is the player’s
  freighter vanishing with no death path.
- **`maxTradeJumps` is 5**, so one haul crosses up to six of eighty systems,
  and each one entered gets a full sky built for it (`fillSystemSky`: statics,
  rocks, ambient wings — 25–136 entities and 4–15 ships). One captain moving
  150 units of ore would make the game simulate a rolling window of somebody
  else’s traffic, and Phase 38’s 0.64 ms at six bubbles was priced as a
  two-minute **transient**, not a steady state.

⚑⚑ **This adopts the declined alternative below only where its own objection
never reached.** That objection is recorded as *“my escort died to a raid while
I was two jumps away becomes a die roll rather than a fight that happened”* —
which is about **combat**, and combat is exactly the half kept as full
entities. It says nothing about a freighter crossing 600,000 km of empty lane
on a schedule, which is what `TraderLeg::Depart` already is for 120 haulers.
And the precedent already shipped: **`RefineJob` is a player asset in a named
system converting value on a coarse clock with no bubble and no entity**, and
nobody has ever called it a spreadsheet. Recorded in full in
`020-captains-as-people.md`.

### What shipping the stationary half changed, 2026-09-03 (Phase 39 stage C)

**The amendment above held and one of its three measurements produced the
opposite answer to the one it implied.**

- **The cap is soft now, and only for the half that is paid for** (the user's
  ruling 11). `enforceBubbleCap` will not choose a bubble a captain is working
  in, and when every candidate is one it stops rather than picking the least bad
  victim. `instantiateSystem` still refuses an *ambient* bubble at the cap, so
  the fence did not stop meaning something — it stopped being able to take back
  what the player bought.
- **There is no `kHeldIndefinitely`.** The clause Phase 38 drafted into
  `bubbleRetentionSeconds` was right about the requirement and wrong about where
  it goes: a sentinel is a number the cap compares, the save round-trips and
  every decrement site has to recognise. `bubbleRetentionSeconds` answers the
  ordinary `kCoolingSeconds` for both of its clauses and `releaseCooledBubbles`
  **renews** it while the order stands — which is also what makes standing a
  captain down correct without a second rule, because the tick after the order
  goes the system is on the same two minutes any other one gets.
- **⚑⚑⚑ THE COST ARGUMENT ABOVE IS THE ONE THAT MOVED, AND IT MOVED IN THE
  CHEAP DIRECTION.** `kMaxInstantiatedSystems`' comment reads *"`k` bubbles cost
  `k*n^2`"*, and the shape of that sentence is what made a soft cap sound
  frightening. Measured past the cap (debug, 600 frames, shipped galaxy):
  **1 → 0.069 ms, 4 → 0.467, 6 → 0.629, 8 → 0.996, 10 → 1.261, 12 → 1.496**,
  against a 16.7 ms frame. The first three reproduce Phase 38's own numbers to
  the third decimal. **The curve is linear at about 0.12 ms per bubble**, because
  `resolveCollisions` is quadratic *inside* a bubble and bubbles cannot see each
  other — so `k` bubbles cost `k` times one, and `n` is a per-system constant
  that has nothing to do with `k`. A player would need on the order of 130
  captains to spend a frame, and each of those is a hull they bought.

⚑ **And the coarse layer turned out to be needed by the stationary half after
all, for one thing.** A field sits 8e7–4e8 m from the barycentre and so does the
dock a captain sells at, so the run between them is a crossing of a playfield
that a 120 m/s hull takes days over. `keepTraderOnSchedule` exists for exactly
that reason — *"the record moves it faster than any hull flies"* — so a
stationary captain's crossing is paced at
`MiningParams::fieldMaxDistance / EconomyParams::traderLegSeconds`, a number
borrowed from the constant whose own comment says *"in-system travel per
endpoint"*. The split is about where the RECORD lives, not about who is allowed
to use the coarse fleet's arithmetic.

### Amendment, 2026-09-03 (Phase 40 spec) — the fine layer reopens where the player has posted a fleet

**Phase 39's ruling 12 said an unwatched fight is a die roll. This reverses that
for one case — a system the player has posted a FLEET in — and the re-read that
priced the reversal found the original ruling had been argued against the wrong
layer.**

- **⚑⚑⚑⚑ THE PER-FRAME COST OF AN UNWATCHED FIGHT IS ALREADY PAID.**
  `rollHeldBubbleHazard`'s own comment justifies the coarse pricing by saying the
  alternative *"pays per-frame for a fight nobody is watching"*. It does not.
  `tickSystem` runs for **every** bubble, and `sim.pilots`, `sim.collision.build`,
  `sim.collision.resolve`, impact damage, `sim.projectiles` and `sim.weapons` are
  all inside it — which is Phase 38's own statement of the consequence, in its
  own words: *"A raider in Attack goes on attacking, guns keep firing, hulls keep
  taking damage — the fight continues — but nothing re-targets."* What Phase 38
  scoped out is `collectDuePilotThinks`, and `kThinkInterval` is **0.5 s**. So
  reopening buys a **2 Hz decision**, not a 60 Hz combat bill.
- **⚑⚑⚑ THE REAL OBSTACLE IS THE REGISTRY FRAME, WHICH IS THIS DOC'S OWN
  DECISION WORKING AS INTENDED.** The nine `sol.pilot_*` bindings answer through
  world functions that read `playerRegistry()` **25 times** between them, and two
  of the nine are player-frame *by definition* — `pilot_attack_player`, and
  `pilotEngageEnemy`'s clause that asks whether the player is docked. The frame
  being a property of the registry is what makes the cross-registry reach
  unaskable; reopening therefore happens **in C++, per bubble**, which is the
  exception Phase 39 already carved for `tickStationaryCaptains` and
  `tickPatrolBeat`, pointed the other way. **Lua stays player-scoped.**
- **⚑⚑ THE DIE ROLL MUST STAND DOWN FOR EXACTLY THOSE SYSTEMS.**
  `rollHeldBubbleHazard` skips slot 0 and only slot 0. A roll and a real fight in
  one bubble is *"a captain that is both things"* — the defect Phase 39's risk
  register names first — reached from the opposite side, and nothing in the 440
  tests asks the question.
- **⚑⚑⚑⚑ AND STANDING THE ROLL DOWN IS ONLY HALF A RULING, WHICH BUILDING IT
  IS WHAT SHOWED (Phase 40 stage C, 2026-09-04).** A stand-down with nothing
  put in its place makes posting a fleet an **invulnerability field**: the
  coarse layer stops touching the system, and the fine layer has no producer of
  its own — `spawnAmbientPilots` fills a sky once, at bubble open, and a held
  bubble never re-opens. That is Phase 39 stage D's *"a posted captain was safe
  precisely because nobody was looking"* re-created from the other side, one
  phase after it was closed. **So the ruling is that the same danger has a
  different CONSEQUENCE where a fleet is posted, not that it stops**: one rate,
  and it either takes a captain by arithmetic or sends hulls through a gate to
  try it. ⚑ The arrival rate is the **bare** one — `heldBubbleRiskPerSecond`'s
  halving per guard was a *model* of a fight nobody could watch, and modelling
  the guard while the guard is in the sky flying it would price it twice. A
  fleet is therefore visited **more often** than a guarded pair would have
  been; what it buys is surviving the visit.
- **⚑⚑⚑ AND THE COST ARGUMENT MOVED IN THE CHEAP DIRECTION FOR THE SECOND
  CONSECUTIVE AMENDMENT, ON A DIFFERENT AXIS.** The one above measured held
  *systems* at ~0.12 ms each, linear. A fleet is the other axis — N hulls in ONE
  system, where `resolveCollisions` is quadratic. Measured 2026-09-03 (debug, 600
  frames, one held bubble across 24 shipped systems, baseline 0.067 ms):
  **hulls 0.00204 ms each with r² 0.010; entities 0.00199 ms each with r² 0.836.**
  Hulls explain one percent of the variance and entities eighty-four — system 8
  is 15 hulls in 25 entities for 0.155 ms, system 11 is 5 hulls in 136 entities
  for 0.350 ms. **A bubble costs what is in it, and a hull is one entity much as
  a rock is.** Six captains added to a system is about 0.012 ms. *The quadratic
  term is over movers, which the shipped galaxy tops out at 15 — 105 pairs — and
  it is not detectable at that scale.* **So fleet size is a design limit, not a
  frame-budget one.**

## Alternatives considered

- **Coarse away, real when near** (the recommended option, declined). Owned ships
  out of system run as `EconomyTrader`-shaped agents and promote to full entities
  when the player arrives. Cost: near zero new architecture — the machinery runs
  ~45 haulers today and the promotion seam is Phase 8x's. Loss: an owned ship
  cannot do anything the coarse layer cannot express, so "my escort died to a
  raid while I was two jumps away" becomes a die roll rather than a fight that
  happened. **That loss is exactly what the user declined to accept**, and the
  reason is legitimate: the pillar says the universe does not wait for you, and a
  fleet that is only a spreadsheet when unobserved is a weaker version of that
  claim than the game already makes about NPC traders.
- **Full entities in every system, always** (all 80). Rejected — not asked for,
  and it would put the entire galaxy's ambient population in the ECS to no
  gameplay end. The decision is *owned assets*, not *everything*.
- **A global coordinate frame.** Rejected on the space-scale constraint, which
  `docs/engine-plan.md` calls "day one, non-negotiable".

## Consequences

- **This is the largest single item in the v2 arc**, and it is a phase before it
  is a feature. It lands as its own phase (engine plan Phase 38) with nothing
  player-visible in it except that a ship left behind is still there.
- **Every system that reads a position must learn to ask "in which frame?"** The
  132 `m_currentSystem` references are the map of that work; most are queries
  that become "the player's system" and are correct unchanged, but each has to be
  read rather than assumed.
- **Rendering and UI are unaffected in kind**: the camera is in one system, and
  entities in other systems are not drawn. Radar, target cycling and picking all
  gain a frame filter they did not need before — and that filter is the main
  correctness risk, because omitting it shows a ship two jumps away as a contact
  at 300 m.
- **Collision, steering, weapons and mining become per-system**, which is a loop
  nesting change rather than an algorithm change.
- **The save format grows a system index per entity** and gains ships that are
  not in the player's system. `kSaveVersion` bumps.
- **The fallback stays available and stays cheap.** If the phase's real cost
  exceeds its estimate, the coarse option above is a working design that can be
  adopted without redesigning captains or fleets — they are specified against a
  command vocabulary, not against an entity model.
