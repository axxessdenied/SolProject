# 006 — Crew: trivial passive-bonus version in v1

- **Date**: 2026-08-15
- **Status**: accepted

## Context

GDD open question Q5, due at Phase 8: whether crew/officers exist in v1.
The GDD leaned "post-v1 unless trivial". Decided by the user 2026-08-15
while scoping the Phase 8 outfitting spec (crew shares its machinery).

## Decision

**Crew ships in v1, but only the trivial version**: crew members are
hired at stations for a one-time cost and grant flat passive stat
bonuses (e.g. +shield regen, +weapon recharge) while aboard. A ship has
a fixed number of crew berths from its def. No personalities, no
management, no leveling, no wages, no crew death mechanics.

Mechanically, crew members are data-driven defs using the same stat
modifier vocabulary as outfitting modules — a crew member is effectively
a module that occupies a berth instead of a slot. That reuse is what
makes the feature trivial enough to clear the GDD's own bar.

## Alternatives considered

**Post-v1 entirely** (GDD leaning) — safest scope, but once outfitting
has a stat-modifier system, berths + hire cost is a data file and a UI
tab, and it adds a progression axis for nearly free. Rejected because
the marginal cost dropped below "trivial".

**Officers with personalities/skills (Starsector-style)** — real design
surface (leveling, loyalty, permadeath interplay with Q6). Out of scope
for v1; nothing in the trivial version blocks upgrading later.

## Consequences

- The outfitting spec (engine plan Phase 8a) includes crew berths on
  ship defs, a `[[crew]]` def type sharing module modifier keys, and a
  hire/dismiss UI at stations.
- Save format carries the hired crew per owned ship.
- Any richer crew system post-v1 must grow out of this data model or
  consciously replace it.
