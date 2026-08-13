# Architecture

**Status:** Mostly proposed pre-production architecture. The [Implemented build foundation](#implemented-build-foundation), [Selected reference-frame model](#selected-reference-frame-model), and [Selected hybrid propagation and transition contract](#selected-hybrid-propagation-and-transition-contract) sections below are implemented technical truth as of P1a increments A1, A2, and A3, and the [Implemented renderer foundation](#implemented-renderer-foundation) is implemented technical truth as far as P1b increment B1 has progressed. Everything else remains proposed and is not implemented. Accepted decisions are recorded in `docs/decisions/`.

## Implemented build foundation

Delivered by P1a increment A1 and verified against the evidence in
[`evidence/p1a/A1/Index.md`](../evidence/p1a/A1/Index.md).

### Toolchain

MSVC 19.51.36252.0 (toolset 14.51.36231) under Visual Studio 18 Community, CMake 4.4.2, and
Ninja 1.12.1 resolved from `PATH`. `_MSC_VER` 1951 is the recorded minimum toolset; raising it
is a deliberate act, not a side effect of an upgrade.

### Presets

Single-configuration Ninja trees per ADR 0001, with binary directories at `build/<preset>/`.
Configure, build, and test presets share each name.

| Purpose | Preset |
|---|---|
| Debug | `windows-msvc-debug` |
| Release | `windows-msvc-release` |
| ADR 0010 negative control | `windows-msvc-release-negcontrol-contract` |

Only Release output is eligible as performance evidence; scenario reports carry an
`evidenceEligibility` field so a Debug number cannot be quoted by mistake. The negative-control
preset deliberately violates ADR 0010 and fails `ToolchainReport` by design.

### Applied compiler flags

`cmake/SolProjectOptions.cmake` is the single place compiler policy is applied to
project-owned targets:

```
-std:c++latest /fp:precise /arch:AVX2 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W4 /WX
```

`/std:c++23` is rejected by this toolset; `/fp:fast` is rejected at configure time. No flag
spelling disables FMA contraction on MSVC 19.51 — `/fp:precise` disables it by default, and
that default is verified by measurement rather than assumed. See ADR 0010's recorded
implementation values.

### Prototype tree and namespace

P1a code lives under `prototypes/p1a/` in the `sol::proto` namespace, deliberately outside the
`engine/`, `game/`, `editor/`, and `tests/` routing in `AGENTS.md`. The P1a plan makes these
executables disposable, and a separate tree keeps disposable work from acquiring the standing
of production code by proximity.

`prototypes/p1a/Harness/` is the exception: it is durable, holds the measurement, provenance,
and reporting types every later increment reports through, and is kept free of simulation and
domain concepts so promoting it later remains a real option rather than a sunk cost. Promotion
still requires explicit review.

### Measurement report format

Scenarios emit UTF-8 JSON with `\n` line endings and two top-level sections:

- `environment` — provenance that legitimately varies between runs: timestamp, output path,
  git commit and dirty flag, preset, compiler and flags, host CPU and OS, peak process memory,
  allocation counts.
- `results` — everything the scenario computed, byte-identical across runs of the same build.

Determinism comparison covers `results` only. Doubles are written with `std::to_chars`
shortest round-trip, and any value whose reproducibility matters is additionally emitted as its
raw IEEE-754 bit pattern. Peak process memory and allocation counts are mandatory in every
report.

P1a's own scenarios use no third-party dependency and are plain executables registered with
CTest. That remains true of everything under `prototypes/p1a/`. It is **no longer true of the
repository**: P1b increment B1 triggered ADR 0007's dependency workflow, and `vcpkg.json` now
exists. See [Dependencies](dependencies.md) and the [renderer foundation](#implemented-renderer-foundation)
below.

## Selected reference-frame model

Delivered by P1a increment A2 and verified against the evidence in
[`evidence/p1a/A2/Index.md`](../evidence/p1a/A2/Index.md). This section records a measured
selection, not a proposal. It does not yet describe production code: A2's implementation lives in
`prototypes/p1a/Frames/` and promoting it requires an explicit review.

### The selection

**Frames are stored relative to their immediate parent, and conversions walk to the lowest common
ancestor.** A single global root holding every frame's transform against the Solar System
barycentre was implemented as the competing candidate and measured against it.

Both models meet every accepted P1a threshold, so compliance did not decide it. The measured gap
in the case that dominates real use did:

| Property | Global root | Parent-relative |
|---|---|---|
| Round-trip error, boundaries below barycentric magnitude | 4.5 µm | under 1.3 nm |
| Round-trip error, full chain to the barycentre | 4.50 µm | 4.57 µm |
| Cost, adjacent-frame conversion | 26.4 ns | 15.8 ns |
| Cost, full chain to the root | 10.5 ns | 82.4 ns |
| Rebuild, per timestep | 316 ns | 10.7 ns |

Timings are medians on the development machine and are a distribution, not a constant; the
evidence index records their spread and the run-to-run variation. The precision figures are
bit-exact and gated as such.

A global root routes every conversion through barycentric magnitude regardless of destination, so
converting a vehicle's state into a launch-site frame 100 m away costs the same precision as
converting it to the Solar System barycentre. Parent-relative storage pays that cost only when a
conversion actually asks to cross that boundary, and most do not.

The accepted trade is that a full-chain conversion to the root is 7.9× slower, at an absolute
cost of 85.6 ns.

### Frame chain

```
VehicleLocal -> LaunchSiteEnu -> EarthBodyFixed -> EarthIcrf -> EarthMoonBarycentreIcrf -> SsbIcrf
```

`VehicleLocal` is a floating local origin whose axes stay parallel to the launch site's, so a
craft translates within the graph without rotating under it. `SunIcrf` and `MoonIcrf` branch off
the same graph.

A state carries its frame **and its epoch**. Two frames in this chain rotate, so a state
converted against the wrong instant is wrong by hundreds of metres and looks entirely reasonable;
conversions reject an epoch mismatch rather than proceeding.

### Precision budget

Per conversion at the surface anchor, for the selected model: **4.797 µm against a 1 mm
threshold, 208× headroom.** The dominant term is the arithmetic of forming barycentric
coordinates; every boundary below that contributes under a nanometre.

Repeated conversion does not accumulate. A round trip reaches a **bitwise fixed point after one
conversion**, because the intermediate quantisation at barycentric magnitude is far coarser than
the perturbation the round trip introduces. Storing converted states and reconverting them is
therefore safe, which is a stronger guarantee than the threshold asked for.

The limit that constrains the roadmap: one ULP of a `double` is 0.98 mm at Neptune's distance,
so a global-root double has no millimetre headroom at all beyond roughly Jupiter. Parent-relative
storage does not have this problem, because its magnitudes are the relationships they describe
rather than distances to a global origin.

### Units and time

SI throughout, with conversions confined to named boundary functions. Positions are metres,
velocities metres per second, and the km-valued NAIF and Horizons data is converted once at parse.

Campaign time accumulates as exact integer nanoseconds and converts to the ephemeris scale once,
explicitly, per ADR 0010. The tick rate itself remains open.

UTC, TAI, TT, and TDB are separate scales driven entirely by the pinned leap-second kernel: no
leap-second count and no TAI−TT offset is written into the source. UTC is represented as a
calendar, not a second count, so the 23:59:60 leap-second instant is representable and keeps its
pre-step offset. The campaign epoch converted through this boundary agrees with JPL Horizons to
the fixtures' printed 0.1 ms resolution.

### What this section does not decide

- **Earth orientation.** A2 uses the IAU_EARTH definition from a pinned kernel, which omits
  nutation, polar motion, and UT1−UTC and can differ from ITRF by tens of metres at the surface.
  Adequate for measuring a rotating boundary's numerics; not a navigation model. The production
  Earth orientation model remains open.
- **Origin motion.** A2 extrapolates celestial origins linearly from one fixture epoch. That is
  self-consistent frame kinematics, not an ephemeris. **Superseded by A3**, which propagates
  celestial origins as ADR 0011 conics; see [Celestial origin motion](#celestial-origin-motion)
  below. A2's own code is deliberately unchanged, so its committed evidence stays reproducible.
- **Which ellipsoid, beyond P1.** ADR 0008 was amended after A2 to define the anchor 5 m above
  the reference ellipsoid, and A2 adopts the IAU `pck00011` value. WGS84 places the same anchor
  0.403 m away and may be preferable later for interoperability with real geospatial data;
  adopting it would require new content coordinates and fixtures rather than a reinterpretation
  of the P1 reference case.
- **Promotion.** Nothing in `prototypes/p1a/Frames/` is production code. Unlike the measurement
  harness it carries domain concepts, so promoting it is a larger decision, not a smaller one.

### What would reopen it

P1b increment B1 evaluates the screen-space jitter gate against this model. The P1a plan makes a
B1 failure a reason to revisit this decision rather than a P1a failure.

## Selected hybrid propagation and transition contract

Delivered by P1a increment A3 and verified against the evidence in
[`evidence/p1a/A3/Index.md`](../evidence/p1a/A3/Index.md). This section records a measured
selection, not a proposal. It does not yet describe production code: A3's implementation lives in
`prototypes/p1a/Orbit/` and is disposable under the P1a plan.

The gravity baseline itself is [ADR 0011](decisions/0011-gravity-and-orbit-baseline.md) — patched
conics with spheres of influence, no perturbations, no decay. What follows is the *contract* for
moving a craft between the two regimes that model implies.

### One owner at a time

Exactly one regime is authoritative for a craft's state at any instant:

| Regime | What it is | When it applies |
|---|---|---|
| Local numerical | Fixed-step integration | Ascent, thrust, atmospheric flight — anywhere a non-gravitational force acts |
| Analytical coast | Closed-form conic about the central body | Everywhere else |

There is no interval during which both run and are reconciled. Reconciling two authoritative
states is how discontinuities are introduced, so the contract removes the possibility rather than
bounding the consequence.

### The handoff is lossless because the coast anchors on the state it is handed

Beginning a coast stores the handed-over position and velocity verbatim as the conic's anchor,
and every later evaluation propagates **from that anchor by total elapsed time**. Nothing is
recomputed at the transition, so the discontinuity is exactly zero — measured as exactly zero, bit
for bit, at 64 orbital phases across three orbits, and unchanged after 100 000 transition cycles.

Anchoring on classical elements instead was measured as the realistic alternative, since elements
are what an orbital map draws and what a save file wants to hold. It costs at most 0.88 µm per
transition and reaches a bitwise fixed point within six transitions, so it cannot random-walk
either. **Both are safe; the representation is a storage and legibility decision, not a numerical
one.**

Returning to the local regime has no eligibility rules of its own. The asymmetry is deliberate:
the integrator is a superset of what the conic can represent, so that direction is always valid.

### Eligibility is explicit, named, and re-checked after every change of primary

A coast may begin only when the craft is outside the atmosphere limit, above the surface, inside
the sphere of influence it claims, not under thrust, and on a non-degenerate conic. Each failure
returns the reason that names the condition rather than a boolean.

**Eligibility is re-checked after every sphere-of-influence crossing.** A state that is a valid
conic about Earth can be radial about the Moon, and a radial trajectory has no conic at all. When
the new conic is degenerate the craft drops to the local integrator, which represents radial
motion without difficulty. A3 found this by producing a physically impossible result before the
check existed.

### Crossings are discrete scheduled events

Sphere-of-influence crossings are placed to the nanosecond by bisection on the conic, and
ownership passes at that single instant with the coast re-anchored against the new primary. Three
properties make the placement reproducible:

- **The crossing predicate is a pure function of the instant** — the probe propagates from the
  coast anchor, not from wherever the current increment began. Without this, runs at different
  warp factors disagreed about when a boundary was crossed by up to 1 387 s over a three-day
  escape.
- **Increments are subdivided until a boundary is provably unreachable within them.** Sampling
  only at increment ends lets a warp tick step over a short excursion entirely, which A3 measured.
- **A hysteresis band of 10⁻⁴ of each sphere radius** stops ownership changing hands on rounding.
  Defensible because the Laplace radius is a switching convention rather than a physical surface.

With these, the same crossing is found at the identical nanosecond across warp granularities
spanning four orders of magnitude, and the state discontinuity of the re-expression is 4.0 µm at
Earth's boundary and 15 nm at the Moon's.

**The gravitational hierarchy is not the frame hierarchy.** The frame graph parents Earth and the
Moon to the Earth-Moon barycentre, which is right for coordinates. A barycentre has no mass and
owns no sphere of influence, so for gravity the Moon's primary is Earth, Earth's is the Sun, and
the Sun is the root. Both relations are carried explicitly.

### Time warp

| Rule | Why |
|---|---|
| Anchor the coast; never step it | An anchored coast never composes increments, so it is **bit-identical** at every warp factor. A stepped coast differs by ~1 mm over ten orbits and buys nothing. |
| Constrain warp ticks to integer multiples of the fixed local step | The local regime is then bit-identical too, because the step sequence is unchanged. Unaligned ticks differ by micrometres and are not reproducible. |
| Accumulate campaign time as integer nanoseconds | Two runs cannot be compared unless they reach the same instant. ADR 0010 requires this; A3 demonstrates the negative control. |

**Known limit on the campaign clock.** The nanosecond count accumulates exactly without bound,
but converting it to seconds goes through a `double`, which resolves individual nanoseconds only
to 2^53 ns = **104.25 days**. Every P1a measurement is inside that window; a multi-year campaign
is not. Determinism is unaffected either side of it — the same integer always converts to the
same `double` — so what degrades is exactness of representation, not reproducibility. A
production clock needs a coarser tick, a split representation, or elapsed times measured against
a moving anchor rather than the campaign epoch. This is a P2 design input, not an open question.

The consequence for gameplay is a constraint rather than a prohibition: **warp under thrust is not
a determinism problem, it is a quantisation problem.** Whether powered warp is desirable for
control-authority reasons is a P2/M5 question.

### Celestial origin motion

Celestial frame origins move by ADR 0011 conic propagation about each body's gravitational
primary: the Moon about Earth with μ = GM_Earth + GM_Moon, Earth and the Moon split about their
barycentre by mass ratio, and that barycentre about the Sun. This replaces A2's linear
extrapolation, from which it departs by more than 1 000 km within a day.

The Sun's motion about the Solar System barycentre **remains linear**, because ADR 0011 gives the
Sun no gravitational primary — its barycentric wobble is driven by the perturbations the ADR
excludes. This affects only the `SsbIcrf`-to-`SunIcrf` boundary, which nothing A3 gates on
crosses.

### Selected supporting values

| Choice | Value | Basis |
|---|---|---|
| Local integrator | **RK4** | Clears the 100 m one-orbit gate at a 64 s step for 332 acceleration evaluations — 3× cheaper than Yoshida 4 and 16× cheaper than velocity Verlet, each compared at the step it needs to clear the same gate. Its energy error is secular, which does not decide the choice because the hybrid contract never integrates a stable orbit for long — and the measured fifty-orbit drift is 136 µm. |
| Earth atmosphere limit | **140 km** altitude | A gameplay and physics-regime boundary, not a tolerance-derived one. Deriving it from the handoff tolerance gives ~450 km, which would make the first playable's own contract orbit un-warpable — a sign the derivation asks the wrong question, since ADR 0011 *defines* orbits as drag-free rather than approximating a drag-affected trajectory. |
| Boundary hysteresis | 10⁻⁴ of sphere radius | Tuned against chattering only, not against gameplay. |

### What this section does not decide

- **Attitude, torque, and control authority.** A3 models a point mass throughout.
- **Encounter prediction.** The machinery exists — crossings are found by bisection and are
  reproducible to the nanosecond — but predicting an encounter ahead of time is a search over
  future conics that A3 does not perform.
- **Marginal captures.** A trajectory tangent to a sphere boundary has no well-conditioned
  crossing time, which is a property of the physics rather than of the implementation. A3
  measures the cost and does not gate it; the rule a game needs is most likely a deliberate one
  about when the simulation commits to a capture.
- **Gameplay tolerances.** A3's tolerances are engineering tolerances met by six orders of
  magnitude. What a player perceives is a different question.
- **Promotion.** Nothing in `prototypes/p1a/Orbit/` is production code, and like the frame library
  it carries domain concepts.

### What would reopen it

Adding perturbations to the propagation — which ADR 0011 names as its most likely future upgrade —
reopens **every part** of this contract, because the analytical coast would stop being exact and
each tolerance above is built on its being exact. Putting a craft on the numerical integrator for
a long continuous span, such as a multi-day low-thrust transfer, reopens the integrator choice
alone.

## Implemented renderer foundation

Delivered by P1b increment B1, which is **in progress**. This section records only what exists
and passes tests today; it grows as the increment does. Unlike the P1a sections above, this is
production code from the first commit, by the user decision recorded in the P1b milestone plan.

### Production tree

```
engine/platform/ sol::platform  window and OS integration; owns GLFW, never sees Vulkan
engine/render/   sol::render    the only module permitted to see Vulkan types
tests/render/    sol::test      capability tests, capability report, and the render loop
```

`game/` and `editor/` do not exist yet.

The two engine modules meet at plain OS handles. `sol::platform` hands over `HWND`/`HINSTANCE`
as `void*`; `sol::render` builds the Win32 surface from them. GLFW can create a Vulkan surface
directly and doing so would have been shorter — it would also have put a Vulkan type in the
platform module's interface, which is the thing ADR 0002 forbids.

**Windows headers are excluded down to `NOGDI`.** `wingdi.h` defines `DeviceCapabilities` as a
macro, which silently rewrites `sol::render::DeviceCapabilities` in any translation unit that
reaches Windows headers — and every Vulkan Win32 surface does. `WIN32_LEAN_AND_MEAN` does not
exclude GDI. This is the same class of collision `NOMINMAX` already handles.

### Shader pipeline

GLSL is compiled to SPIR-V at build time by `glslc` from the pinned SDK, with
`--target-env=vulkan1.2` so a shader reaching for a later feature fails the build rather than
the one machine that lacks it. The SPIR-V is emitted as a C initialiser list and embedded in
the binary, so there is no runtime file I/O, no path resolution, and no shader-packaging
decision — that belongs to a later milestone.

### How the ADR 0002 boundary is enforced

Vulkan appears in no public header. `sol::render`'s Vulkan dependencies are linked `PRIVATE`
and `VulkanInstance` hides its implementation behind a pointer, so a consumer that wanted a
`VkDevice` could not name one. The boundary is a compile error rather than a convention.

The load-bearing consequence is `DeviceCapabilities`: a plain struct describing a device in
`std::` types, with no Vulkan handle in it. It exists because the boundary demanded it, and it
turned out to be what makes the capability gate measurable at all — see below.

### Capability model

Three things kept separate, because only the third needs a GPU:

| Piece | What it is |
|---|---|
| `CapabilityRequirement` | What the renderer demands, declared as data |
| `checkCapabilities()` | A pure function of (requirement, capabilities) returning every unmet requirement |
| `VulkanInstance::enumerateDevices()` | Queries real devices into plain values |

This separation is required by the accepted [reference-hardware evidence plan](../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md).
Both GPUs available to this project exceed the Vulkan 1.2 candidate floor and satisfy every
requirement, so on real hardware the rejection path is code no run would ever enter — and a
gate that is never exercised is not a gate. Hand-written capability values are how it is
exercised.

The requirement set B1 imposes is deliberately small, because each entry narrows the hardware
the game runs on and ADR 0002 forbids undemonstrated requirements:

- Vulkan API 1.2, the ADR 0002 candidate floor;
- `VK_KHR_swapchain`, since core Vulkan has no presentation;
- `VK_FORMAT_D32_SFLOAT` usable as a depth/stencil attachment — the one hard hardware demand,
  because a 24-bit normalised depth buffer cannot hold a surface-to-orbit range without
  collapse and the depth gate is measured against reversed-Z 32-bit float;
- one queue family with both graphics and presentation;
- `maxImageDimension2D` of at least 4096, which is Vulkan's own guaranteed minimum and so can
  only reject a non-conformant implementation.

No device *feature* is required. `shaderFloat64` is specifically not required: camera-relative
rendering exists precisely so the GPU never needs double precision, and requiring it would
contradict the frame model selected in P1a increment A2.

### Loader failure is a value, not a crash

Nothing links `vulkan-1.lib`. volk resolves entry points at runtime, so a machine with no
Vulkan driver produces a diagnostic the renderer chose to write rather than an OS-level
"DLL not found" before any project code runs. ADR 0002 requires the shipped game to detect the
loader and fail clearly; this is the mechanism.

### Measured on this machine, 2026-08-13

Both present devices meet the requirement set. Recorded under their own names — neither is a
baseline class, and neither may be reported as a proxy for one:

| Device | Kind | Device API | Driver | Queue families |
|---|---|---|---|---|
| NVIDIA GeForce RTX 4060 Laptop GPU | discrete | 1.4.312 | 581.15.0.0 | 6, one graphics+present |
| Intel UHD Graphics (Alder Lake-P) | integrated | 1.4.323 | 101.7082 | 2, one graphics+present |

Loader instance API 1.4.357, validation layer installed and active.

The driver strings above are Vulkan's vendor-specific encoding and are **the same drivers** the
evidence plan's inventory lists as `32.0.15.8115` and `32.0.101.7082`; those are the Windows
driver-package versions. Neither encoding is comparable across vendors, and a reader
reconciling the two documents should expect the pair rather than a discrepancy.

Validation is captured programmatically through a debug messenger chained into instance
creation, so messages emitted during `vkCreateInstance` itself are not lost. The run above
produced three, all `LLP_LAYER_3` loader warnings about a third-party overlay layer installed
on the machine (`GalaxyOverlayVkLayer`), and none from this project's Vulkan usage. Under ADR
0002 these fall in the "explained and accepted" category rather than counting against the
clean-validation gate.

These numbers live here mid-increment under the lightweight-lane rule in `AGENTS.md`. There is
no `evidence/p1b/` yet; full evidence with raw-output locations attaches at increment closure.

### Presentation and depth

The renderer presents through a classic render pass — not dynamic rendering, which would have
meant requiring a capability above the 1.2 floor for convenience. Two frames in flight; one
present semaphore **per swapchain image** rather than per frame, because a per-frame signal
semaphore can still be owned by an outstanding present when the driver returns images out of
order.

Present mode is **FIFO**, and that is a measurement decision rather than a default. FIFO is the
only mode Vulkan guarantees, and it presents every frame exactly once at a fixed cadence; a
mode that drops frames would inject presentation variance into the screen-space jitter gate,
which measures per-frame centroids and is supposed to isolate the renderer.

Depth is **reversed-Z with an infinite far plane**, in a 32-bit float buffer:

- Near maps to 1 and far to 0, so floating-point depth's precision — which is concentrated near
  zero — lands at the near plane where it is needed instead of at the far plane where it is
  not. This is why the capability set demands a float depth format rather than a normalised
  one, and it is the mechanism the depth gate rests on.
- There is no far clip. Choosing a far distance for a scene holding both a launch pad and a
  planet is a choice between clipping the planet and destroying near precision; the infinite
  form costs nothing once Z is reversed.
- The depth compare is `GREATER`, and depth clears to 0.

The GPU never receives a world coordinate. Object positions are subtracted from the camera
position in `double` and only then narrowed to `float`, in one function, so there is a single
line to audit when the jitter gate is measured. The view matrix has no translation term at all:
the camera *is* the origin. A conventional view matrix would reintroduce the camera's world
magnitude in `float`, which is exactly what the frame model selected in A2 exists to prevent.

### Screen-space jitter: first gating result

**Measured 2026-08-13 on the RTX 4060 Laptop GPU at 1280×720, Release.** Both reference views
the P1b plan requires, 600 measured frames each after 30 warm-up:

| View | Camera world magnitude | Jitter X max | Jitter Y max | Frames |
|---|---|---|---|---|
| Surface anchor | 6 378 km from origin | 0.000000 px | 0.000000 px | bit-identical |
| Orbital vantage, 200 km | 6 578 km from origin | 0.000000 px | 0.000000 px | bit-identical |

Against a 0.25-pixel gate. The frames are bit-identical, so the jitter is exactly zero rather
than merely small.

**Why a zero here is not self-evidently meaningful, and what makes it so.** The gate holds
everything constant and measures temporal stability. A renderer that narrowed world coordinates
to `float` before subtracting the camera — the precise mistake `cameraRelative()` exists to
prevent — would also render bit-identical frames and also score zero. It would simply be stably
wrong. The gate alone cannot tell the two apart.

A **sub-pixel response control** does. The camera is stepped laterally by 10 mm at a world
magnitude where one ULP of a `float` is 0.5 m — fifty times the step — and the marker's
centroid is required to track the geometric prediction:

| Quantity | Value |
|---|---|
| Predicted shift | 0.169439 px/step |
| Observed shift | 0.169654 px/step (0.13% agreement) |
| Worst per-step error | 0.002529 px |
| Monotonic | yes |

With `float` world coordinates every step would fall inside one representable value and the
response would be either nil or a ~12-pixel jump. Resolving motion fifty times below the float
floor, smoothly and to a fraction of a percent, is what makes the zero-jitter result evidence
about precision rather than only about stability. **A2's frame-model selection is confirmed
under the renderer, not merely assumed compatible with it.**

Two measurement-method details were forced by getting this wrong first, and both are load
bearing rather than incidental:

- **The marker needs a smooth intensity profile.** With hard edges and no antialiasing, pixel
  coverage is binary, the rasterised footprint changes in whole-pixel steps, and the centroid
  quantises to roughly a pixel — so a 0.25-pixel gate is unmeasurable and a zero reading means
  only "below the quantisation floor". The sub-pixel information has to live in intensity,
  which is how astrometry has always solved this.
- **No hard boundary anywhere in the measured region.** A `discard` cutoff, or the geometry's
  own edge, makes pixels pop in and out discretely and puts the quantisation straight back. The
  profile decays below the detection threshold on its own, and the centroid weight is
  background-subtracted so a pixel at the threshold contributes zero.

### Depth: second gating result

**Measured 2026-08-13 on the RTX 4060 Laptop GPU, Release**, with the camera held at
Earth-radius magnitude throughout so the sweep exercises real world coordinates.

Under the reversed-Z infinite projection the stored depth is an analytic function of distance,
`depth = nearPlane / distance`, so a depth value converts back to a distance exactly. That is
what turns this threshold from an inspection into a measurement. Depth is read back from the
buffer and three things are checked across nine orders of magnitude, from 1 m to 10 000 km:

| Distance | Depth | Predicted | Relative error | Resolvable separation | Separation / distance |
|---|---|---|---|---|---|
| 1 m | 1.052632e-1 | 1.052632e-1 | 7.8e-8 | 60 nm | 5.96e-8 |
| 1 km | 1.052632e-4 | 1.052632e-4 | 5.1e-8 | 31 µm | 3.05e-8 |
| 100 km | 1.052632e-6 | 1.052632e-6 | 7.7e-8 | 3.9 mm | 3.91e-8 |
| 1 000 km | 1.052632e-7 | 1.052632e-7 | 3.1e-8 | 94 mm | 9.38e-8 |
| 6 378 km | 1.650375e-8 | 1.650375e-8 | 1.0e-7 | 150 mm | 2.35e-8 |
| 10 000 km | 1.052632e-8 | 1.052632e-8 | 2.8e-9 | 500 mm | 5.00e-8 |

- **Depth matches the prediction** to within 1e-7 relative everywhere — float round-off, not
  model error.
- **No collapse anywhere.** Two distinct distances always produce two distinct stored depths.
  The resolvable separation is measured by bisection rather than asserted; z-fighting is the
  same phenomenon, so this measures the z-fighting threshold too.
- **No near/far discontinuity.** The resolvable separation tracks distance *linearly* — the
  ratio stays in a 2.4e-8 to 9.4e-8 band across the whole range, a 4× spread that is accounted
  for by a float's ULP doubling within each binade plus the bisection's bracket.

In practical terms: **depth resolves about 6 cm at 1 000 km and 15 cm at Earth's radius**, and
the figure scales with distance rather than degrading.

**The negative control is what makes those numbers mean something.** A conventional finite-far
projection — what reversed-Z replaced — is run through the identical harness, differing only in
the projection matrix, the depth compare op, and the clear value. It degrades monotonically and
then fails outright:

| Distance | Separation / distance, conventional |
|---|---|
| 1 m | 5.96e-8 |
| 10 m | 2.62e-6 |
| 100 m | 3.70e-5 |
| 1 km | 1.80e-4 |
| 10 km and beyond | **collapsed** |

That degradation curve *is* the near/far discontinuity the threshold names, and the production
path does not have it. A depth test that could not produce this failure would say nothing about
the one that passes.

**Still not measured:** LOD continuity does not exist to measure, and the capability-reporting
gate's synthetic profiles remain unverified against real device reports.

**Cost recorded rather than absorbed:** the depth attachment now uses `STORE` rather than
`DONT_CARE` so the buffer survives the render pass for readback. A production renderer without
readback would discard it. That write bandwidth is included in any frame-time figure this
renderer produces. Frames presenting is not evidence that the depth range behaves.

## Proposed architecture

Everything below this point is proposed and unimplemented.

## Architectural goal

SolEngine supplies the focused services needed to build one demanding 3D space simulation game. It is not intended to become a general-purpose engine. The game remains the validation surface for every engine feature.

## Accepted foundation

Decisions accepted at the planning gate. Where A1 has since implemented and measured one of
these, the [Implemented build foundation](#implemented-build-foundation) section above is
authoritative for the literal values.

- C++23 with the canonical `sol` namespace.
- PascalCase C++ filenames/types, camelCase functions/locals/parameters, `m_`-prefixed private members, `kPascalCase` constants, and SCREAMING_SNAKE_CASE macros (ADR 0003).
- MSVC on Windows x64.
- CMake with separate Debug and Release single-configuration Ninja presets.
- `CXX_STANDARD 23`, `CXX_STANDARD_REQUIRED ON`, and compiler extensions disabled on project-owned targets.
- Baseline reference PC: Intel Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, and SSD; target 60 FPS at 1080p on low/medium settings.
- Integrated investigation tier: Intel UHD 630 and AMD Vega 8-class graphics; target 30 FPS at 720p/low, subject to P1 Vulkan/driver validation.
- SolEngine is initially a set of internal static libraries with no stable C++ binary ABI or shared-library export surface (ADR 0005).
- Source-level module interfaces use owned subsystem namespaces such as `sol::core`, `sol::render`, `sol::platform`, and `sol::assets`. Implementation-only symbols live under the owning subsystem's `detail` namespace, and public source APIs receive Doxygen contracts (ADR 0006).
- vcpkg manifest mode with a reviewed `builtin-baseline` is the default C/C++ dependency acquisition policy; no package is selected until an owning milestone accepts it (ADR 0007).
- The JPL DE440/DE441 solution family supplies initial astronomical reference states, with each fixture recording its actual product and with player-facing UTC converted at a pinned-data TDB ephemeris boundary; the P1 launch anchor is 28.0° N, 80.5° W, 5 m above the reference ellipsoid, and every geodetic coordinate carries its datum (ADR 0008, amended after P1a increment A2).
- Persistence uses human-readable UTF-8 JSON for settings/content, JSON-manifest blueprint packages, and JSON-manifest chunked binary campaign containers, with concrete encodings selected later (ADRs 0004 and 0009).
- Determinism is bit-exact on the same build and machine, tolerance-based across machines. `/fp:precise` and `/arch:AVX2`, never `/fp:fast`; `double` for authoritative state, deterministic iteration order, seeded generators, and integer campaign-time accumulation (ADR 0010).
- Orbital propagation uses patched conics with spheres of influence. No perturbations, drag, or decay enter the propagation; aerodynamic forces still act on active craft inside the atmosphere in the local regime (ADR 0011).
- Assets are authored in Blender, interchanged as glTF 2.0 with metric units and explicit axis conversion, generated procedurally where parametric, and baked at build time into an engine-ready runtime format. The runtime loads only baked assets (ADR 0012).

Vulkan is the preferred graphics direction, not yet an accepted production implementation dependency. P1b uses Vulkan 1.2 as the candidate floor, queries actual device capabilities, and treats later capabilities as optional. It must establish surface-to-orbit render precision, depth behavior, LOD continuity, baseline discrete-GPU capability support, UHD 630/Vega 8-class status, and a validation/capture workflow. ADR 0002 closes on that evidence plus a documented Direct3D 12 analysis; a comparison spike is not required.

Renderer frame time is recorded in P1b but **gated in M2**, where the scene contains representative assets: p95 no greater than 16.67 ms at 1080p low/medium on the discrete baseline, with a spike criterion of p99 no greater than 25 ms and no frame exceeding 33 ms. The integrated investigation tier is measured against 33.3 ms p95 at 720p/low and its support status is decided at M2. See ADR 0002 and the P1b milestone plan.

## Proposed layer model

```text
Game presentation and player workflows
  construction | flight | map | science | company | strategy
                         |
Game-domain simulation
  spacecraft | astrodynamics | environment | people | economy | factions
                         |
SolEngine services
  application | time | tasks | assets | rendering | input | audio | UI
  persistence | diagnostics | math/units | physics integration | platform
                         |
Pinned third-party libraries and Windows/platform APIs
```

The dependency direction is downward. Generic engine services must not depend on game-domain systems. Domain systems may use engine abstractions, but should remain runnable headlessly when rendering and input are irrelevant.

## Proposed runtime worlds

The game needs multiple representations of one authoritative universe:

1. **Campaign universe:** persistent identities, ownership, discoveries, organizations, resources, scheduled events, and coarse background activity.
2. **Orbital/trajectory world:** celestial states and analytical or numerical trajectories over large distances and long time spans.
3. **Local physics world:** active craft, nearby bodies, contacts, joints, aerodynamics, damage, and people at a bounded scale and timestep.
4. **Render world:** camera-relative transforms, visible terrain patches, effects, and interpolation derived from simulation state.

These are representations, not independent sources of truth. Transitions must have explicit ownership rules, invariants, tolerances, and tests.

## Seamless scale strategy

“Seamless” is a player-facing contract: an active craft can travel from a planetary surface through atmosphere to orbit without a loading screen or a discontinuous game-mode jump. It does not require one coordinate frame, one physics timestep, or uniform detail everywhere.

The leading design is:

- hierarchical/dynamic reference frames for simulation;
- double-precision authoritative state where required;
- camera-relative rendering and floating local origins;
- planet-local terrain and atmosphere representations with level of detail;
- high-fidelity integration for active/local objects;
- analytical propagation for stable inactive/coasting objects;
- aggregate/event-driven updates for distant economic and population systems later.

This design is provisional until technical prototypes measure precision, stability, transition continuity, performance, and time-warp behavior.

The content baseline uses real Solar System names, dimensions, orbital distances, and vetted astronomical data. The first playable limits high-detail surface content to one bounded launch region while retaining full planetary radius and astronomical scale. Fictional corporations and politics avoid binding game progression to real institutions.

The initial authoritative campaign epoch is displayed as 2026-01-01 00:00:00 UTC. The DE440/DE441 solution and pinned NAIF generic kernels provide the initial reference data; ephemeris fixtures are evaluated at a TDB boundary with complete provenance (ADR 0008). P1 uses 28.0° N, 80.5° W, and 5 m above the reference ellipsoid as its reproducible launch anchor. The final facility remains fictional and geographically distinct from real launch complexes; its name, authored terrain placement, and regulatory/operating arrangements remain game-design decisions.

## Time model

The architecture must separate:

- wall-clock time;
- render/interpolation time;
- fixed local-physics time;
- authoritative campaign/orbital time;
- time-warp policy and scheduled-event processing.

Systems must declare which clock they consume. High warp may require leaving local physics for analytical propagation; powered flight under warp is an open design decision.

UTC is a player-facing representation and TDB is the accepted ephemeris evaluation boundary. The engine's monotonic campaign-time storage and Earth orientation model remain milestone decisions; they must convert explicitly rather than treating UTC and TDB as interchangeable scalar values.

## Data and persistence

Modding and eventual save compatibility are confirmed goals. Proposed principles:

- stable persistent identifiers independent of memory addresses or transient ECS handles;
- schema and content-pack versions from the first persistent save;
- data-driven part, material, resource, research, contract, and organization definitions;
- validation with actionable diagnostics before content enters a running universe;
- explicit migrations when compatibility becomes supported rather than best-effort silent loading;
- separation of player-authored craft blueprints from campaign saves so designs can be shared/imported.

Artifact families and compatibility are accepted in ADRs 0004 and 0009. Internal pre-alpha saves and blueprints may be invalidated with clear notices. Version 1.0 must migrate every supported public-alpha save and blueprint; later releases support their current major series plus the final supported schema of the immediately preceding major series. Concrete archive, compression, binary encoding, registries, and mod packaging remain milestone decisions.

## Verification strategy

The proposed test pyramid is:

- pure unit tests for math, units, identifiers, registries, and business rules;
- deterministic headless scenario tests for trajectories, staging, resources, science, contracts, and economy;
- transition tests between local physics, analytical propagation, save/load, and time-warp states;
- golden/reference cases derived from trusted astrodynamics sources where appropriate;
- performance budgets and soak tests for long-running universes;
- interactive visual and control smoke tests for rendering, construction, and flight.

Prototype tolerances, benchmark hardware, scenarios, and measurement rules are defined in the [P1a](../SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md) and [P1b](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) milestone plans. Renderer frame-time gating is owned by M2 rather than P1b. Later production tolerances remain attached to their owning milestones.

Determinism is scoped by ADR 0010: bit-exact on the same build and machine, tolerance-based across machines. Scenario tests may assert exact numerical values on the development machine; any future CI on differing hardware must use documented tolerances from the start.

## Unresolved foundation choices

Production renderer adoption/abstraction, validation of the integrated investigation tier, window/input library, UI, physics integration, ECS/data model, audio, concrete serialization/archive libraries, test framework, terrain implementation, campaign-time tick rate, Earth orientation, craft physical representation, and mod packaging require explicit decisions before their owning milestones.

ADR 0010 fixes campaign time as an integer accumulation with an explicit tick rate; the tick rate itself and the Earth orientation model remain open. ADR 0011 fixes the orbital model; the atmospheric boundary altitude at which propagation switches between drag-affected local integration and drag-free conic coast remains open. P1b increment B2 decides the craft physical representation.
