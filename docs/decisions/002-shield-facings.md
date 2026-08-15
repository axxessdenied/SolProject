# 002 — Shields have directional facings

- **Date**: 2026-08-15
- **Status**: accepted

## Context

GDD open question Q2, due at Phase 6 (Combat & AI): are shields one
regenerating bubble around the ship, or split into facings the pilot protects
by positioning? Decides the shape of the damage model (engine plan Phase 6)
before the first weapon lands a hit. Decided by the user 2026-08-15.

## Decision

**Directional facings.** Shields are split per facing (baseline: fore and
aft; ships may define more, e.g. lateral facings on capitals) with
independent strength and regeneration per facing. Damage applies to the
facing the hit arrives through; a collapsed facing exposes armor/hull on
that side while the others still hold.

## Alternatives considered

**Single bubble** — simpler to implement and to read on the HUD, but it
makes positioning irrelevant to defense, and the GDD's combat pillar is
readable, *positional* fighting (§5). Rejected for flattening the skill
dimension the rest of the defense stack (locational armor, systems damage)
is built around.

## Consequences

- The damage model resolves hit direction into a facing before applying
  shield absorption; the flight model's orientation data feeds combat.
- Ship defs (`game/data/ships.toml`) grow shield stats per facing (count,
  strength, regen); the strict def schema and HUD both need facing-aware
  display (Phase 6 provisional HUD: at minimum fore/aft rings).
- NPC steering behaviors can meaningfully "roll a fresh facing" — an AI
  behavior worth having, and a tax on AI simplicity we accept.
- Power management (Q3, `003-power-management.md`) modulates regen across
  all facings, not per facing — pips stay a three-way dial.
