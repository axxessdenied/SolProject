# 017 — Law is played out, not rolled: the transponder, the stop, and the scan

- **Date**: 2026-08-28
- **Status**: accepted

## Context

The Depth Arc asks for a transponder the player can switch off, contraband whose
legality varies by faction, technologies that help you avoid being caught, and a
black-market faction that operates out of other people's stations.

Priced against the code on 2026-08-28:

- **None of it exists.** Zero occurrences of `transponder`, `contraband`,
  `smuggl`, `illegal`, or a crime/wanted state anywhere in `engine/`, `game/` or
  `tools/`. This is the largest genuinely-empty area in the whole arc.
- **But three of its four ingredients are already built.**
  - **Hailing is a live loop.** `SpaceWorld::hailTarget()` (`kHailRange` = 20 km)
    queues a request, `GameContent` drains it, asks the Lua `pilot_hail` hook and
    calls one of three answers. Phase 8s built the whole round trip.
  - **Docking clearance is a timed, revocable grant.** `DockClearance` with
    `kClearanceSeconds = 180` — a station already decides whether it will let you
    in and can change its mind.
  - **Scanning over time exists.** Phase 8e's target scan has `scanRange`,
    `scanSpeed`, a progress rate and modules that modify both. A cargo scan is
    that machinery pointed at a hold.
  - Missing: reputation *consequence* beyond hostility, and any notion of a
    ship's legal status.
- **Faction relations are a live symmetric matrix** with war thresholds and
  hysteresis, and pirate factions already fence past `min_rep` — the catalog gate
  already understands "this seller does not care about your record."

So the expensive-sounding half — patrols that stop and scan you — is mostly
existing systems wired together, and the genuinely new work is the *state*: what
a transponder is, what a jurisdiction thinks of a cargo, and what happens after.

## Decision

**The full inspection loop, played out in real time and real space.**

1. A patrol **notices** — dark transponder, standing, a random check, or a tip.
2. It **hails** and orders a hold (the Phase 8s path, with new verbs).
3. If you hold, it runs a **cargo scan over real seconds at real range** (the
   Phase 8e path, pointed at the hold).
4. The manifest is judged against **that faction's** legality table.
5. **Consequence** scales with what was found, your standing and how the stop
   went: waved on, fined, impounded, bountied, or fired on.

**Every step is a decision the player can refuse**, and refusing is legible: you
can run before the hail, run during the scan, or comply and lose the cargo. That
is the reason this shape was chosen over a die roll — it makes smuggling a
*piloting* problem, which is what gives covert hulls, signature dampeners and
transponder spoofers something to be good at.

**Legality is a property of jurisdictions, never of cargo.** A commodity is not
flagged illegal; each faction declares `contraband` and `restricted` lists. The
same crate is medicine, a licensed pharmaceutical, or ten years, depending on
whose space you are in.

**The black market is a `kind = "shadow"` faction**: no territory, no stations of
its own, present wherever a station's module list carries shadow modules
(`decisions/016`). Standing with it is earned by the acts that cost standing with
the law — which makes smuggling an allegiance rather than a tolerated exploit,
and gives the reputation web (GDD §7) a genuinely opposed axis it has never had.

## Alternatives considered

- **Detection and consequence only** — transponder state plus a detection roll
  against your kit and local patrol density, no interactive stop. Much cheaper and
  it delivers the economic loop. Rejected because it makes the counter-technology
  a stat rather than a tactic: with no stop to survive, a signature dampener is a
  bigger number and nothing else, and the covert hull family (GDD §11.2) loses its
  reason to exist. The user chose the fuller shape knowing the price.
- **Cargo legality with no transponder** — contraband is refused or fined at the
  dock and never checked in space. Rejected: it moves the whole mechanic to a
  menu, and "risk turning your transponder off" was the thing actually asked for.
- **A global wanted level** (one number, all factions). Rejected as incompatible
  with the reputation web already built: bounties are per faction because
  standing is per faction, and one number would flatten the system's best feature.

## Consequences

- **Ships gain legal state**: transponder on/off, broadcast identity, per-faction
  bounty. It saves, so `kSaveVersion` bumps.
- **Patrol AI gains verbs.** `pilot_think`'s `patrol` role currently has three
  outcomes (flee, engage, resume patrol). It gains notice → hail → hold → scan →
  judge, and because the Lua hook already receives role, state, attitude and
  faction, most of the new policy can live in `init.lua` rather than in C++ —
  which matters, because **69 of 158 bound `sol.*` functions are currently unused**
  and this is the kind of work they exist for.
- **Cargo becomes inspectable**, which it has never been. Phase 8e's exploration
  spec explicitly listed *"scannable NPC cargo"* as out of scope; this reverses
  that deferral deliberately, and now there is a reason for it.
- **The covert subsystem family gets its purpose** (`decisions/014`): dampeners,
  spoofers, shielded holds and early-warning sensors are mounts that are not
  carrying guns. That trade is the design.
- **Balance risk, stated up front**: an inspection loop that fires too often is a
  tax on ordinary play, not a mechanic. Notice must be *rare* in policed core
  space for a clean pilot and *common* for a dark one, and that is a tuning
  problem the phase must playtest rather than assume.
- **Tone boundary**: contraband is drugs, alcohol, proscribed foods, unlicensed
  technology, weapons and stolen data. Trafficking in people is out of scope for
  this game and is not in the vocabulary.
