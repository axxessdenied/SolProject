# P1a — precision and orbit prototypes: evidence index

**Milestone:** [P1a — Precision and Orbit Prototypes](../../SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md)
**Owner:** Claude, single writer throughout
**Increments:** A1, A2, A3 — all complete
**Closed:** 2026-08-12, by the `complete-milestone` review recorded below

This is the single index the P1a plan's exit criteria require. It links every increment's raw
results, scenario definitions, toolchain metadata, accepted failures, and conclusions. The
per-increment indexes below are the detailed records; nothing here restates their numbers except
where a milestone-level claim needs one.

`evidence/**/raw/` is excluded by `.gitignore`. Every raw file is reproducible from the literal
commands in the increment handoffs; the indexes and handoffs are the durable record.

## Increments

| Increment | Scope | Evidence | Handoff |
|---|---|---|---|
| A1 | Measurement and build harness | [Index](A1/Index.md) | [Handoff](A1/Handoff.md) |
| A2 | Reference frames and numerical precision | [Index](A2/Index.md) | [Handoff](A2/Handoff.md) |
| A3 | Hybrid orbit and time warp | [Index](A3/Index.md) | [Handoff](A3/Handoff.md) |

## Toolchain and host

One machine, unchanged across all three increments, and recorded in every report's
`environment` section.

| Item | Recorded value |
|---|---|
| Compiler | MSVC 19.51.36252.0, `-std:c++latest`, `/fp:precise /arch:AVX2` |
| Contraction | `implicit-off-under-fp-precise`, proven by the A1 negative control |
| Build | CMake + Ninja, separate single-configuration Debug and Release presets (ADR 0001) |
| OS | Windows 10.0.26200 |
| CPU | 12th Gen Intel Core i7-12650H, 16 logical processors, AVX2 + FMA3 |
| Dependencies | **None.** P1a is deliberately dependency-free; no `vcpkg.json` exists (ADR 0007 untriggered) |

## Accepted thresholds, and where each was met

Every threshold the plan accepted has a reproducible result. **None was relaxed, waived, or
relabelled.**

| Accepted criterion | Owner | Measured | Verdict |
|---|---|---|---|
| Frame conversion round trip: ≤ 1 mm position, ≤ 1 µm/s velocity at the surface anchor | A2 | 4.796 µm, 0.240 nm/s (hierarchical); 4.910 µm, 0.357 nm/s (flat) | **Met by both candidates** |
| Conversion drift: 10⁶ conversions bounded and non-accumulating | A2 | bounded, and idempotent after the first conversion | **Met** |
| Local ↔ analytical handoff: ≤ 1 m position, ≤ 1 mm/s velocity | A3 | **exactly 0** (state-anchored); 0.88 µm / 2.2 × 10⁻¹⁰ m/s (element-anchored) | **Met** |
| Reference orbit: one 200 km period within 100 m of Kepler | A3 | 44.6 m at a 64 s RK4 step; 0.5 mm at 4 s | **Met** |
| Warp equivalence within the handoff and reference-orbit tolerances | A3 | **bit-identical** (anchored coast, and aligned local ticks); 1.0 mm (stepped coast) | **Met** |
| Determinism: event ordering and bit-identical output on the same build | A1, A2, A3 | bit-identical across separate process launches; crossing instants identical to the nanosecond | **Met** |
| Peak memory and allocation counts recorded (mandatory) | all | 4.8–4.9 MB peak working set; allocation-free propagation paths | **Met** |

One case is **measured but deliberately not gated**, and is named rather than dropped: a marginal
sphere-of-influence capture has no well-conditioned crossing time. See A3 Finding 6 and the
exception recorded in ADR 0011.

## Architectural dispositions

P1a succeeds by selecting, constraining, or rejecting candidates. It produced four selections and
left one question deliberately open:

| Question | Disposition | Recorded in |
|---|---|---|
| Frame/origin model | **Selected:** hierarchical parent-relative frame graph | [architecture.md](../../docs/architecture.md#selected-reference-frame-model) |
| Local↔analytical transition contract | **Selected:** one authoritative regime, state-anchored coast, refined crossing events, named rejections | [architecture.md](../../docs/architecture.md#selected-hybrid-propagation-and-transition-contract) |
| Local numerical integrator | **Selected:** RK4 | A3 Index; architecture.md |
| Earth atmosphere limit | **Selected for P1a:** 140 km, tuning deferred to P2/M5 | [ADR 0011](../../docs/decisions/0011-gravity-and-orbit-baseline.md) |
| Coast anchor representation | **Open, and safe either way** — both measured six orders of magnitude inside tolerance | A3 Index |

**ADR 0010 was amended** (contraction wording) and **ADR 0008 was amended** (DE440/DE441 family;
anchor defined against the reference ellipsoid) from A1 and A2 evidence. **ADR 0011 was confirmed
from measurement**, with one wording correction to match how the sphere-of-influence radii are
actually obtained.

## Accepted failures and limitations

Nothing failed a gate. These are the limitations P1a records so that P1b and P2 design around
them rather than discover them:

- **Earth orientation is IAU_EARTH, not ITRF** — no nutation, polar motion, or UT1−UTC. An
  orientation model, not a propagation model; unresolved by A3 (A2, A3).
- **The Sun's barycentric motion remains linear**, because ADR 0011 gives the Sun no
  gravitational primary. Affects only the `SsbIcrf`→`SunIcrf` boundary, which nothing P1a gates
  on crosses (A3).
- **The ephemeris is a three-body hierarchy** — Sun, Earth, Moon. No P1a number covers a body
  outside it (A3).
- **The campaign clock's seconds conversion is exact only to 2^53 ns = 104.25 days.** A3's
  longest run is at 96% of that window, so every P1a number is exact; a multi-year campaign is
  not, and needs a coarser tick, a split representation, or anchor-relative elapsed times.
  Determinism is unaffected either side of the boundary — only exactness of representation is.
  `OrbitSelfCheck` pins the boundary directly (A3).
- **A boundary excursion shorter than the 1 ms subdivision floor can still be missed.** Runs
  report when they touched the floor (A3).
- **The hysteresis band is a chosen constant**, tuned against chattering and not against
  gameplay (A3).
- **Single-machine evidence.** ADR 0010 promises only same-machine bit-exactness, so this is in
  scope, but the four cross-run goldens will need tolerance-based equivalents the first time the
  suite runs on other hardware.
- **The suite does not catch what reading the output catches.** Across A2 and A3, six defects
  were found by inspecting implausible numbers rather than by a failing check. Each got a
  specific check afterwards, which does not generalise. This is P1a's weakest methodological
  point and is stated rather than smoothed over.

## Disposable code

Everything under `prototypes/p1a/` is disposable by the P1a plan. Nothing has been promoted, and
promotion requires an explicit review rather than file copying.

| Path | Durability |
|---|---|
| `Harness/` | Domain-free by design; the most promotable, and the plan anticipated it |
| `Frames/` | Carries domain concepts; promotion is a real decision |
| `Orbit/` | Carries domain concepts. `Stumpff`, `KeplerPropagator`, `OrbitalElements`, and `CampaignClock` are the most likely to survive; `HybridPropagator`'s *design* matters more than its code; `BodySystem` and `ConicEphemeris` are the most likely to be replaced |

No prototype was rejected, so the plan's rule about removing rejected code did not apply. One
target, A1's `TimingScenario`, was removed with user approval once `FrameModelCost` superseded it.

## Milestone review, 2026-08-12

`complete-milestone` was run against this milestone and its evidence. It re-ran both
configurations from the checked-in presets and independently re-derived the reported numbers
rather than reading them from the indexes.

| Check | Result |
|---|---|
| Release configure, build, `ctest` | **20/20 passed** |
| Debug configure, build, `ctest` | **20/20 passed** |
| `OrbitSelfCheck` | 122 checks, 0 failed |
| Scenario re-run vs. recorded raw output | `HybridHandoff`, `SoiCrossing`, `WarpEquivalence` byte-identical; `ReferenceOrbit` identical except its timing fields |

The review raised eight findings. **None invalidated a threshold result or reversed a
selection**, and all eight were resolved before closure:

| # | Finding | Resolution |
|---|---|---|
| 1 | README still said implementation had not started and had no build instructions | Rewritten with current status and the literal build commands |
| 2 | No milestone-level evidence index, which the plan's exit criteria require | This document |
| 3 | The campaign clock's 104-day exactness window was recorded only in a header comment | Surfaced as a limitation above and in A3's handoff; `OrbitSelfCheck` now pins the boundary with 4 checks |
| 4 | Velocity Verlet's cost was overstated 2× — the stateless API's count was quoted as the integrator's | `minimumAccelerationEvaluationsPerStep` added and reported alongside; the relative-cost conclusion now reads **16×**, not 32×. RK4's selection is unaffected |
| 5 | `rebaseTo` did not itself re-check eligibility; the guarantee held only on `advanceTo`'s path | Re-check moved into `rebaseTo`, so every caller gets it. Behaviour unchanged — all A3 results byte-identical |
| 6 | ADR 0011 said sphere-of-influence radii are "recorded as fixtures"; they are computed at load | ADR corrected to describe what is actually done |
| 7 | Sun and Moon carried a zero surface radius, silently disabling the below-surface rejection for both — on the mistaken belief the pinned kernel lacked them | Both now read from `BODY10_RADII` and `BODY301_RADII`. No scenario result changed, so nothing had been relying on the gap |
| 8 | `meanRadiusMetres` held the equatorial radius | Renamed `surfaceRadiusMetres`, with the equatorial choice and its conservative direction documented |

Findings 4, 5, 7, and 8 changed code, so A2's and A3's raw evidence was regenerated. **No
physical number moved in either increment.** The only deltas are A3's `ReferenceOrbit` narrative
and its new evaluation-count fields, and a +6 allocation / +168 byte increase in A2's
`ReferenceFixtures` from parsing three extra kernel entries at load. Both are recorded as dated
addenda in the affected indexes.

## State

P1a is **implementation-complete, reviewed, and integrated into `dev`**. It is **not released**.

| State | Reached | How |
|---|---|---|
| Implementation-complete | 2026-08-12 | A1, A2, and A3 closed against their accepted thresholds |
| Reviewed | 2026-08-12 | `complete-milestone`, recorded above |
| Integrated into `dev` | 2026-08-13 | PR #3 from `feature/p1a-hybrid-orbit-and-warp`, merged as `bf18c33` |
| Released | — | No tag, nothing on `main`, no release artifact exists |

The three commits are `d314dad` (A3 as delivered), `1e8f6ea` (milestone closure), and the merge
`bf18c33`. A clean rebuild of `dev` passes 20/20 in both configurations. Tagging and any
promotion to `main` remain separate explicit user requests under `AGENTS.md`.
