# P1a increment A3 — handoff record

Required at increment closure by the P1a milestone plan and the `AGENTS.md` lightweight-lane
rule.

## Goal and outcome

Build a deterministic headless two-body scenario under ADR 0011; compare candidate fixed-step
local integrators against a Kepler analytical coast; define explicit eligibility rules and a
bidirectional handoff; handle sphere-of-influence entry and exit with defined state ownership;
run normal-time and accelerated coasts of the same 200 km orbit over the same elapsed campaign
time; and report position, velocity, orbital elements, conserved quantities, transition counts,
and event chronology.

**Outcome: achieved.** Every A3 done criterion has a reproducible result. The transition contract
is selected and recorded in [`docs/architecture.md`](../../../docs/architecture.md). Six findings
are recorded in [Index.md](Index.md); three were defects in A3's own code or claims, found and
fixed before the numbers were treated as evidence. **No open decision requires a user ruling.**

## Owner, branch, base

- Single writer: Claude.
- Branch **`feature/p1a-hybrid-orbit-and-warp`**, branched from `dev` at commit `1774f85`, at the
  user's explicit request. Every report accordingly records `gitCommit: 1774f855…` with
  `gitDirty: true`; re-running the evidence commands after a commit produces reports with
  `gitDirty: false` and identical numbers.
- **Scope note.** `docs/project_status.md` recorded A3 as planned but not authorized. The user's
  direct instruction to start A3 is taken as the authorization, on the same basis as A2, and the
  status document is updated to say so. Commit, push, merge, tag, and pull request remain
  unrequested and were not performed.
- The same document also claimed A2 was uncommitted in the `dev` working tree. That was stale —
  A2 landed as PR #2, commit `8e96d2a` — and has been corrected.

  > **Superseded, 2026-08-13.** The bullets above record A3's state at its closure and are kept
  > as written. A3 was subsequently committed as `d314dad`, the milestone review's corrections as
  > `1e8f6ea`, and both merged into `dev` as PR #3 (`bf18c33`); the numbers did not change.
  > Current milestone state belongs to [`docs/project_status.md`](../../../docs/project_status.md),
  > not to this record.

## Changed files

New:

```
prototypes/p1a/Orbit/CMakeLists.txt
prototypes/p1a/Orbit/include/Sol/Proto/Orbit/{BodySystem,CampaignClock,ConicEphemeris,
  HybridPropagator,Integrator,KeplerPropagator,OrbitalElements,Stumpff,TwoBody}.h
prototypes/p1a/Orbit/src/{BodySystem,CampaignClock,ConicEphemeris,HybridPropagator,Integrator,
  KeplerPropagator,OrbitalElements,Stumpff,TwoBody}.cpp
prototypes/p1a/{OrbitSelfCheck,ReferenceOrbit,HybridHandoff,SoiCrossing,WarpEquivalence}/Main.cpp
evidence/p1a/A3/{Index.md,Handoff.md}
```

Modified: `prototypes/p1a/CMakeLists.txt`, `docs/project_status.md`, `docs/architecture.md`,
`docs/changelog.md`, `docs/decisions/0011-gravity-and-orbit-baseline.md`,
`SolProjectNotes/Open-Questions.md`.

Modified during the P1a milestone review: `README.md`, `docs/decisions/README.md`,
`prototypes/p1a/Frames/{include/Sol/Proto/Frames/ReferenceData.h,src/ReferenceData.cpp}`, and
A3's own `Integrator`, `TwoBody`, `BodySystem`, `HybridPropagator`, `CampaignClock`,
`OrbitSelfCheck`, and `ReferenceOrbit`. Added: `evidence/p1a/Index.md`.

Deleted: none.

**A2's origin-motion model is untouched.** That is deliberate. A3 replaces A2's linear
origin-motion model, but it does so by adding `ConicEphemeris` in the orbit library rather than by
editing `frames::buildSnapshot`. Editing it would have changed the numbers A2's committed
evidence reports, and the replacement is more useful as a measured comparison than as a silent
substitution: `OrbitSelfCheck` asserts that the A3 snapshot leaves A2's launch-site boundary
bit-identical while changing Earth's origin, so any A2-to-A3 difference is attributable to origin
motion alone.

**One additive change to `Frames/` was made during the P1a milestone review**, and it is recorded
here rather than folded in silently. `ReferenceData` now also parses `BODY10_RADII` and
`BODY301_RADII` from the already-pinned planetary-constants kernel and exposes
`equatorialRadiusMetres(naifId)`, because `BodySystem` had been leaving the Sun's and the Moon's
surface radii at zero on the mistaken belief the kernel did not carry them — which silently
disabled the below-surface eligibility rejection for both bodies. No conversion, transform, or
frame boundary was altered. A2's scenarios were re-run to confirm it: `FrameRoundTrip`,
`AscentSampling`, and `PrecisionBudget` are byte-identical, and `ReferenceFixtures` differs only
by **+6 allocations and +168 bytes** from the three extra kernel lookups at load. No A2 physical
number moved.

**Dependency changes: none.** P1a remains dependency-free and no `vcpkg.json` exists, so ADR
0007's workflow was not triggered. A3 adds no fixtures either; it uses A2's pinned ADR 0008 data
unchanged.

## Literal commands

Environment as recorded in A1's handoff: `INCLUDE` and `LIB` were already present in the shell.
On a shell without them, run `Launch-VsDevShell.ps1 -Arch amd64` first.

```powershell
# Clean tree
Remove-Item -Recurse -Force build

# Debug
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug

# Release
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release

# Evidence capture
foreach ($cfg in @("release","debug")) {
  $dir = "build\windows-msvc-$cfg\prototypes\p1a"
  foreach ($s in @("ReferenceOrbit","HybridHandoff","SoiCrossing","WarpEquivalence")) {
    & "$dir\$s.exe" --out "evidence\p1a\A3\raw\$cfg-$s.json"
  }
}
```

Each scenario also accepts `--fixtures <dir>` to override the fixture root baked in at configure
time.

## Results

```
Debug   : 100% tests passed out of 20    (42.8 s)
Release : 100% tests passed out of 20    ( 9.0 s)
```

Twelve tests from A1 and A2, plus eight added by A3: `OrbitSelfCheck`, `ReferenceOrbit`,
`HybridHandoff`, `SoiCrossing`, `WarpEquivalence`, and the three cross-run determinism goldens
`HybridHandoffAcrossRuns`, `SoiCrossingAcrossRuns`, and `WarpEquivalenceAcrossRuns`.

`OrbitSelfCheck` alone runs 122 checks; `SoiCrossing` runs 153. The A1 caveat still applies: do
**not** run `ctest --preset windows-msvc-release-negcontrol-contract`, which fails
`ToolchainReport` by design.

Hardware, toolchain, thresholds, and summary metrics are tabulated in [Index.md](Index.md).

## Failed or waived criteria

None. No threshold was relaxed and no criterion was waived.

**One case is measured but deliberately not gated**, and this is a scope statement rather than a
waiver. `SoiCrossing`'s `lunarMarginalCapture` — an approach slower than lunar escape speed at
the boundary, captured into an orbit that grazes the sphere from inside — has crossings that are
ill-conditioned by construction. Its *state* discontinuities are gated and met like every other
case; only tick-invariance of the crossing *instants* is exempt, because the trajectory has no
well-conditioned crossing time for any scheme to reproduce. Finding 6 in [Index.md](Index.md)
records why, and the residual question is listed in
[Open Questions](../../../SolProjectNotes/Open-Questions.md).

Five defects were found in A3's own code and claims and fixed before the numbers were treated as
evidence. Three were caught by reading output rather than by a test, which is the same limitation
A2's handoff recorded and which A3 did not close:

1. **A self-check measured a function's slope instead of its accuracy.** The Stumpff continuity
   check sampled the series and closed forms at two *different* arguments either side of the
   threshold and differenced them, which measures dC/dz. Rewritten to compare the two forms at
   one argument.
2. **A convergence-order check measured two noise samples.** Integrator order was measured over
   100 s at 1 s and 0.5 s steps, where RK4's truncation error is *below* the Kepler reference's
   own 1e-13 relative rounding. The reported ratio was noise over noise. Moved to 800 s at 16 s
   and 8 s steps, four orders of magnitude above the floor.
3. **`rebaseTo` did not re-check coast eligibility after a change of primary.** A state that is a
   valid conic about Earth can be radial about the Moon. The propagator coasted on a degenerate
   conic and produced a sphere-of-influence exit 1 679 s after an entry that physically required
   130 000 s. Found by noticing the number was impossible, not by a test. Finding 2.
4. **The crossing predicate was not a pure function of the instant.** Warp factors disagreed about
   when a boundary was crossed by up to 1 387 s over a three-day escape. Finding 3.
5. **`ReferenceOrbit` reported 131 ns per acceleration evaluation for its first configuration**
   against about 30 ns for identical work in every later one — a cold cache, not a slow
   integrator, and the same class of defect as A1's contraction probe and A2's zero-nanosecond
   rebuild. A warm-up run was added and recorded in the metadata.

A sixth item is a corrected claim rather than a code defect, and is the one most worth carrying
forward: **`WarpEquivalence` asserted that the local numerical regime could not be warp-invariant,
and its own measurement contradicted the assertion.** Finding 5. The narrative was rewritten and
the unaligned-tick case added, which turned a wrong warning into a usable constraint.

## Remaining risks

- **The Sun's barycentric motion is still linear.** ADR 0011 gives the Sun no gravitational
  primary, so there is no conic to propagate — its barycentric wobble is driven by the very
  perturbations the ADR excludes. This affects only the `SsbIcrf`-to-`SunIcrf` boundary, which
  nothing A3 gates on crosses: every propagation result is geocentric or Moon-relative, and the
  sphere-of-influence hierarchy is rooted at the Sun for this reason. It is stated in
  `ConicEphemeris`'s own header rather than left to be discovered.
- **Earth orientation is still IAU_EARTH, not ITRF.** Unchanged from A2 — no nutation, polar
  motion, or UT1−UTC. A3 does not close it; it is an orientation model, not a propagation model.
- **The ephemeris is a three-body hierarchy.** Sun, Earth, Moon. Adding planets is mechanical, but
  no A3 number should be quoted as covering a body that is not in it.
- **A boundary excursion shorter than the subdivision floor can still be missed.** Conservative
  stepping proves a boundary is unreachable within an increment, but bottoms out at 1 ms so that
  a craft tangent to a boundary terminates. `incrementsAtSubdivisionFloor` reports when a run
  touched the floor; every gated run did, between 1 and 4 times.
- **The hysteresis band is a chosen constant.** 10⁻⁴ of each sphere radius — 92 km at Earth,
  6.6 km at the Moon. It is defensible because the Laplace radius is a switching convention
  rather than a physical surface, but it has not been tuned against gameplay, only against
  chattering.
- **The campaign clock's seconds conversion is exact only to 2^53 ns = 104.25 days.** The clock
  accumulates as an exact integer without bound, but `seconds()` converts through a double, which
  stops resolving individual nanoseconds past that point. A3's longest run — the 100-day no-decay
  coast — sits at 96% of the window, so every number in this increment is exact. A campaign is
  measured in years and will not be. Determinism is unaffected either side of the boundary, since
  the same integer always converts to the same double; what degrades is exactness of
  representation. A production clock needs a coarser tick, a split representation, or elapsed
  times taken against a moving anchor rather than the campaign epoch. Added by the P1a milestone
  review, which found this recorded only in `CampaignClock.h`; `OrbitSelfCheck` now pins the
  boundary with four checks.
- **Single-machine evidence.** One i7-12650H. ADR 0010 promises only same-machine bit-exactness,
  so this is in scope, but A3 now pins three cross-run goldens that will need tolerance-based
  equivalents the first time this runs on other hardware — the same debt A2 recorded, now larger.
- **The suite still does not catch what reading the output catches.** Three of five defects above
  were found by inspection. A3 added checks for each specific case, which does not generalise.

## Disposable code

`prototypes/p1a/Orbit/` is the substantial new code, and like `Frames/` it is **not** free of
domain concepts — it knows about conics, spheres of influence, atmosphere limits, and campaign
time — so promoting it is a larger decision than promoting the harness, not a smaller one.

The parts most likely to survive review are `Stumpff`, `KeplerPropagator`, `OrbitalElements`, and
`CampaignClock`: they are self-contained, heavily checked, and implement well-specified
mathematics. `HybridPropagator` encodes the transition contract and is the piece whose *design*
should be reused even if the code is not. `BodySystem` and `ConicEphemeris` are the most likely
to be replaced, because a production ephemeris will need more bodies and probably a queryable
kernel rather than a three-body conic hierarchy anchored at one fixture epoch.

Nothing was deleted. No prototype was rejected, so the P1a rule about removing rejected code did
not apply.

## Smallest next action

**P1a's exit criteria are met and the milestone is closed.** A1, A2, and A3 are complete, every
accepted threshold has a reproducible result, ADR 0011 is confirmed from evidence, and
[`evidence/p1a/Index.md`](../Index.md) links each increment's raw results, scenario definitions,
toolchain metadata, accepted failures, and conclusions.

The `complete-milestone` review ran on 2026-08-12. It re-ran both configurations (20/20 in each),
re-derived the reported numbers from fresh output, and raised eight findings — four against this
increment, all resolved before closure, none invalidating a threshold or reversing a selection.
The review record is in the milestone index.

P1a is **implementation-complete, reviewed, and integrated into `dev`** — merged on 2026-08-13
as `bf18c33` via PR #3, with this increment landing as `d314dad` and the review's corrections as
`1e8f6ea`. It is **not released**: nothing is tagged and nothing is on `main`.

The smallest next action is a decision about P1b, which remains **not** authorized and needs an
explicit authorization of its own naming its single writer and branch.

Four questions are handed to P2/M5 rather than answered here, and each is recorded in
[Open Questions](../../../SolProjectNotes/Open-Questions.md):

| Question | What A3 established |
|---|---|
| Powered warp | Not a determinism problem, as A3 first assumed. Warp must be **quantised** so offered factors divide the local step. Whether powered warp is desirable for control-authority reasons is untouched. |
| Attitude control under warp | Untouched. A3 models no attitude, no torque, and no control authority; the craft is a point mass throughout. |
| Encounter prediction | Untouched as a *feature*. The machinery exists — crossings are found by bisection on the conic and are reproducible to the nanosecond — but predicting an encounter ahead of time is a search over future conics, which A3 does not perform. |
| Final gameplay tolerances | A3's tolerances are engineering tolerances met by six orders of magnitude. What a *player* can perceive, and what a contract should require, is a different question and is not answered by any number here. |

A fifth is new, and belongs with them: **what the simulation should do about a marginal capture.**
Finding 6 shows these trajectories have no well-conditioned crossing time, which is a property of
the physics rather than of the implementation. A game offering lunar capture will produce them,
and the answer is most likely a deliberate rule about when the simulation commits to a capture
rather than a numerical refinement.
