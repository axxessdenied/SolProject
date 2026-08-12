# Architecture

**Status:** Mostly proposed pre-production architecture. The [Implemented build foundation](#implemented-build-foundation) and [Selected reference-frame model](#selected-reference-frame-model) sections below are implemented technical truth as of P1a increments A1 and A2; everything else remains proposed and is not implemented. Accepted decisions are recorded in `docs/decisions/`.

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

No third-party dependency is used. Scenarios are plain executables registered with CTest, so
ADR 0007's dependency workflow has not yet been triggered and no `vcpkg.json` exists.

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
  self-consistent frame kinematics, not an ephemeris. Propagation belongs to increment A3 under
  ADR 0011.
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
