# P1a increment A3 — evidence index

**Increment:** A3, hybrid orbit and time warp
**Owner:** Claude (single writer)
**Branch / base:** `feature/p1a-hybrid-orbit-and-warp`, branched from `dev` — see [Handoff.md](Handoff.md)
**Date:** 2026-08-12
**Result:** All A3 done criteria met. The transition contract is selected. Six findings recorded;
three were defects in A3's own code or claims, found and fixed before the numbers were treated as
evidence. **No open decision requires a user ruling.**

Raw measurement output lives in `raw/`, which `.gitignore` excludes. It is reproducible from the
commands in [Handoff.md](Handoff.md); this index and the handoff are the durable record.

## Headline

**Every accepted A3 threshold is met with between three and eight orders of magnitude of
margin, and the interesting results are not the margins.**

Three of them decide architecture:

1. **The local-to-analytical handoff is exactly lossless** — zero, bit for bit, at every sampled
   orbital phase — because the coast anchors on the state it is handed rather than recomputing
   it. The alternative worth having, anchoring on classical elements, costs **0.88 µm at worst**
   and reaches a bitwise fixed point within six transitions, so it does not random-walk either.
   The choice can be made on storage and inspectability rather than on numerical fidelity.
2. **An anchored analytical coast is warp-invariant exactly, not approximately** — bit-identical
   final states across warp granularities spanning four orders of magnitude — because it never
   composes increments. The stepped alternative is not, and buys nothing for the difference.
3. **The local numerical regime is warp-invariant exactly when the warp tick is an integer
   multiple of the fixed local step.** This corrects a claim A3 first asserted and its own
   measurement contradicted; see Finding 5. It converts a vague warning about powered warp into
   a concrete constraint on which warp factors may be offered.

And one closes ADR 0011's own validation item: a 200 km circular orbit shows **no secular
altitude change over 100 days of warped coast** — semi-major axis moves 5.8 µm, which is
rounding, not decay.

## Toolchain and host

Unchanged from A1 and A2, and recorded in every report's `environment` section.

| Item | Recorded value |
|---|---|
| Compiler | MSVC 19.51.36252.0, `-std:c++latest`, `/fp:precise /arch:AVX2` |
| Contraction | `implicit-off-under-fp-precise` (A1 Finding 2) |
| OS | Windows 10.0.26200 |
| CPU | 12th Gen Intel Core i7-12650H, 16 logical processors, AVX2 + FMA3 |
| Peak working set | 4.81–4.94 MB across the four scenarios |
| Allocations in measured regions | 22–105 allocations, 2.0–11.2 KB total |

Allocation counts are low because every propagation path is allocation-free; what the counters
see is report construction.

## What was built

| Path | Role |
|---|---|
| `prototypes/p1a/Orbit/` | Conics, Stumpff functions, three integrators, sphere-of-influence hierarchy, conic ephemeris, integer campaign clock, hybrid propagator |
| `prototypes/p1a/OrbitSelfCheck/` | Guards the orbit library. **118 checks** |
| `prototypes/p1a/ReferenceOrbit/` | The accepted 100 m one-orbit gate, and the long-run energy behaviour it cannot see |
| `prototypes/p1a/HybridHandoff/` | The accepted 1 m / 1 mm/s handoff gate, and rejection coverage |
| `prototypes/p1a/SoiCrossing/` | Sphere-of-influence entry and exit; the only structurally nonzero transition |
| `prototypes/p1a/WarpEquivalence/` | The accepted warp-equivalence gate, and ADR 0011's no-decay item |

No new fixtures and **no new dependencies**. A3 uses A2's pinned ADR 0008 data unchanged: every
gravitational parameter, sphere radius, and body state below derives from `gm_de440.tpc`,
`pck00011.tpc`, and the four checksummed Horizons tables.

## Threshold results

| Accepted criterion | Measured | Margin | Verdict |
|---|---|---|---|
| Reference orbit: one 200 km period within 100 m of Kepler | 44.6 m at a 64 s step (RK4); 0.5 mm at 4 s | 2.2× at the coarsest passing step | **Met** |
| Local ↔ analytical handoff: ≤ 1 m position | **0.0 m** (state-anchored); 0.88 µm (element-anchored) | exact / 1.1 × 10⁶× | **Met** |
| Local ↔ analytical handoff: ≤ 1 mm/s velocity | **0.0 m/s**; 2.2 × 10⁻¹⁰ m/s | exact / 4.6 × 10⁶× | **Met** |
| SOI crossing within the same tolerances | 4.0 µm (Earth→Sun); 15 nm (Earth→Moon) | 2.5 × 10⁵× | **Met** |
| Warp equivalence within handoff and reference-orbit tolerances | **bit-identical** (anchored); 1.0 mm (stepped) | exact / 1000× | **Met** |
| Determinism: event ordering and bit-identical output on the same build | bit-identical; crossing instants identical to the nanosecond across five warp factors | — | **Met** |
| Invalid transitions rejected explicitly | 7 of 7 rejection reasons constructed and exercised | — | **Met** |
| Peak memory and allocation counts recorded | 4.81–4.94 MB; 22–105 allocations | — | **Met** |

No threshold was relaxed and no criterion was waived. One case is measured but deliberately not
gated, and it is named rather than dropped — see Finding 6.

## The reference orbit, and what the gate cannot see

One 200 km circular period, integrated and compared against the Kepler analytic state at the same
campaign instant. Under ADR 0011 the conic *is* the authoritative model, so every metre here
belongs to the integrator.

Position error after one orbit, in metres:

| Step | RK4 | Velocity Verlet | Yoshida 4 |
|---|---|---|---|
| 64 s | **44.6** | 78 880 | 1 089 |
| 16 s | 0.135 | 4 940 | **4.30** |
| 4 s | 4.88 × 10⁻⁴ | 309 | 1.68 × 10⁻² |
| 1 s | 1.77 × 10⁻⁶ | **19.3** | 6.59 × 10⁻⁵ |
| 0.25 s | 3.42 × 10⁻⁸ | 1.21 | 2.35 × 10⁻⁶ |

Cost of clearing the 100 m gate, in acceleration evaluations per orbit — the honest comparison,
since each candidate needs a different step to clear the same bar:

| Integrator | Largest passing step | Evaluations per orbit | Relative cost |
|---|---|---|---|
| **RK4** | 64 s | **332** | 1.0× |
| Yoshida 4 (symplectic) | 16 s | 996 | 3.0× |
| Velocity Verlet (symplectic) | 1 s | 10 620 | 32× |

The eccentric 200 × 2000 km case gives the same ordering and the same passing steps, so the
result is not an artefact of the circular case being easy.

### Why the symplectic property does not decide this

Over 50 orbits at a 4 s step, the classical reason to prefer a symplectic integrator shows up
exactly as expected:

| Integrator | Worst relative energy error, orbit 1 | Worst over 50 orbits | Secular? | Semi-major axis drift |
|---|---|---|---|---|
| RK4 | 4.14 × 10⁻¹³ | 2.07 × 10⁻¹¹ | **yes** (ratio 50, matching the orbit count exactly) | −136 µm |
| Velocity Verlet | 1.25 × 10⁻¹⁰ | 1.26 × 10⁻¹⁰ | no | +2.1 µm |
| Yoshida 4 | 1.02 × 10⁻¹⁴ | 7.29 × 10⁻¹⁴ | no | +0.3 µm |

RK4's energy error accumulates in proportion to run length, which under ADR 0011 means it
manufactures the orbital decay the ADR forbids.

**It does not decide the integrator, for two independent reasons.**

The first is architectural: under the hybrid contract a craft in a stable orbit is *not
integrated*. It coasts on a closed-form conic, which conserves energy exactly. The local
integrator runs during ascent, thrust, and atmospheric flight — minutes at a time, not years. A
fifty-orbit continuous integration is a scenario this architecture does not produce.

The second is magnitude: RK4's fifty-orbit semi-major axis drift is 136 micrometres. Extrapolated
linearly, a full year of continuous integration would move it under two centimetres.

**Recommendation: RK4 for the local numerical regime**, at three times less cost than the nearest
symplectic candidate that clears the same gate. What would reopen it is any decision that puts a
craft on the numerical integrator for a long continuous span — low-thrust transfers held under
power for days, or an atmosphere limit raised until the reference orbit falls inside it.

## The handoff, and why the gate needed a second option to mean anything

A coast that anchors on the Cartesian state it was handed has a discontinuity of exactly zero,
because nothing is recomputed. Reporting that alone would clear a 1 m gate without examining
anything, so A3 measures the gate against both anchor representations a real design chooses
between.

Worst transition round trip over 64 orbital phases, and drift over 100 000 transition cycles:

| Orbit | Anchor | Worst round trip | 100 000-cycle drift | Cycles to bitwise fixed point |
|---|---|---|---|---|
| 200 km circular | Cartesian state | **exactly 0** | **exactly 0** | 1 |
| 200 km circular | Classical elements | 2.59 × 10⁻⁸ m | 9.3 × 10⁻¹⁰ m | 1 |
| 200 × 2000 km | Cartesian state | **exactly 0** | **exactly 0** | 1 |
| 200 × 2000 km | Classical elements | 1.67 × 10⁻⁷ m | 9.3 × 10⁻¹⁰ m | 1 |
| 200 × 100 000 km | Cartesian state | **exactly 0** | **exactly 0** | 1 |
| 200 × 100 000 km | Classical elements | 8.80 × 10⁻⁷ m | 8.4 × 10⁻⁹ m | 6 |

The element-anchored path degrades with eccentricity, as expected, and still lands six orders of
magnitude inside the tolerance. More usefully, it reaches a **bitwise fixed point** — after at
most six transitions the state stops changing entirely — so repeated handoffs across a campaign
cannot random-walk. This is the same idempotence A2 found in repeated frame conversion, arriving
in a second subsystem for the same underlying reason.

**Consequence:** the anchor representation is not a numerical decision. Both are safe. Choose on
storage size, save-file legibility, and what the orbital map wants to read.

Three quantities are kept separate throughout, because conflating them is the easy mistake:
the **discontinuity** at a transition (what the gate bounds), the **drift** repeated transitions
accumulate, and the **divergence** between the integrator and the conic while both run — the last
of which belongs to the integrator, and is 1.77 µm over one orbit at a 1 s RK4 step.

### Rejection coverage

All seven invalid transition conditions are constructed and exercised, and each returns the
reason that names it: thrust active, inside the atmosphere, degenerate conic, outside the claimed
sphere of influence, below the surface, already coasting, and unknown central body. A rejection
path that is never executed is not a safeguard.

## Sphere-of-influence crossings

Laplace radii from the pinned data: **Earth 9.2918 × 10⁸ m**, **Moon 6.6195 × 10⁷ m**. Both
within a percent of the published values, and derived rather than copied.

This is the only structurally nonzero transition in the contract: the state is re-expressed
against a different origin whose position comes from the ephemeris.

| Case | Crossings | Worst discontinuity | Crossing instant across five warp factors |
|---|---|---|---|
| Earth escape → heliocentric | 1 exit | 3.98 µm | **identical to the nanosecond** |
| Lunar transit (entry and exit) | 1 entry, 1 exit | 1.49 × 10⁻⁸ m | **identical to the nanosecond** |
| Lunar marginal capture | 2–6, varying | 3.41 × 10⁻⁸ m | not reproducible — see Finding 6 |

The identical-to-the-nanosecond result is the load-bearing one and it did not come free: it
required the crossing predicate to be a pure function of the instant, which it was not in A3's
first implementation (Finding 3), and it required the propagator to prove a boundary cannot be
reached within an increment rather than only sampling at increment ends (Finding 4).

**The gravitational hierarchy is not the frame hierarchy.** A2 parents Earth and the Moon to the
Earth-Moon barycentre, which is correct for coordinates and wrong for gravity: a barycentre has
no mass and owns no sphere of influence. Under ADR 0011 the Moon's primary is Earth, Earth's is
the Sun, and the Sun is the root. A3 carries both relations explicitly and `BodySystem` exists to
keep them from being conflated.

## Time warp

Ten orbital periods of campaign time, reached at five warp granularities, in three modes:

| Mode | 1 s | 10 s | 100 s | 1000 s | 10 000 s |
|---|---|---|---|---|---|
| Anchored analytical coast | baseline | **bit-identical** | **bit-identical** | **bit-identical** | **bit-identical** |
| Stepped analytical coast | baseline | 0.89 mm | 1.01 mm | 1.00 mm | 1.00 mm |
| Local numerical, aligned tick | baseline | **bit-identical** | **bit-identical** | **bit-identical** | **bit-identical** |

The anchored coast costs 6 evaluations at the coarsest granularity against 53 097 at the finest,
for an identical answer.

Local-regime alignment, at a 1 s fixed step:

| Tick | Divides the step? | Difference from baseline | Bit-identical |
|---|---|---|---|
| 1 s, 2 s, 10 s | yes | 0 | **yes** |
| 2.5 s | no | 4.76 µm | no |
| 3.7 s | no | 8.43 µm | no |

### ADR 0011's no-decay item

100 days of warped coast at a 10 000 s tick:

| Quantity | Change |
|---|---|
| Semi-major axis | −5.77 µm |
| Periapsis radius | −13.2 µm |
| Apoapsis radius | +1.68 µm |
| Final eccentricity | 1.13 × 10⁻¹² |
| Sphere-of-influence transitions | 0 |

Micrometres over a hundred days is the arithmetic, not the physics. The orbit does not decay,
which is what ADR 0011 says it must not do.

## Findings

### Finding 1 — the atmosphere limit cannot be derived from the handoff tolerance, and trying reveals a category error

ADR 0011 leaves the atmosphere limit to A3. The obvious derivation is to place it where neglected
drag costs less than the 1 m handoff tolerance over one orbit. That gives roughly **450 km** —
which would make the P1a reference orbit, and the first playable's own contract orbit,
permanently ineligible for analytical coast and therefore un-warpable.

A threshold that excludes the mission the game is built around is a sign the derivation is
answering the wrong question, and it is. ADR 0011 does not *approximate* a drag-affected
trajectory; it **defines** orbits as drag-free and non-decaying. There is no true trajectory for
the coast to diverge from, so there is no accuracy budget to spend. The 1 m tolerance governs the
discontinuity introduced *at a transition*, which is a self-consistency property of the handoff
and is unaffected by where the boundary sits.

What the boundary actually selects is which regime a craft is simulated in — a gameplay and
physics-model decision, not a numerical one. **A3 records 140 km**, above the altitude where drag
meaningfully shapes a powered ascent and below any orbit the player is expected to hold. Final
tuning belongs to P2/M5 and is listed in the open questions.

### Finding 2 — a change of primary can invalidate a coast, and the first implementation did not re-check

A state that describes a perfectly good conic about Earth can be **radial** about the Moon: zero
angular momentum, no conic at all. `rebaseTo` originally changed the central body without
re-checking eligibility, so a lunar approach on a head-on course was handed to the Moon and
coasted on a degenerate conic — producing positions that were smooth, plausible, and meaningless,
and a spurious sphere-of-influence exit 1 679 s after an entry that physically required 130 000 s.

This is precisely the failure mode A3's rejection rules exist to prevent, occurring on the one
path that did not consult them. The propagator now re-validates after every rebase and drops to
the numerical integrator when the new conic is degenerate — which is the right response rather
than refusing the crossing, since the integrator represents radial motion without difficulty and
only the closed-form conic cannot.

It was found because the scenario's own numbers were physically impossible on inspection, not by
a test. That is the same detection route as three of A2's four defects, and it remains the
weakest part of the suite.

### Finding 3 — the crossing instant was not a pure function of the instant, and warp equivalence failed by 23 minutes

The bisection that places a crossing propagated from wherever the current increment happened to
start, while the end-of-increment test propagated from the coast anchor. The crossing predicate
was therefore not a function of the instant alone — it depended on which increment was asking —
so runs at different warp factors bracketed the boundary differently and disagreed about when it
was crossed by up to **1 387 s over a three-day escape**.

Anchoring the probe makes the predicate pure, and any interval bracketing the crossing then
converges to the same nanosecond. This is what turns tick-invariance from a hopeful property into
a structural one, and it is the single change that most improved A3's results.

A craft in the local numerical regime has no anchor, so its probe still runs from the current
state. Crossings detected there are only as reproducible as the integrator's path to them. That
is accepted rather than overlooked: a boundary is hundreds of thousands of kilometres from any
atmosphere, so a craft reaching one is coasting.

### Finding 4 — a warp tick can step over a boundary excursion entirely

Sampling the boundary test only at increment ends means an excursion that begins and ends inside
one increment is invisible. A3 measured this directly: a craft that left the Moon's sphere and
returned within one 1 000 s tick was never seen to leave, and diverged from the same trajectory
at finer ticks from that point on.

The propagator now shrinks an increment until the craft **provably cannot reach a boundary within
it** — clearance divided by an upper bound on closing speed is a duration during which no
crossing is possible, whatever the trajectory does. The cost is measured: 53 to 1 664
subdivisions across a ten-day escape, which is negligible against 90 to 864 003 coast
evaluations.

A separate hysteresis band of 10⁻⁴ of each sphere radius — 92 km at Earth, 6.6 km at the Moon —
stops ownership changing hands on rounding. Without it a craft grazing a boundary chattered:
A3 measured an exit followed by a re-entry **fifteen milliseconds later**, repeating. A dead band
is defensible here in a way it would not be in an exact model, because the Laplace radius is a
convenient place to switch primaries rather than a physical surface.

### Finding 5 — A3 asserted the local regime could not be warp-invariant, and its own measurement said otherwise

The first version of `WarpEquivalence` claimed the local numerical regime "is not warp-invariant
either, and cannot be made so". The measurement returned bit-identical results at every warp
factor tested.

The claim was wrong for a specific reason: every tick in the set — 1, 10, 100, 1 000, 10 000 s —
is an exact multiple of the 1 s local step, so no increment ever needed a truncated remainder and
the sequence of full steps was identical. The corrected statement is sharper and actually usable:
**the local regime is warp-invariant exactly when the warp tick is an integer multiple of the
fixed local step.** Ticks of 2.5 s and 3.7 s against a 1 s step differ by 4.76 µm and 8.43 µm and
are not bit-identical; 1 s, 2 s, and 10 s are.

The consequence is a design constraint rather than a prohibition. Warp does not have to be
forbidden under thrust on determinism grounds — it has to be **quantised**, so that the set of
offered warp factors divides evenly into the local step.

This is recorded prominently because the failure mode is the dangerous one: a plausible assertion
that the evidence contradicts, in a document whose whole purpose is to be trusted later.

### Finding 6 — a marginal capture has no well-conditioned crossing time, and this is not fixable

An approach slower than lunar escape speed at the boundary is captured into a bound orbit that
repeatedly grazes the sphere from inside. Its crossings are ill-conditioned by construction: the
radius is tangent to the boundary, so an arbitrarily small change in state moves a crossing
arbitrarily far in time, or removes it.

A3 measures it and reports **6, 6, 4, 2, and 2 crossings** at the five warp factors, with the
first entry reproducible to 65 ns and later crossings not reproducible at all. Discontinuities
stay inside tolerance throughout — a crossing whose *instant* is not reproducible still hands the
state over cleanly.

Tick-invariance is therefore **measured but not gated** for this case. Gating it would force
either a relaxed threshold, which the P1a rules forbid, or the misrepresentation of a property of
the trajectory as a defect in the propagator. It is named in the open questions instead, because
a game that offers lunar capture will produce these trajectories and will need a rule — most
likely a deliberate one about when the simulation commits to a capture, rather than a numerical
one.

## Validation performed

`OrbitSelfCheck` runs **118 checks** in both configurations before any measurement is trusted,
following the A1 and A2 pattern. It covers:

- Stumpff functions against closed-form values at z = π² and z = −4, and the series and closed
  forms against each other at the switchover — the check that would catch a threshold moved to
  where neither form is well conditioned;
- Kepler propagation: exact identity at zero duration, one-period return within 1 µm, antipodal
  half-period, forward/backward round trip, and conservation of energy and angular momentum to
  10⁻¹⁴ relative, across circular, eccentric, and hyperbolic arcs;
- element conversion round trips including **both degenerate corners** — the circular equatorial
  case is the A3 reference orbit, so the singular corner of the classical set is the main path
  rather than an edge case, and undefined angles are reported as undefined rather than filled
  with a fabricated zero;
- integrator convergence order, measured at step sizes chosen so truncation error dominates the
  Kepler reference's own rounding floor — the first attempt measured two noise samples and
  reported their ratio;
- sphere-of-influence radii against published values, and the hierarchy's own coherence;
- the conic ephemeris reproducing its fixtures at the anchor epoch within 1 m, diverging from
  linear extrapolation by more than 1 000 km over one day, and holding the Moon's geocentric
  distance inside its real perigee/apogee range over 30 days;
- **the integer campaign clock, with a negative control**: 3 600 one-second increments reach
  exactly the same instant as one hour, and 3 600 accumulations of 0.1 s in a double do not reach
  360 s. The warp comparison is meaningless without the first, and the second demonstrates why
  ADR 0010 requires it rather than asserting it;
- every eligibility rejection, every handoff invariant, and 1 000 coast begin/end cycles leaving
  the state bit-identical.

`HybridHandoffAcrossRuns`, `SoiCrossingAcrossRuns`, and `WarpEquivalenceAcrossRuns` rerun each
scenario in a separate process and compare the whole `results` section byte for byte, extending
the golden-pinning A2 introduced. `ReferenceOrbit` is not pinned this way because its results
carry timing.

Full suite: **20 tests, 100% passing in both Debug and Release.**

## Decision recorded

**The transition contract is selected**, in four parts:

1. **Exactly one regime owns a craft's state at any instant.** There is no interval during which
   both run and are reconciled, because reconciling two authoritative states is how
   discontinuities get introduced.
2. **The analytical coast anchors on the state it is handed, and evaluates from that anchor** by
   total elapsed time rather than by composing increments. This makes the regime-change
   discontinuity exactly zero and makes warp invariance exact.
3. **Sphere-of-influence crossings are discrete scheduled events**, refined to the nanosecond by
   bisection on the conic, with ownership passing at a single instant and the coast re-anchored
   against the new primary. Increments are subdivided so a boundary cannot be stepped over, and a
   hysteresis band prevents chattering.
4. **Eligibility is checked explicitly and rejected by named reason** — before a coast begins and
   again after every change of primary.

Supporting selections: **RK4** for the local numerical regime; **140 km** for Earth's atmosphere
limit; **warp ticks constrained to integer multiples of the local step**. The anchor
representation is left open because both measured options are safe.

**What would reopen this.** A decision that puts a craft on the numerical integrator for a long
continuous span reopens the integrator choice. Adding perturbations to the propagation — which
ADR 0011 names as its most likely future upgrade — reopens all four parts, because the analytical
coast would stop being exact and every tolerance here is built on its being exact.
