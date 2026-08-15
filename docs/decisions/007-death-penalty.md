# 007 — Death penalty: insurance deductible + opt-in hardcore

- **Date**: 2026-08-15
- **Status**: accepted

## Context

GDD open question Q6, due at Phase 8: how harsh death is. The GDD
leaned "insurance default + optional hardcore"; Phase 7 shipped the
respawn-at-last-dock rule with the cost explicitly deferred to
outfitting (which defines what a ship and its fit are worth). Decided
by the user 2026-08-15, confirming the leaning.

## Decision

**Default mode**: ship destruction respawns the player at the last
dock in the same ship with its full fit and crew, cargo is lost, and an
**insurance deductible** is charged: a fixed fraction of the replacement
value (ship hull price + fitted module prices). If the player cannot
cover it, credits clamp at zero — no debt system in v1.

**Hardcore (ironman)**: an opt-in flag at new-game. Death permanently
deletes the save; the run is over. No middle modes in v1.

## Alternatives considered

**Lose uninsured ship** (insurance as a purchasable policy; die without
one and restart in a starter ship) — more EVE-flavored risk texture,
but adds a policy-management chore and a brutal failure mode the GDD's
influences table explicitly leaves behind ("full-loot harshness").

**Minimal penalty** (flat fee, no insurance concept) — simplest, but
death stops mattering as wealth grows; a percentage of fit value scales
with the player automatically.

## Consequences

- Death cost scales with what you fly: upgrading your fit raises your
  deductible — a natural risk/reward dial with zero extra design.
- Cargo loss keeps trading runs tense without threatening progression.
- The deductible fraction is a tuning constant (game side); flag it as
  a lever, not a promise.
- Hardcore needs new-game plumbing (flag in the save header) and a
  deliberate save-delete path — it must never trigger in default mode.
- No debt system means no death spiral; revisit only if playtesting
  shows zero-credit respawns feel consequence-free.
