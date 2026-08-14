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

A camera whose forward and up are parallel, or either of which is zero, is **refused rather than
resolved**. The side axis is their cross product, so the camera's roll about its own view
direction is undefined and there is no correct value to pick. Until 2026-08-13 `normalise`
returned a zero vector by design and the frame rendered as a bare clear with no error. The
tempting fix is a fallback up axis, and it is the wrong one here: it renders a plausible image at
an arbitrary roll, and a centroid measured in a rotated frame is still a number.

The surface colour format is a **requirement, not a preference**: presenting needs
`B8G8R8A8_SRGB` or `R8G8B8A8_SRGB` in `SRGB_NONLINEAR`, and device creation fails otherwise with a
diagnostic naming both the requirement and what the surface offered. Two things depend on it and
both fail silently rather than loudly. An sRGB surface makes the display hardware apply the
transfer function so shader output stays linear; a UNORM surface double-applies gamma, and every
pixel gate — the jitter centroid especially — would then be measured through a tone response
nothing in the harness models, while still producing a number. And the capture path sizes its
staging buffer at 4 bytes per pixel and reads back byte triples, which a wider format would
overrun.

Until 2026-08-13 this was a preference with a `formats.front()` fallback, and the margin was
thinner than it read: on the RTX 4060 the surface lists `B8G8R8A8_UNORM` **first**, so the
fallback was precisely the gamma-doubling case, and the list also offers
`A2B10G10R10_UNORM_PACK32`, which would have satisfied the 4-byte stride while breaking the byte
triples. The old code reached the right format only because the sRGB one happened to be offered.

A frame that renders reports `FrameStats::presented`, and every path that returns without drawing
leaves it false. This is a measurement contract, not bookkeeping. The readback buffers are
persistently mapped and keep their previous contents, so a caller that cannot tell a skipped frame
from a rendered one reads the *last* frame's pixels and scores the duplicate as a sample — which
is silent, because the duplicate is a well-formed frame. A minimised window was exactly that hole
until 2026-08-13: it returned early without rebuilding anything, so the existing
`swapchainRebuilt` flag stayed false and the capture path handed back stale pixels. In the jitter
gate a duplicate contributes zero centroid deviation and strengthens the bit-identical reading; in
the LOD gate it contributes a zero frame difference and pulls down the median that the outlier
floor and every ratio are measured against. The flag is set at one point, after a successful
present, so every early return is false by construction rather than by each path remembering.

Covered by `render.renderer-contract`, which reaches the zero-area path through
`notifyResized(0, 0)` rather than asking a window manager to minimise a window on cue. It asserts
that the frame reports `presented == false`, that the capture comes back empty, and — if it does
not — that the returned pixels are byte-identical to the previous frame, so a failure names the
stale-readback defect instead of merely reporting a wrong size. Verified by reverting the guard,
against which all three assertions fire.

Depth is **reversed-Z with an infinite far plane**, in a 32-bit float buffer:

- Near maps to 1 and far to 0, so floating-point depth's precision — which is concentrated near
  zero — lands at the near plane where it is needed instead of at the far plane where it is
  not. This is why the capability set demands a float depth format rather than a normalised
  one, and it is the mechanism the depth gate rests on.
- There is no far clip. Choosing a far distance for a scene holding both a launch pad and a
  planet is a choice between clipping the planet and destroying near precision; the infinite
  form costs nothing once Z is reversed.
- The depth compare is `GREATER`, and depth clears to 0.

Object *origins* are subtracted from the camera position in `double` and only then narrowed to
`float`, in one function, so there is a single line to audit when the jitter gate is measured.
The view matrix has no translation term at all: the camera *is* the origin. A conventional view
matrix would reintroduce the camera's world magnitude in `float`, which is exactly what the
frame model selected in A2 exists to prevent.

This paragraph previously opened "the GPU never receives a world coordinate", which is false and
was withdrawn on 2026-08-13. It holds for terrain, which narrows per vertex on the CPU. It does
not hold for the reference objects: their vertex positions are formed *on the GPU* as
`origin + unitCube * radius`, so a planet-sized object's vertices carry its own radius as their
magnitude. The retraction was recorded in the changelog and in the status handoff at the time
and did not reach this document, which is the same drift that entry was itself about.

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
response would be nil. Resolving motion fifty times below the float floor, smoothly and to a
fraction of a percent, is what makes the zero-jitter result evidence about precision rather
than only about stability.

**Two corrections, because both were published wrong.** The control originally stepped the
camera along an axis whose magnitude *cancels exactly* — the marker sits at `camera + offset`,
so every untouched component is bit-identical in both operands and subtracts to zero in `float`
just as in `double`. A fully `float` pipeline passed it to within 4e-9 px. It now steps the
axis that carries the 6 378 km magnitude, where a 10 mm step is 1/50 of a `float` ULP, so a
`float` pipeline produces exactly zero response. Only now does the control discriminate.

And this **does not confirm A2's selected model**, as an earlier version claimed. It confirms
that camera-relative rendering with `double` world coordinates survives the renderer.
`WorldVec3` is a bare `double` triple with no frame identity, no parent and no epoch; nothing
here walks a frame graph to a lowest common ancestor or checks an epoch. What is exercised —
subtract in `double`, then narrow — is common to A2's hierarchical model *and* to the
global-root candidate A2 rejected, and a single unlabelled `double` world frame inherits exactly
the global-root limit A2 recorded. The P1b done-criterion that A2's model supports the jitter
gate is **not yet met**.

The gate itself is also magnitude-inert for the same reason the old control was: with the
camera stationary, `cameraRelative()` returns a constant, bitwise identical to running at the
world origin. The gate measures stability; the control is what carries precision.

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
  ratio stays in a 2.4e-8 to 9.4e-8 band across the whole range.

**Correction to the figures in that table.** The bisection finds the distance to the next
*rounding boundary*, which lands anywhere within one ULP depending on where the probe sits —
not the quantisation step. Those numbers are therefore best-case draws, and the 4× spread is
that rounding lottery, not binade doubling: binades can only produce 1.6×. The bisection-bracket
half of the old explanation was wrong by four orders of magnitude, since the search converges to
1e-12 of the distance.

The figure that matters for z-fighting is the **guaranteed** separation — one full ULP, at which
two surfaces are always distinguishable — and it is roughly **7 cm at 1 000 km and 69 cm at
Earth's radius**. An earlier version quoted the best-case draws as the result, and gave 94 mm
and "about 6 cm" for the same quantity in two different places.

The conclusions are unaffected: depth still never collapses, and the separation still scales
with distance rather than degrading. Only the quoted magnitudes were optimistic.

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

### Terrain and level of detail

A real-scale planet cannot be one mesh — Earth's surface at one-metre resolution is on the
order of 10^14 vertices — so the renderer chooses each frame which parts of the surface to
represent finely. The scheme is CDLOD on a cube-sphere: six quadtrees, subdividing when the
camera is inside a level's range, with **every vertex carrying both its own position and the
position it would occupy one level coarser**. A morph factor evaluated **per vertex, in the
vertex shader, from that vertex's own camera distance** blends between them, so a fine mesh
continuously *becomes* the coarse one and the switch happens when the two are already identical.
(This paragraph said "per-patch" until 2026-08-13, describing the arrangement the same section
goes on to record as a defect. Per patch is precisely what does not work.)

Terrain vertices are emitted **camera-relative**, subtracted in `double` on the CPU. A
planet-relative position is ~6.4e6 m, where a `float` quantises to half a metre; subtracting
the camera in the shader would be catastrophic cancellation and the terrain would visibly
quantise near the viewer.

Shading is a function of terrain height only, never of LOD level. Colouring by level would
paint a hard seam at exactly the transitions the gate measures.

Each patch's grid is sampled once. A vertex's coarse counterpart is that vertex snapped to even
indices, which is how the parent grid samples the patch — and `surfacePoint` there evaluates the
identical expression on identical operands as the fine sample already computed at that position,
so it is read out of the fine grid rather than recomputed. Until 2026-08-13 it was recomputed,
costing a second ten-octave noise evaluation per vertex for a value already in memory, with only
25 of a 9×9 patch's coarse samples distinct and each recomputed up to four times. That is
**162 `surfacePoint` calls per patch reduced to 81**, exactly half, with bit-identical output —
measured on the LOD gate at **8 m 29.6 s → 5 m 26.0 s**, 36% of total wall time on a run that is
substantially GPU work and readback.

Terrain is culled against **both** the horizon and the view frustum. The frustum planes are
extracted from the frame's view-projection by Gribb-Hartmann; because the view matrix is
camera-relative and carries no translation, they arrive in camera-relative space, which is the
space terrain is emitted in. The matrix is computed once per frame and handed to both the
selection and the draw, so the frustum culled against is by construction the frustum rasterised
with.

The infinite far plane appears as a **degenerate row**: under reversed Z the `z >= 0` clip
condition reduces to `near >= 0`, whose xyz part is exactly zero, because there is no far clip to
impose. Dropping zero-length normals is how that missing plane is handled — five planes under the
production projection, six under the conventional control, from the same function.

Culling happens **before** the subdivision decision, so a rejected node takes its whole subtree
with it; a node's children lie within its own extent, so the bounding sphere that bounds the node
bounds the subtree.

This was added while the LOD gate's pop detector cannot fire, which normally would be the wrong
order of work — a visibility cull is exactly the kind of change that makes patches appear and
disappear, and the horizon cull was itself a pop source when it was wrong. What makes it
verifiable anyway is that a conservative cull removes only geometry that was never visible, so
**the rendered image must be unchanged**. That is the acceptance test, and it holds: across the
gate's 600-step descent in both configurations, every image-derived statistic is identical —
pops, worst local ratio 5.53×, medians 0.0832 and 0.0885, maxima 0.2014 and 0.3222, and both
worst-concentration figures. Only the counts moved: **peak patches 1 008 → 240**, peak vertices
81 648 → 19 440, and gate wall time **5 m 26.0 s → 1 m 59.2 s**. Device allocation is unchanged at
43.03 MiB, since capacity is fixed rather than demand-driven.

One second-order effect is worth recording, because it looks like a regression and is not. The
gate's transition *sweep* reports different numbers, because `findTransitionAltitude` locates its
band by the largest patch-count change and the counts moved — so the scan selected a different
transition (213 432 m against 140 866 m) and the sweep measured a different one. The sweep is
therefore not stable under changes to patch selection, which is a property of that instrument
rather than of the renderer, and another reason the descent rather than the sweep carries the
verdict.

### LOD gate: not satisfied, and not certifiable as measured

**Memory is bounded structurally. It is not measured, and the earlier claim that it was is
withdrawn.** Two independent reasons, both of which the previous wording obscured:

- **The criterion is a 30-minute traverse.** What runs is 600 render steps — tens of seconds.
  Substituting a shorter test for the stated duration is exactly the kind of threshold change
  the P1b plan says may only be made by a documented, user-approved planning update. It was
  made silently.
- **The number cannot vary.** Device allocation is reported from VMA over the whole allocator,
  and every allocation happens once during creation; nothing is allocated or freed during a
  traverse. `max == min` with a zero trend is therefore a tautology, not an observation. The
  reported total is arithmetically identical to the sum of the fixed allocations.

What *is* true, and is the honest form of the claim: the terrain buffers are allocated once at
a fixed capacity and written in place, so a bounded working set is structural. The instrument
also cannot see host-side, descriptor-pool, or driver-side growth, since VMA does not own those.

The figure is sampled per frame with `vmaGetHeapBudgets`, summed across heaps. It was
`vmaCalculateStatistics` until 2026-08-13 — the call VMA names "calculate" rather than "get"
because it walks every block and allocation under the allocator's mutexes, and documents for
debugging rather than per-frame use. That put a full allocator traversal inside `renderFrame`,
which is the function designated as the only valid source of frame-time evidence: the instrument
was sitting inside the measurement it was going to be measured by. Nothing times frames yet, so
it cost nothing that was ever read. The two calls report the same total, verified by the LOD
gate's output being byte-identical across the change.

Capacity was sized from a patch budget after review found the vertex and index capacities
inconsistent — they were independent round numbers in a 2:1 ratio while a patch emits 81
vertices and 384 indices, so the index buffer always exhausted first and roughly 55 MB of the
vertex allocation was unreachable at any camera position. The budget is now 4 096 patches against
a measured peak of 1 008 on the gate's descent — 4.06× — and total device allocation is 43.03 MiB.

The peak was published as 874 until 2026-08-13. That figure was recorded in the same commit that
raised the gate's `subdivisionFactor` from 0.6 to 3.0 and does not correspond to any measurement
at 3.0: the peak scales with the quality setting, and the number was carried past the change that
invalidated it. The headroom it argued for was never in doubt — 4.06× is still ample — but the
margin was overstated by 15%, and a capacity justified by a stale peak is justified by nothing.

**The popping half has no trustworthy result, and the gate is recorded as incomplete rather
than passed.** Building it found two real defects, both of which would have produced a
confident false pass:

- **The morph factor was always exactly 1.0.** A node is emitted precisely when `distance >=
  range`, and the factor was computed against that same `range` — so every emitted node sat
  past the end of its own band. Terrain rendered permanently at the coarse position, morphing
  nothing. The factor must be measured against the *parent's* range, at twice the distance,
  because that is where the node is actually replaced. With the bug, enabling and disabling
  morphing produced identical frame-difference statistics to four decimal places, and a naive
  reading called that "no popping".
- **The terrain had no detail at the scale LOD operates on.** Relief topped out near 1 000 km
  of wavelength while the finest patches span kilometres, so fine and coarse grids sampled
  nearly the same height and no scheme could pop. Frame-to-frame differences never exceeded
  0.03 of one luminance level. Seven octaves reaching ~4 km wavelength raised the traverse's
  median difference from 0.0009 to 0.076.

The **metric has since been rebuilt** and is believed correct:

- **Per-pixel change distribution, not the frame mean.** A 256-bin histogram of per-pixel
  luminance change per step yields both a 99.9th percentile and a count above a perceptible
  step (16 of 255) in one linear pass. The mean was structurally wrong: morphing deliberately
  substitutes many tiny continuous deformations for one large discrete jump, so it frequently
  produces *more* total image change while being smoother. Measured with the mean across two
  altitude bands, the ordering of the two configurations reversed depending on the band.
- **The transition is located, not guessed.** Two hand-derived altitude bands each turned out
  to contain no transition at all, and in both the control measured *smoother* than the
  production path — which is what an abrupt scheme looks like when nothing switches, since its
  geometry is then perfectly static while morphing deforms continuously. The harness now scans
  for the altitude at which the renderer's patch count changes and sweeps around that.
- **Terrain is lit** by a normal recovered from screen-space derivatives, because facet
  orientation changes sharply at a tessellation change even where height barely moves.

The scene then had to be steepened before any of that could register. A level change uncovers
exactly the terrain energy between the two grids' spacings — about 1.6 km and 3.1 km at the
levels this test exercises — and the spectrum had no energy below 4 km. Ten octaves reaching
230 m, a slower amplitude falloff, and a stress relief of 20 km put real slope where the grids
differ. The stress scene is deliberately far rougher than Earth: if morphing keeps transitions
invisible on terrain this rough it keeps them invisible on gentler terrain, whereas a pass on
gentle terrain would imply nothing.

#### The measurement, and what it found

An intermediate measurement, retained because the reasoning drawn from it was wrong and the
correction matters more than the numbers: 13 pops without morphing against 2 with it, worst
magnitude 0.2822 against 0.0030.

**The two surviving pops were caused by morphing after all, and the inference recorded here
previously was invalid.** That inference ran: they occur at the same steps in both runs,
morphing is the only difference between the runs, therefore morphing is not the cause. It does
not follow. An *incomplete* morph leaves a residual discontinuity at the same step in both runs
— large without morphing, small with it — and 0.2822 against 0.0030 is precisely that signature.

The defect: **the morph factor was computed per patch rather than per vertex.** CDLOD's
continuity guarantee depends on each vertex deriving its factor from its own distance, so two
adjacent patches agree along a shared edge. A single factor from the node centre differs between
neighbours, so the shared edge becomes two different polylines; and since the subdivision event
is governed by the *parent's* centre distance while each child's factor came from its own, a
near-side child could still be short of full morph when its parent handed over. The factor is
now evaluated in the vertex shader from `length(inPosition)` — the vertex's own camera distance,
needing no extra data.

Fixing it exposed a constraint the per-patch version had hidden. The morph band is at most the
level's range, which is `subdivisionFactor` patch-widths, and a smooth per-vertex morph needs it
wider than about 2.8 of them. The 0.6 factor previously used — and wrongly described in code as
"what a shipping renderer would use for performance" — is below that floor; at 0.6 the
per-vertex morph measured *worse* than no morphing. The factor is now 3.0. That it is *also* the
renderer's default, rather than a test-only setting, became true only on 2026-08-13 — it was
asserted here before it was — and is recorded under "Recorded quality setting" below.

A third morph defect was found by review on 2026-08-13, after the per-vertex fix and outside the
gate's reach. **Root patches were pinned to full coarse morph.** A level-0 node has no parent, and
the CPU says so by emitting a zero-width morph band; the shader widened that degenerate band to a
`1e-6` epsilon before dividing, which drives the factor to 1 for any distance above a micrometre.
A root patch therefore rendered permanently at its coarse grid and jumped to its fine grid at the
instant its children took over — a discontinuity sitting inside the mechanism that exists to
remove discontinuities. The shader now treats a zero-width band as "no parent" and returns a
factor of 0.

**No measurement on this branch is affected.** Level 0 is emitted only beyond
`2R × subdivisionFactor`, about 38 000 km, while every gate runs 3–300 km at `maxLevel 10` where
level 0 always subdivides — so no gate could have seen it, and the LOD gate's output is
byte-identical either side of the fix. The defect was reachable in the renderer rather than in the
gate: any view of a planet from geostationary altitude or beyond, or any caller passing
`maxLevel = 0`.

**It is now covered by `render.renderer-contract`**, as an invariance test rather than a threshold.
With `maxLevel = 0` every emitted patch is a root, so enabling and disabling the morph must produce
*the same image, byte for byte* — there is no coarser level to blend toward. That reaches the
defect from any camera distance and runs in under a second, where a traverse step at 40 000 km
would have been slow and would still only have measured a spike. The test carries a vacuity guard,
since two identical blank frames would satisfy the invariant while proving nothing: terrain must
first be shown to cover more than 5% of the frame, and it covers 22%. Verified against the defect
by reverting the shader guard, which moved 58 720 of 66 062 terrain pixels — 89% of the planet.

**The gate still cannot be certified, and the reason has moved again.** At a factor where the
morph is well-conditioned, transitions are sub-pixel: both configurations now record **zero
pops**. Visibility and validity pull against each other through the same parameter — raising the
factor until the morph is sound is exactly what makes transitions invisible — and a coarser grid
was tried to break that tie and did not help. The renderer behaves correctly; the control cannot
demonstrate that it matters.

Two earlier candidate causes were tested and neither was it, recorded so they are not retried:

- **The horizon cull was genuinely broken** and is now fixed. It subtracted a cube-face size
  fraction from a cosine — dimensionally meaningless — and as a result admitted patches roughly
  78° past the true horizon at 250 km altitude, where the real horizon is about 16°. It is
  replaced by the exact horizon-plane test, `dot(P, Ĉ) ≥ occluder² / |C|`, applied to a node's
  whole bounding extent with the occluder shrunk and the bound grown by the relief band, so a
  node becomes eligible while still genuinely hidden. **This fix left the pops unchanged**, so
  the cull was a real defect but not this one.
- **Morphed per-vertex normals**, replacing the fragment shader's derivative-recovered normal,
  on the theory that a patch's degenerate triangles at full morph give undefined derivatives.
  Measured **strictly worse** — production pops rose 2 → 9 and the separation collapsed from
  13-vs-2 to 12-vs-9 — because a child's coarse-normal stencil clamps at its own patch boundary
  instead of reading its parent's neighbours, trading one artefact at the transition instant
  for a permanent seam around every patch. Reverted. Doing it properly needs a one-vertex skirt
  of neighbour data per patch.

### Recorded quality setting, and device attribution

The LOD threshold is defined "at the recorded quality setting", and the setting was previously
recorded nowhere. It is `subdivisionFactor = 3.0` with `gridResolution = 8` and, for the gate's
stress scene only, 20 km of relief. The relief is a deliberately extreme test configuration and
is labelled as such.

**"The renderer's default" became true on 2026-08-13 and was not true when first written.** The
public `TerrainSettings::subdivisionFactor` defaulted to **2.5** while the gate measured at 3.0 —
so the gate was not measured at the shipped setting, and worse, 2.5 is below the ~2.8 floor the
scheme needs, meaning every caller taking the default got an ill-conditioned configuration. The
default is now 3.0. `gridResolution = 8` is an internal `TerrainConfig` default and is not
reachable through the public settings struct at all, which is why it cannot drift.

The correspondence is now checked by the compiler rather than asserted by this paragraph: the LOD
gate carries a `static_assert` tying its quality constant to `TerrainSettings{}.subdivisionFactor`,
so moving the renderer's default fails the gate's build until someone decides whether the
threshold is still defined at the setting the renderer ships. That assertion was verified to fire
by setting the default to 2.9 and confirming the build broke.

Every gate also prints the shader binary that produced its numbers, e.g.
`glslc --target-env=vulkan1.2 -g -O0 (Debug)`. Debug and Release do not run the same SPIR-V —
Debug compiles `-g -O0` to keep a RenderDoc capture readable, Release `-O`, and `spirv-opt` is
free to reassociate floating-point arithmetic while doing so. ADR 0010 pins MSVC's floating-point
behaviour for the CPU and says nothing about the GPU, so nothing constrains that. Both
configurations run the full suite, so every gate result exists in two variants from two different
shader binaries. The string is baked in from the same CMake variable that builds the `glslc`
command line, so the reported flags cannot drift from the invoked ones. This **records** the
divergence rather than removing it; the Debug shaders stay unoptimised because a capture workflow
is an outstanding B1 deliverable.

**All gate results on this branch are from one device**: the NVIDIA RTX 4060 Laptop GPU, driver
581.15.0.0, at 1280×720, Release. The accepted evidence plan requires gating thresholds to be
measured on *both* available devices, and the Intel UHD (Alder Lake-P) is unmeasured for jitter
and depth. That is outstanding, not waived. The plan's residual risk also stands: a
driver-specific shader optimisation could alter precision, so these results are scoped to this
device and driver. ADR 0010 governs MSVC and CPU floating point and says nothing about GPU or
driver determinism, on which the bit-identical-frames and exact-depth-inequality results both
depend.

The gate stays registered and `DISABLED` so one known-failing gate does not mask regressions
across the other twenty-six, with the reasoning recorded in the build description rather than
hidden by it.

**Also still unmeasured:** the atmosphere, which the P1b plan's narrowing option permits as a
simple analytic shell, and the capability-reporting gate's synthetic profiles.

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
