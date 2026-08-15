# 004 — Travel model: jump gates baseline, jump drive as late-game unlock

- **Date**: 2026-08-15
- **Status**: accepted

## Context

GDD open question Q1, due at Phase 7 (Universe & Economy): whether
inter-system travel is gates-only or players can eventually own a jump
drive for free point-to-point travel. This shapes the galaxy graph's
meaning (regions, chokepoints, trade lanes), the economy's geography, and
whether Phase 4's provisional cruise mode is the real in-system travel
layer. Decided by the user 2026-08-15.

## Decision

**Jump gates are the baseline network; a player-owned jump drive is a
late-game unlock.** Phase 7 builds gate travel only. The galaxy generator,
save format, and travel code treat the gate graph as authoritative, but
nothing may assume "gates are the only way a ship changes system" — the
drive arrives later (Phase 8+ outfitting/progression) as free travel that
bypasses the graph.

## Alternatives considered

**Gates only, forever** — simplest, but forecloses a marquee late-game
power fantasy the GDD already leans toward.

**Jump drives from the start** — free travel early dissolves the
core/frontier/fringe gradient and the economy's chokepoints; the gate
graph would be scenery.

## Consequences

- Phase 7 scope: gate entities placed by the generator, gate-transit flow
  (approach → activate → system swap via sim-LOD promotion/demotion),
  galaxy graph as the pathing substrate for NPC traders.
- In-system travel is cruise drive (Phase 4), now no longer provisional —
  see `005-time-compression.md` for the pacing consequence.
- System-change machinery must take a (from, to) pair, not a gate pair,
  so the later drive reuses it; gate-specific logic stays at the edges.
- The drive itself (cost, charge time, range, fuel?) is specced at
  Phase 8 outfitting; no engine work now beyond not painting over the door.
