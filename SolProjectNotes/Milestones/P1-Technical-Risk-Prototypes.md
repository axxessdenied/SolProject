# P1 — Technical Risk Prototypes

**Status:** Planned; not authorized to implement

**Outcome owner:** Unassigned until implementation is authorized. One Codex or Claude feature owner must hold the branch/worktree and remain the only writer.

**Planning source:** User approval on 2026-08-12

## Outcome

P1 produces measured evidence for the technical claims that could otherwise invalidate the first playable: astronomical-scale precision, seamless rendering, local-to-analytical orbital transitions, 150–300-part craft simulation, resource networks, and versioned persistence.

P1 succeeds by selecting, constraining, or rejecting candidate approaches. Its executables may be disposable; its scenarios, raw measurements, conclusions, and ADR updates are durable.

## Authorization and prerequisites

Implementation remains forbidden until all of the following are true:

- `docs/project_status.md` explicitly marks the planning gate **Approved**;
- the user explicitly authorizes implementation and identifies the single writer;
- the intended branch and base are recorded under the repository Git policy;
- required baseline or remote test hardware is available, or the user accepts a documented evidence plan for unavailable devices;
- each third-party package has an accepted need and completes ADR 0007's dependency workflow before its declaration is added;
- Debug and Release preset names plus the exact supported MSVC toolset are recorded during the first build increment.

## Boundaries

### In scope

- minimal instrumented executables and headless scenarios needed to answer the six risk areas below;
- reference fixtures derived from ADR 0008 with provenance, units, frames, epochs, and checksums;
- candidate algorithms or small libraries compared behind prototype-local boundaries;
- machine-readable results plus concise evidence reports;
- ADR and plan changes justified by measured results.

### Non-goals

- production gameplay, construction UI, campaign progression, polished terrain, final assets, or shippable content;
- a reusable general engine framework or production API designed around disposable prototypes;
- final gravity, atmosphere, aerodynamics, physics-library, renderer-helper, ECS, UI, audio, or serialization-library selection beyond what an increment directly proves;
- production support for integrated graphics merely because the prototype launches;
- carrying rejected prototype code into P2 for sunk-cost reasons.

## Shared measurement rules

- Performance gates use optimized Release builds with diagnostics that do not materially distort the result. Debug/validation runs are reported separately.
- Every report records commit, preset/toolchain, OS build, CPU, GPU, driver, API/device capabilities, quality settings, resolution, scenario seed/input, warm-up, sample count, and raw-output location.
- Frame-time and subsystem-time gates use p95 over at least 10,000 post-warm-up samples. CPU and GPU times are reported separately where available even when the gate applies to total frame time.
- Numerical comparisons record units, reference frame, time scale, reference result, absolute error, and relative error where meaningful.
- A threshold may be changed only by a documented planning/ADR update approved by the user; failed results must not be relabeled as passes.
- Integrated-graphics failure is an allowed result if discrete baselines pass and the investigation device is explicitly removed from promised support. Vulkan failure requires a documented alternative decision before P2 rendering begins.

## Accepted thresholds

| Area | Pass criterion |
|---|---|
| Local ↔ analytical handoff | State discontinuity no greater than 1 m position and 1 mm/s velocity per transition |
| Reference orbit | After one approximately 200 km circular-orbit period, propagated position is within 100 m of the accepted reference at the same campaign time |
| Render precision | Stationary surface and orbital reference views exhibit no more than 0.25 pixels of screen-space jitter |
| Discrete renderer | p95 total frame time no greater than 16.67 ms at 1920×1080 low/medium on GTX 1060 6 GB and RX 580 8 GB baseline classes |
| Integrated investigation | p95 total frame time measured against 33.3 ms at 1280×720 low on UHD 630 and Vega 8 classes; failure is reported rather than hidden |
| Constructed craft | At 300 parts, p95 60 Hz craft-physics work no greater than 4 ms on the accepted baseline CPU classes |
| Resource networks | At 300 parts, p95 fluid/electrical solve no greater than 1 ms in the representative network mutation scenario |
| Persistence | Round trips preserve stable IDs, topology, classified exact quantities, chronology, and content references semantically; unknown required data fails without mutation |

Peak process memory, committed GPU memory, allocations, and upload volume are mandatory measurements. Production memory caps are deferred until representative assets exist.

## Increment 1 — Measurement and build harness

### Deliverables

- the smallest C++23/MSVC/CMake/Ninja build graph needed for independent headless and graphical prototypes;
- separate Debug and Release presets with explicit source lists;
- a repeatable metrics output format and scenario metadata record;
- capability reporting for the host toolchain and, when graphics begins, the Vulkan loader/device.

### Done criteria and validation

- a clean configure/build/test succeeds through the checked-in presets using the documented toolchain and dependency bootstrap;
- one headless timing scenario and one graphical smoke scenario emit complete metadata and machine-readable measurements;
- Debug and Release artifacts cannot overwrite one another;
- the handoff records the literal configure, build, test, and scenario commands. Until preset names are accepted, command contracts are `cmake --preset <preset>`, `cmake --build --preset <build-preset>`, and `ctest --preset <test-preset>`.

### Documentation trigger

Update project status, implemented architecture, the selected MSVC minimum, preset names, and every dependency record actually introduced.

## Increment 2 — Reference frames and render precision

### Deliverables

- explicit-unit/frame types sufficient to compare candidate hierarchical origins, double-precision authoritative state, and camera-relative render transforms;
- ADR 0008 fixtures for the 2026 epoch and the 28.0° N, 80.5° W, 5 m launch anchor;
- stationary and continuous surface-to-200-km camera paths with numerical and screen-space diagnostics.

### Done criteria and validation

- frame conversions preserve state within the stricter of their measured floating-point budget or the accepted 1 m/1 mm/s transition ceiling;
- the surface and orbital stationary views meet the 0.25-pixel jitter gate;
- the continuous path shows no coordinate jump or loading-mode discontinuity;
- repeated conversions do not show unbounded drift, and every failure identifies the frame boundary responsible.

### Documentation trigger

Record the selected frame/origin model or keep alternatives explicitly open with the evidence needed to decide them.

## Increment 3 — Vulkan large-world renderer

### Deliverables

- a Vulkan 1.2 candidate path that queries all required device capabilities and rejects unsupported configurations clearly;
- minimal full-scale Earth, atmosphere/sky transition, depth stress geometry, camera-relative rendering, and representative scalable quality settings;
- validation-layer and graphics-capture workflow;
- capability and performance reports for both discrete baseline GPU classes and both integrated investigation classes when accessible;
- a focused Direct3D 12 comparison spike or evidence-based analysis sufficient to close ADR 0002.

### Done criteria and validation

- both discrete GPU classes meet the 16.67 ms p95 gate in the defined 1080p scene;
- integrated results are measured against 33.3 ms p95 at 720p/low and labeled passed, failed, or unavailable with evidence;
- the render-precision gate remains satisfied throughout the exercised surface/orbit views;
- exercised validation output is clean or every remaining message is explained and accepted;
- peak CPU/GPU memory and allocation/upload behavior are recorded;
- ADR 0002 is accepted, rejected, or superseded from evidence before this increment closes.

### Documentation trigger

Update ADR 0002, architecture, hardware support claims, graphics non-goals, and any selected shader/allocator/window/Vulkan-loading dependencies.

## Increment 4 — Hybrid orbit and time warp

### Deliverables

- a deterministic headless two-body reference scenario, clearly labeled as a prototype reference rather than the final gameplay gravity policy;
- a candidate fixed/local numerical integrator, analytical coast, explicit eligibility rules, and bidirectional handoff;
- normal-time and accelerated-coast runs of the same approximately 200 km circular orbit using the same elapsed campaign time;
- metrics for position, velocity, orbital elements, conserved quantities, transition counts, and event chronology.

### Done criteria and validation

- every local/analytical transition meets 1 m position and 1 mm/s velocity discontinuity limits;
- the accelerated one-orbit result is within 100 m of the accepted reference at the same campaign time;
- identical inputs reproduce event ordering and numerical results within declared tolerances on the same supported build/hardware;
- invalid transition conditions are rejected explicitly rather than producing a discontinuity;
- the result identifies which gravity, integrator, and warp questions remain for P2.

### Documentation trigger

Record the selected transition contract and update the open questions for gravity, powered warp, encounters, and final gameplay tolerances.

## Increment 5 — Constructed craft and resource networks

### Deliverables

- representative 150-part and 300-part staged craft graphs with fixed inputs and seeds;
- candidate rigid-body/joint update, staging separation, deterministic failure event, and instrumentation;
- explicit logical propellant/fluid and electrical graphs with disconnection, valve/switch, depletion, and staging mutations;
- timing breakdowns that separate physics, collision/joint work, fluid/electrical solving, and harness overhead.

### Done criteria and validation

- the 300-part scenario meets the 4 ms p95 craft-physics and 1 ms p95 resource-network gates at 60 Hz on the baseline CPU classes;
- staging preserves or changes mass, momentum, resources, connectivity, and identity according to the scenario oracle;
- repeated runs preserve event ordering and remain within declared numeric tolerances;
- invalid networks are reported deterministically and do not silently create craft-wide resource pools;
- any physics or graph library recommendation completes dependency review before architectural acceptance.

### Documentation trigger

Record accepted craft representation and resource-network boundaries, measured part-count limits, and dependency decisions; do not turn the 300-part test into an unlimited support claim.

## Increment 6 — Versioned persistence round trips

### Deliverables

- prototype representations for stable IDs, a craft/assembly topology, content references, resources, chronology, and artifact versions;
- UTF-8 JSON content/settings fixture, blueprint-package fixture, and chunked campaign-container fixture consistent with ADR 0009;
- save/load/resave, unknown-version, missing-content, unknown-required-chunk, corruption, and recoverable-failure scenarios.

### Done criteria and validation

- semantic round trips preserve every field classified as exact and compare tolerant numerical fields against an explicitly recorded tolerance;
- stable identities and topology do not depend on memory addresses or transient handles;
- unknown required data and unsupported future versions fail with distinct actionable diagnostics and do not mutate the source;
- a failed migration/write leaves the original or recoverable backup intact;
- no prototype encoding library becomes production architecture without dependency and persistence review.

### Documentation trigger

Update implemented persistence truth, selected encodings/dependencies, schema/version registers, and ADR 0009 only where evidence changes its boundary.

## Increment 7 — Evidence synthesis and P2 recommendation

### Deliverables

- one index linking raw results, scenario definitions, hardware/toolchain metadata, accepted failures, and conclusions from all increments;
- updated ADRs that accept, reject, supersede, or defer each candidate architecture;
- a P2 recommendation identifying reusable contracts and explicitly disposable prototype code.

### Exit criteria

- every accepted threshold has a reproducible result or an approved documented revision;
- Vulkan/alternative rendering, frame/origin strategy, hybrid transition contract, craft/graph feasibility, and persistence foundation each have a clear architectural disposition;
- integrated-graphics status is honest and does not block closure if the accepted discrete baseline passes;
- no unresolved result can silently become a P2 implementation assumption;
- `complete-milestone` review verifies evidence before project status marks P1 complete.

## Risks and recovery

- **Unavailable reference hardware:** do not substitute stronger hardware and extrapolate. Mark the device unavailable and obtain user-approved remote/borrowed evidence before making its support claim.
- **Instrumentation distortion:** compare instrumented and minimally instrumented runs; report overhead.
- **Prototype contamination:** keep production-facing APIs out of disposable experiments. Reuse requires an explicit review, not file copying by default.
- **Threshold failure:** preserve the raw evidence, reject or narrow the candidate, and update ADR/status. Do not tune away physics or simulation fidelity to disguise a graphics failure.
- **Dependency dead end:** keep the candidate behind a narrow boundary and retain a dependency-free fixture/scenario so another candidate can be evaluated.
- **Data loss:** prototype migrations and writes operate on copies or recoverable backups.

Rejected prototype code may be removed only after its evidence and decision record are retained and the exact removal scope is reviewed. No cleanup may touch production or unrelated user work.

## Handoff contract

Each increment handoff records:

- goal and current outcome;
- single owner, branch, and base;
- changed files and dependency changes;
- literal configure/build/test/scenario commands and results;
- hardware, driver, toolchain, settings, samples, raw evidence, and summary metrics;
- failed/waived criteria—waivers require prior user-approved plan changes;
- ADR/documentation changes, remaining risks, disposable code, and the smallest next action.

## Decisions

| Decision | Status | Why |
|---|---|---|
| P1 is evidence-oriented and may use disposable code | Confirmed | User approved risk-first vertical slicing and measurable criteria |
| Vulkan 1.2 candidate floor with queried optional capabilities | Confirmed for P1 | User approved recommendation; production Vulkan remains Proposed in ADR 0002 |
| Discrete and integrated frame-time gates above | Confirmed | User approved recommendation |
| 1 m/1 mm/s handoff and 100 m one-orbit gates | Confirmed | User approved recommendation |
| 0.25-pixel stationary jitter gate | Confirmed | User approved recommendation |
| 300-part 4 ms physics and 1 ms resource-network gates | Confirmed | User approved recommendation |
| Persistence semantic round-trip invariants | Confirmed | User approved recommendation and ADRs 0004/0009 |
| Integrated investigation failure may narrow support without failing P1 | Confirmed | User approved conditional integrated-graphics goal |
| Exact production libraries and reusable code | Open by owning increment | Must follow evidence and ADR 0007 rather than be selected globally |
