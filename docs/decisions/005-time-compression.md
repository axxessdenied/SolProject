# 005 — No time compression; tune cruise speed instead

- **Date**: 2026-08-15
- **Status**: accepted

## Context

GDD open question Q4, due at Phase 7: whether the game gets SETA-like
time compression outside combat (the GDD leaned yes — "needed at these
distances"). Systems are hundreds of thousands of km across; the question
is whether crossing them stays tolerable in real time. Decided by the
user 2026-08-15, against the GDD leaning.

## Decision

**No time compression. The game runs at 1× real time, always.** Travel
pacing is tuned through the cruise drive envelope (top speed, spool-up,
brake-off distance) and through system layout (how far apart the
generator places things people actually visit), so that a typical
in-system leg lands in the tens of seconds and a long one stays under a
few minutes.

## Alternatives considered

**SETA-like compression** (GDD leaning) — solves leg time but drags a
tail: every sim system (economy agents, NPC pilots at 2 Hz, projectiles,
regen rates) must be correct at 6–10× step scaling or visibly pop when
compression kicks in/out, and combat-interrupt rules need their own
design. Rejected by the user in favor of keeping one timescale.

**Defer until after flying Phase 7** — retrofitting compression into an
agent economy later is exactly the expensive path; deciding now keeps
the economy sim free to assume uniform dt.

## Consequences

- The whole sim assumes uniform real-time dt forever: economy agent
  scheduling, pilot think rates, regen/capacitor curves. This is a real
  simplification Phase 7 gets to bank on.
- Cruise (Phase 4, provisional until now) is confirmed as the in-system
  travel model; its numbers become first-class tuning levers in
  `game/data` and the Phase 7 generator must respect a "max leg time at
  cruise" budget when placing stations/gates relative to system scale.
- NPC traders cross systems on the same clock the player does; economy
  tick rates are chosen so prices move on a scale the player can watch.
- If playtesting later proves legs too long anyway, the recourse is
  faster cruise / tighter layouts, not compression — revisiting this
  decision requires reopening the uniform-dt assumption explicitly.
