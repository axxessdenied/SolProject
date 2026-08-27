# 013 — Translucent draws are sorted back to front, and the pass gives up its material bucketing to get it

- **Date**: 2026-08-27
- **Status**: accepted

## Context

Phase 12 shipped alpha-blended meshes with no back-to-front sort, and wrote the
reason down in `scene_renderer.cpp` rather than leaving it implied:

> The only translucent object in the game is one flat disc per gate, and gates
> are ~100,000 km apart … machinery guarding nothing today.

That was a good deferral and it was honest about being one. Phase 25's
diagnosis (finding 10) flagged that the phase would make it real, and the
spec handed the question to **stage C** with an instruction: *decide it rather
than discover it in a screenshot.*

**What changed underneath the deferral is not the picture but the premise.**
Stage B made a second translucent *material* a `[[material]]` row and no C++
at all. So "there is only one translucent thing" stopped being a fact about
the engine, which nobody could change without writing code, and became a fact
about today's shipped content, which an author or a mod can change with one
def row and no review. A deferral whose justification anyone can invalidate by
editing a data file is not a deferral any more.

**And the failure it was hiding is the worst-shaped kind.** Before this change,
translucent draws were recorded in *material index* order — the order rows
happen to sit in a file. Two blended surfaces overlapping on screen therefore
composited in an arbitrary order that no one chose, that changes when an
unrelated material is added above them, and that looks like a *lighting* bug
rather than an *ordering* one. Nobody files that bug correctly.

## Decision

**Translucent instances are sorted back to front, per instance, across
materials, and the pipeline is rebound only when the material actually
changes.** The opaque pass keeps its per-material bucketing exactly as stage B
built it.

The sort key is squared camera distance, descending, with the material index
breaking ties so the order is **total**. A total order matters beyond
tidiness: two membranes at exactly equal distance would otherwise sort
differently between standard-library implementations, and a frame that differs
by toolchain is a frame no A/B capture can measure against — which is this
project's main instrument.

## Why bucketing loses here and only here

Grouping draws by material exists to hold the bind count down. That argument is
strong in the opaque block, where every ship, station and rock in the bubble is
drawn and the bind count would otherwise be the object count.

It is weak in this pass, and the reasons are specific rather than a shrug:

- **A blended, depth-write-free draw is expensive per pixel and deliberately
  rare.** This is the pass you do not put a thousand objects in. The bind count
  here is bounded by something already small.
- **The worst case is a bind per draw; the shipped case is one bind.** Rebinding
  on *change* means a run of one material — several gate membranes, which is
  exactly what the galaxy contains — still costs a single bind.
- **Correctness has no cheaper substitute.** Sorting the *buckets* against each
  other by their nearest instance was considered and rejected: it keeps one
  bind per material and is still wrong whenever two translucent objects of
  different materials interleave in depth, which is precisely the case the sort
  exists for. It would have bought back the bind count by making the answer
  wrong in a narrower band — the worst trade available, because the remaining
  bug is harder to reproduce than the one it replaced.

## What was explicitly not done

- **No depth pre-pass, no order-independent transparency, no per-triangle
  sort.** Per-object sorting is wrong for a translucent object that overlaps
  itself; nothing in this game has one (the membrane is a flat disc). When
  something does, that is a different decision with a different price, and this
  one does not pretend to have covered it.
- **The pass position is unchanged.** Translucent draws still go after the sky
  and before the particles, for the reason Phase 12 recorded: the sky is a
  full-screen pass that survives wherever depth is still at the reversed-Z
  clear, so a blended draw recorded in the opaque block is painted over by it.
  That is a fact about the frame, not about sorting.
- **`translucent` still decides which pass a draw lands in**, rather than the
  blend mode doing it. Also unchanged, also from stage B.

## Consequences

- One `std::sort` per frame over a list that is empty in most frames. The list
  is a member, cleared rather than freed, so a steady state allocates nothing.
- `m_translucentBuckets` is gone; `m_translucentDraws` is flat and carries the
  material index per entry. The opaque side is untouched.
- `drawInstance` is now shared by both passes as a lambda, which keeps the
  property Phase 12 wrote its second loop by hand to preserve: the two passes
  cannot drift on LOD level selection or on the LOD report.
- **The remaining known gap, stated so it is not rediscovered as a surprise:**
  sorting is by object centre. Two large translucent surfaces that interpenetrate
  will still composite wrongly where they cross. Nothing in the game can reach
  that today, and fixing it is a per-triangle problem, not a per-object one.
