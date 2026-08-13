# P1a increment A2 — evidence index

**Increment:** A2, reference frames and numerical precision
**Owner:** Claude (single writer)
**Branch / base:** `dev` working tree, uncommitted — see [Handoff.md](Handoff.md)
**Date:** 2026-08-12
**Result:** All A2 done criteria met. One frame model selected. Four findings recorded; the two
that needed a user decision were resolved on 2026-08-12 and are marked below.

> **Addendum, 2026-08-12 — P1a milestone review.** The review re-ran A2's scenarios against this
> increment's recorded output; every measurement reproduced. It then made one additive change to
> the frame library: `ReferenceData` also parses `BODY10_RADII` and `BODY301_RADII` from the
> already-pinned planetary-constants kernel, so that increment A3's `BodySystem` stops leaving the
> Sun's and the Moon's surface radii at zero. No conversion, transform, or frame boundary was
> altered, and **no A2 physical number changed**: `FrameRoundTrip`, `AscentSampling`, and
> `PrecisionBudget` are byte-identical, and `ReferenceFixtures` differs only in its scenario
> allocation counters — **4 418 → 4 424 allocations, 1 747 079 → 1 747 247 bytes** — from the three
> extra kernel lookups at load. `raw/` has been regenerated. The milestone-level record is
> [`evidence/p1a/Index.md`](../Index.md).

Raw measurement output lives in `raw/`, which `.gitignore` excludes. It is reproducible from the
commands in [Handoff.md](Handoff.md); this index and the handoff are the durable record.

## Headline

**The hierarchical parent-relative frame model is selected.** Both candidates meet every
accepted A2 threshold, so the decision was not made on compliance. It was made on the measured
gap in the case that dominates real use: for a conversion that does not need barycentric
coordinates, the hierarchical model's error is **sub-nanometre** and the flat model's is
**4.5 µm** — a factor of about 4,500 — and the hierarchical model is also **1.7× faster** for
that case (15.8 ns against 26.4 ns).

The flat model is faster the moment a conversion does reach the root (7.9× at full chain depth).
That trade is recorded, and it is why the selection is a decision rather than a discovery.

## Toolchain and host

Unchanged from A1 and recorded in every report's `environment` section.

| Item | Recorded value |
|---|---|
| Compiler | MSVC 19.51.36252.0, `-std:c++latest`, `/fp:precise /arch:AVX2` |
| Contraction | `implicit-off-under-fp-precise` (A1 Finding 2) |
| OS | Windows 10.0.26200 |
| CPU | 12th Gen Intel Core i7-12650H, 16 logical processors, AVX2 + FMA3 |
| Performance counter | 10 MHz, so 100 ns resolution |
| Peak working set | 4.83–5.69 MB across the five scenarios |

## What was built

| Path | Role |
|---|---|
| `fixtures/p1a/` | **Durable.** Pinned NAIF kernels and Horizons state vectors with [provenance](../../../fixtures/p1a/Provenance.md) and SHA-256 |
| `prototypes/p1a/Frames/` | Units, frames, time scales, ellipsoid, both candidate models |
| `prototypes/p1a/FramesSelfCheck/` | Guards the frame library. 95 checks |
| `prototypes/p1a/ReferenceFixtures/` | The ADR 0008 reference-data generation record |
| `prototypes/p1a/FrameRoundTrip/` | The round-trip and drift thresholds |
| `prototypes/p1a/AscentSampling/` | Per-boundary error along a surface-to-200-km trajectory |
| `prototypes/p1a/PrecisionBudget/` | The measured precision budget |
| `prototypes/p1a/FrameModelCost/` | Conversion cost, for P1b to budget against |

The frame chain under test, from the pad outward:

```
VehicleLocal -> LaunchSiteEnu -> EarthBodyFixed -> EarthIcrf -> EarthMoonBarycentreIcrf -> SsbIcrf
```

Six frames, five boundaries, crossing a floating local origin, a topocentric horizon frame, a
rotating body-fixed frame, and two nested inertial translations. Sun and Moon frames hang off the
same graph so the hierarchical model's common-ancestor walk is a real operation.

## Threshold results

| Accepted criterion | Flat (global root) | Hierarchical (parent-relative) | Verdict |
|---|---|---|---|
| Round trip at surface anchor, position ≤ 1 mm | 4.910 µm | 4.796 µm | **Both pass** |
| Round trip at surface anchor, velocity ≤ 1 µm/s | 0.357 nm/s | 0.240 nm/s | **Both pass** |
| Round trip at 200 km, position ≤ 1 mm | 4.910 µm | 5.959 µm | **Both pass** |
| 10⁶ conversions, bounded and non-accumulating | bounded | bounded | **Both pass** |
| Every failure names its boundary | n/a — nothing failed | n/a | Attribution computed anyway |
| Precision budget stated as measured numbers | see below | see below | **Met** |
| One model selected, or alternatives kept open with deciding evidence | — | **selected** | **Met** |
| Selected model's conversion cost recorded | — | 15.8–82.4 ns by case | **Met** |

Threshold application: A2 exists to select or reject candidates, so a threshold is asserted
against *the increment* — the scenario fails only if **no** candidate meets a criterion. Each
model's number is recorded either way. Nothing was relabelled and no tolerance was relaxed.

## The decisive measurement: per-boundary attribution

From `AscentSampling`, median position round-trip error at each boundary across 2001 samples of
a 500 s ascent (metres):

| Boundary | Flat | Hierarchical |
|---|---|---|
| `VehicleLocal -> LaunchSiteEnu` | **4.50 × 10⁻⁶** | 0.0 |
| `LaunchSiteEnu -> EarthBodyFixed` | 0.0 | 2.84 × 10⁻¹⁰ |
| `EarthBodyFixed -> EarthIcrf` | 0.0 | 9.60 × 10⁻¹⁰ |
| `EarthIcrf -> EarthMoonBarycentreIcrf` | 0.0 | 0.0 |
| `EarthMoonBarycentreIcrf -> SsbIcrf` | 0.0 | **4.57 × 10⁻⁶** |
| Full chain | 4.50 × 10⁻⁶ | 4.57 × 10⁻⁶ |

Read the zeros carefully — they are not the same statement in the two columns.

**The flat model spends its entire error on the first conversion out of the source frame**,
because every conversion routes through the barycentric root regardless of destination. Once a
state is at root magnitude, further hops between frames reproduce the same rounded root
coordinate and cost nothing more. Converting a vehicle's state into the launch-site frame 100 m
away therefore costs the same 4.5 µm as converting it to the Solar System barycentre.

**The hierarchical model spends its error only where the magnitude is actually large.** Its four
sub-barycentric boundaries total under 1.3 nanometres. It reaches parity with the flat model only
on the last boundary, the one that genuinely forms 1.5 × 10¹¹ m coordinates — and a conversion
that does not ask for the root never crosses it.

This is the whole decision, stated as measured numbers: both models cost the same when you need
barycentric coordinates, and the hierarchical model is ~4,500× more precise when you do not.

## Conversion drift

10⁶ repeated conversions, in two modes, because "repeated conversion" describes two different
programs:

| Mode | What it models | Flat | Hierarchical |
|---|---|---|---|
| Stateless — every conversion re-derives from one stored original | one authoritative state, many derived views | constant at 4.910 µm through 10³, 10⁴, 10⁵, 10⁶ | constant at 4.796 µm |
| Iterated — every conversion consumes the last one's output | converted states are stored and reconverted | constant at 4.910 µm | constant at 4.796 µm |

**The iterated mode reaches a bitwise fixed point after 1 conversion, in both models.** This was
measured, not assumed: the scenario compares consecutive states bit for bit and reports the
iteration at which they stop changing.

The mechanism matters more than the bound. At barycentric magnitude one ULP is 30.5 µm, so the
round trip quantises the state onto a grid far coarser than the perturbation it introduces. Any
input within half an ULP maps to the same intermediate coordinate and therefore to the same
output — the round trip is idempotent, and a random walk is impossible rather than merely
unobserved.

**Consequence for the architecture:** storing converted states and reconverting them is safe
here. That is a stronger result than "bounded", and it was not obvious in advance; the plausible
prediction was √N growth.

## Precision budget

### Representable resolution — the floor no algorithm can beat

One ULP of a `double`, in metres, at each distance a candidate root would force a coordinate to:

| Distance | Metres per ULP | ULPs per mm |
|---|---|---|
| 100 m — a craft's own extent | 1.42 × 10⁻¹⁴ | 7.0 × 10¹⁰ |
| 2 × 10⁵ m — the 200 km reference altitude | 2.91 × 10⁻¹¹ | 3.4 × 10⁷ |
| 6.378 × 10⁶ m — Earth's surface from its centre | 9.31 × 10⁻¹⁰ | 1.07 × 10⁶ |
| 3.844 × 10⁸ m — Earth to Moon | 5.96 × 10⁻⁸ | 1.7 × 10⁴ |
| 1.47 × 10¹¹ m — Earth to the barycentre | 3.05 × 10⁻⁵ | 32.8 |
| 7.785 × 10¹¹ m — Sun to Jupiter | 1.22 × 10⁻⁴ | 8.2 |
| 4.495 × 10¹² m — Sun to Neptune | 9.77 × 10⁻⁴ | **1.02** |
| 3 × 10¹⁴ m — inner Oort cloud | 6.25 × 10⁻² | 0.016 |

**At Neptune's distance the entire millimetre budget is one representable step.** A global
double root is nominally millimetre-capable there and has no headroom whatever — any arithmetic
at all exceeds the budget. The practical limit for a single-rooted double at this tolerance is
Jupiter. The roadmap's P5–P7 outer-system stages are where a global root stops working, not
where it becomes merely tight.

The hierarchical model does not have this problem at all: its stored magnitudes are the
relationships they describe, never the distance to a global origin.

### The summed per-conversion budget, selected model, surface anchor

| Term | Metres |
|---|---|
| Round-trip arithmetic through the full chain | 4.796 × 10⁻⁶ |
| Deepest non-root magnitude, one ULP | 9.31 × 10⁻¹⁰ |
| **Implemented total** | **4.797 × 10⁻⁶** |
| Threshold | 1.0 × 10⁻³ |
| **Headroom** | **208×** |

Two terms are deliberately excluded and recorded separately, because folding them in would
either double-count or overstate:

- **Datum choice, 0.403 m.** IAU `pck00011` versus WGS84 for the same geodetic anchor. This is a
  definitional offset of the whole site, not an error between two conversions of one state — but
  it is 400× the entire position budget, and quoting an anchor position without its datum is
  meaningless at this tolerance.
- **Reference-data resolution, 0.146 mm.** Horizons prints 16 significant digits, so no
  computation can be more certain about an absolute barycentric position than this. It bounds
  absolute claims, not relative ones.

A third term is excluded because the implementation avoids it — see Finding 1.

## Conversion cost

Release build, 64 samples of 4096 conversions each. Nanoseconds per conversion, as
median (p95):

| Case | Flat | Hierarchical | Applications (flat / hier) | Ratio of medians |
|---|---|---|---|---|
| `VehicleLocal -> LaunchSiteEnu` (adjacent) | 26.4 (30.2) | **15.8 (16.9)** | 2 / 1 | 0.60× |
| `VehicleLocal -> EarthIcrf` | 23.0 (26.4) | 48.5 (68.6) | 2 / 3 | 2.11× |
| `VehicleLocal -> SsbIcrf` (full chain) | **10.5 (12.1)** | 82.4 (92.0) | 1 / 5 | 7.85× |
| `EarthIcrf -> MoonIcrf` (across branches) | 22.9 (24.9) | 28.6 (31.7) | 2 / 2 | 1.25× |

**These are a distribution and will not reproduce exactly.** An earlier capture of the same
Release binary gave adjacent-case medians of 26.2 ns and 15.5 ns and a full-chain hierarchical
median of 85.6 ns — a few percent either way, and the ratios stable to two significant figures.
A1's evidence established this distinction and it applies here unchanged: the numerical results
above are bit-exact and gated as such, these are not, and quoting one of them as a constant would
be misreporting.

Per transform application both models cost 10.5–16.5 ns, so the differences are almost entirely
path length rather than per-hop efficiency. Rebuild cost, paid once per timestep rather than per
object: flat **316 ns**, hierarchical **10.7 ns** — the flat model's conversion-time advantage is
its build-time composition, moved.

Rebuilds and conversions are both batched before timing. The performance counter's resolution is
**100 ns** and a single hierarchical rebuild is 10.7 ns, so an unbatched measurement would have
reported `0.0` — not a fast result but an absent one. It did, in the first capture, and that is
recorded as a fixed defect in [Handoff.md](Handoff.md).

**For P1b:** the adjacent case is the one a renderer pays per visible object. At ~16 ns, ten
thousand objects cost about 0.16 ms per frame against a 16.67 ms budget. Use the per-application
figure to re-derive a cost if P1b's chain is a different depth, and re-measure on the reference
hardware rather than carrying these numbers across.

## Findings

### Finding 1 — an unreduced Earth rotation angle costs 27 µm, and the fix is free

Earth's IAU prime-meridian rate is 360.9856235 °/day. Twenty-six years past J2000 the accumulated
angle is about 3.4 × 10⁶ degrees, where one ULP is 7.6 × 10⁻¹⁰ degrees — **27.2 µm of arc at the
launch site, 2.7% of the entire position budget**, spent on nothing but the representation of an
angle that is about to be reduced modulo 360 anyway.

Splitting the rate into 360 °/day plus a 0.9856235 remainder discards the whole turns before they
are ever formed. The subtraction `360.9856235 - 360` is exact in binary and integer turns are
exactly invisible to sine and cosine, so the reduction is lossless as well as cheaper.

A2 implements the reduced form. The unreduced cost is measured and reported so the reason is on
record — this is the kind of detail that gets "simplified" out during a later refactor by someone
who reads it as an optimisation.

The summed budget above excludes this term because the implementation does not pay it. With it,
the total would be 32.0 µm and headroom 31× instead of 208×.

### Finding 2 — Horizons serves DE441, ADR 0008 names DE440

Every fetched state vector reports `{source: DE441}`. DE441 is the long-span integration of the
same JPL solution; over the interval containing 2026 the two are the same dynamical fit. The
gravitational parameters *do* come from `gm_de440.tpc`, so the constants and the states are from
the same family but not identically named products.

Nothing A2 concludes depends on this, because every number it reports is a frame conversion whose
correctness does not depend on which product supplied the origin.

**Resolved 2026-08-12.** ADR 0008 was amended to name the **DE440/DE441 family** and to require
every fixture to record which product actually supplied it. Downloading `de440.bsp` was rejected:
a 114 MB binary under Git LFS plus a reopened CSPICE dependency review, to obtain numbers that
agree with what Horizons already serves. If a later milestone needs a queryable ephemeris — A3's
SOI transitions are the first plausible candidate — that is a dependency decision on its own
merits rather than a provenance patch.

### Finding 3 — the launch anchor moves 0.403 m between defensible datums

ADR 0008 specifies 28.0° N, 80.5° W, 5 m above mean sea level. Converting that to a body-fixed
vector requires an ellipsoid, and the two obvious choices differ:

| Datum | Equatorial radius | Anchor displacement |
|---|---|---|
| IAU, `pck00011` `BODY399_RADII` | 6 378 136.6 m | — |
| WGS84 | 6 378 137.0 m | 0.403 m |

A2 uses the IAU value because it arrives with a checksum from the source ADR 0008 names. The
displacement is 400× the position budget, which makes "record the datum with the coordinate" a
correctness requirement rather than good practice.

Separately, "mean sea level" is a geoid statement, and the geoid is roughly −30 m from the
ellipsoid in this region — four orders of magnitude past the budget.

**Resolved 2026-08-12.** ADR 0008 now defines the anchor as **5 m above the reference
ellipsoid**, specifically the one from `pck00011.tpc`'s `BODY399_RADII`, and requires a datum to
travel with every geodetic coordinate. Adopting a geoid model was rejected: it would import
EGM96 or EGM2008 as a new data dependency to resolve a 30 m offset for a facility that is
fictional and therefore has no real elevation to be faithful to. Adopting WGS84 was rejected
because no pinned NAIF kernel supplies its constants, which would cost the checksummed-source
property the rest of the fixture set has.

### Finding 4 — `pck00011.tpc` assigns `BODY399_RADII` four times, and three are documentation

Only the assignment inside a `\begindata` block is the adopted constant; the other three are
worked examples in the file's preamble. A parser that greps for the keyword reads an example.

The kernel parser honours `\begindata`/`\begintext` for exactly this reason, and
`FramesSelfCheck` asserts the parsed radii against the adopted values so the property cannot
regress silently. This is the same class of defect as A1's JSON writer emitting every string as
`true`: it would have produced numbers that were confidently, plausibly wrong.

## Validation performed

`FramesSelfCheck` runs 95 checks in both configurations before any measurement is trusted,
following A1's lesson that an increment's own instruments need testing first. It covers:

- SHA-256 against four published FIPS-180-4 vectors — a broken hash still matches itself;
- kernel parsing against the adopted constants, per Finding 4;
- **the time boundary against Horizons**: UTC 2026-01-01 00:00:00 converted through the pinned
  leap-second kernel lands on the fixture epoch within 0.1 ms. This validates the project's
  UTC/TAI/TT/TDB chain against an independent implementation using data neither side derived
  from the other, and is the single most load-bearing check in the increment;
- the ADR 0008 leap-second-adjacent case: 2016-12-31 23:59:60 keeps TAI−UTC = 36 s, the instant
  after it takes 37 s, and the two are exactly 1 s apart on the uniform scale;
- geodetic round trips at five points including both near-polar height branches;
- topocentric basis orthonormality and handedness;
- transform inverse identity at every boundary;
- `compose()` against sequential application — the flat model is built entirely from `compose()`,
  so an error there would have looked like a model difference;
- **the Earth rotation-rate matrix against a central difference of the rotation.** A round trip
  through a wrong-but-consistent transform still returns to its input, so no round trip would
  have caught a plausible-but-wrong ω term. This check would.

`FrameRoundTripAcrossRuns` reruns the threshold scenario in a separate process and compares the
whole `results` section byte for byte, closing A1's open item that no golden values were pinned.
A2's numbers are physical, so pinning them is now worth doing.

## Decision recorded

**Selected: hierarchical parent-relative frame graph, with conversion by walking to the lowest
common ancestor.**

Rationale, in the order the evidence supports it:

1. Sub-nanometre error on every boundary that does not form barycentric magnitude, against
   4.5 µm for the flat model on all of them.
2. Cheaper for the conversion a renderer actually performs per object (15.8 ns vs 26.4 ns).
3. Its stored magnitudes are the relationships they describe, so it does not degrade with
   distance from a global origin — which is what keeps the outer-system roadmap open.
4. Rebuild is 29× cheaper, which matters because the graph is rebuilt every timestep.

Accepted costs:

- 7.9× slower for a full-chain conversion to the root. Acceptable because those are rare in the
  inner loop and the absolute figure is 82 ns.
- More complex than a flat array lookup: an ancestor walk with a descent path.

**What would reopen this.** P1b increment B1 evaluates the screen-space jitter gate against the
frame model A2 selects. Per the P1a plan, a B1 failure reopens this decision rather than being a
P1a failure. The measurement most likely to reopen it is a P1b scene whose per-frame conversion
count makes the full-chain case dominant.
