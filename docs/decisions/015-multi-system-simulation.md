# 015 — A ship you own exists whether or not you are looking at it

- **Date**: 2026-08-28
- **Status**: accepted

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
