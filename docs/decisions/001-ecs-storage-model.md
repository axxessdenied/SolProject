# 001 — ECS storage model: sparse-set

- **Date**: 2026-08-15
- **Status**: accepted

## Context

The engine plan (§2.5) deferred the ECS storage design to a Phase 3 spike:
archetype/SoA vs sparse-set, decided by benchmarking our actual access
patterns — thousands of ships/projectiles iterated linearly per sim tick,
sparse component churn on projectiles, partial-component queries, random
handle lookups. The spike lives in `engine/test/bench/ecs_storage_bench.cpp`
(target `sol_ecs_storage_bench`): two minimal but honest prototypes driven
through one shared workload driver so both execute identical operation
sequences (verified by matching end-state counts and checksums).

## Decision

**Sparse-set storage**: one set per component type — dense value array +
dense entity array + entity-indexed sparse redirection table; O(1) add,
swap-and-pop remove; queries iterate the smallest set's dense arrays and
probe the others through hoisted sparse-table pointers.

## Benchmark results

10,000 ships (2,000 shielded) + 2,000 projectiles, dt = 1/60 s, median of 240
measured ticks per workload; MSVC 19.51 RelWithDebInfo, i7-12650H pinned to
one P-core at high priority (unpinned runs wander ±40% from hybrid-core
scheduling). Representative run:

| workload | sparse-set | archetype | verdict |
|---|---|---|---|
| integrate 12k pos+vel / tick | 22.7 µs | 22.3 µs | parity |
| shield regen 2k of 12k / tick | 3.8 µs | 3.7 µs | parity |
| projectile churn ~500 spawn+destroy / tick | 17.0 µs | 12.4 µs | archetype ~1.35x |
| shield add+remove 256 / tick | 1.2 µs | 4.0 µs | sparse-set ~3x |
| random position lookup ×100k | 204 µs | 224 µs | sparse-set ~1.1x |
| shield regen after churn (5,320 shields) / tick | 10.3 µs | 10.3 µs | parity |

Key observations:

- **Linear iteration is a tie at our scale.** Archetype's theoretical edge
  (no probing) doesn't materialize: component sets created together stay
  membership-aligned in sparse-set, so probes are coherent; and even after
  heavy churn randomizes probe order, the working set (~300 KB of positions)
  sits in L2 and the partial query stays at parity (~1.9 ns/shield both).
- **Structural churn splits.** Whole-entity spawn/destroy slightly favors
  archetype (one row move vs one remove per component set); component
  add/remove favors sparse-set 3x because archetype must relocate the whole
  row to another table. Both are microseconds per tick either way.
- **Every workload is far inside budget**: the worst full tick simulated here
  costs ~50 µs of a 16.6 ms frame at 12k entities.

## Alternatives considered

**Archetype/SoA** (entities grouped by signature into tables of parallel
columns). Lost because the measured performance is a wash on our workloads
while the implementation cost is decidedly not: it needs type-erased column
storage, an archetype graph/lookup, and row-relocation machinery on every
add/remove — the exact operation gameplay code does casually (status effects,
weapon states, tags). Sparse-set is also a better fit for the rest of the
plan: per-component arrays serialize directly (the ECS is the save-game
backbone), dense ranges chunk trivially for the Phase 3 job system, and
per-set iteration keeps queries simple. Tag-heavy filtering, archetype's
other selling point, is served in sparse-set by leading queries with the
smallest set.

## Consequences

- The ECS proper (handles, queries, command buffers — Phase 3) is built on
  sparse-sets per component type; entities are generational handles indexing
  the sparse tables.
- Queries must lead with the smallest participating set; the query API should
  make that automatic (pick the set with fewest elements at iteration start).
- If profiling at much larger scales (100k+ components, partial queries
  falling out of L2) shows probe misses dominating, the escape hatch is
  sorting/grouping owned sets (EnTT-style groups) — an optimization inside
  this design, not a storage rewrite.
- The spike benchmark stays in-tree as a harness for validating such future
  storage optimizations against the same workloads.
