# 003 — Power management is Elite-style pips

- **Date**: 2026-08-15
- **Status**: accepted

## Context

GDD open question Q3, due at Phase 6 (Combat & AI): the pilot's in-combat
power triage — Elite-style pips across weapons/engines/shields, or
X4-style discrete module toggles. Energy weapons draw from the ship power
budget (GDD §5), so this decides both a core combat mechanic and a chunk of
the provisional HUD. Decided by the user 2026-08-15.

## Decision

**Elite-style pips.** A fixed pip budget distributed across three
subsystems — WEP / ENG / SYS(shields) — adjusted live with a few
keystrokes. Pip allocation scales weapon capacitor recharge, thrust/boost
envelope, and shield regeneration respectively. Ships differ in pip budget
and subsystem curves via defs, keeping handling-as-a-stat (GDD §4).

## Alternatives considered

**Module toggles** (X4-style per-module on/off) — finer-grained and suits
slower, managerial play, but it's menu-shaped in the middle of a dogfight.
Rejected: the GDD frames power management as "the pilot's tactical dial",
and a dial wants one-keystroke coarse triage, not a checklist.

## Consequences

- Phase 6 sim gains a power model: per-ship pip budget, three allocation
  targets, and def-driven response curves consumed by weapons (capacitor),
  flight (thrust/boost scaling — touches `ShipTuning`), and shields (regen
  scaling per 002).
- Keybinds and provisional HUD get a pips widget (arrow-key or 1/2/3-style
  allocation; exact binds decided at implementation).
- Ship defs grow power stats under the strict schema; NPC pilots get a
  simple pip policy per role (fighter: WEP-biased; trader: ENG-biased;
  fleeing: ENG/SYS) as part of Phase 6 AI.
