# P1b — Renderer and Craft Prototypes

**Status:** **Increment B1 authorized to implement** on 2026-08-13. Increment B2 remains planned and not authorized. Authorization state is owned by [`docs/project_status.md`](../../docs/project_status.md); this line reflects it and does not grant it.

**Outcome owner:** **Claude**, sole writer, on branch `feature/p1b-vulkan-renderer` from base `dev`.

**Planning source:** User approval on 2026-08-12; scope split and revision approved 2026-08-12

**Predecessor:** [P1a — Precision and Orbit Prototypes](P1a-Precision-and-Orbit.md)

## Outcome

P1b produces measured evidence for the two remaining technical claims that could invalidate the first playable: that a Vulkan renderer can present a real-scale body seamlessly from surface to orbit without precision or level-of-detail dead ends, and that a 150–300-part constructed craft with explicit resource networks can be simulated within a viable budget.

P1b succeeds by selecting, constraining, or rejecting candidate approaches.

## What P1b does and does not gate

P1b gates on **correctness, precision, and the absence of architectural dead ends**. It does not gate on production frame rate.

Frame time in a prototype scene with placeholder geometry, no production part meshes, and no authored terrain is weakly predictive of the shipped renderer. Measuring it is still mandatory — it is the baseline that M2 is compared against — but a slow prototype is a datum, not a failure. **The 16.67 ms discrete-baseline gate is owned by P2/M2**, where the scene contains representative assets.

This is a deliberate revision of the original P1 plan, which gated ADR 0002 on prototype frame time. ADR 0002 has been rewritten to close on P1b's precision and capability evidence, with frame-time confirmation deferred to M2.

## Code disposition

P1b differs from P1a on code reuse, by user decision:

- **Increment B1 (renderer) is built in the production tree.** A working Vulkan instance, device selection, swapchain, synchronization, and pipeline setup is never realistically discarded, and pretending otherwise invites a rewrite that reproduces the same code with less evidence behind it. B1 code is production code from the first commit and is held to production standards for structure, naming, and documentation.
- **Increment B2 (craft physics) remains disposable.** Its purpose is to compare two craft representations and reject one. The rejected representation's code is removed; the surviving one informs a P2 design rather than becoming it by default.

Vulkan types must stay behind renderer-owned interfaces from the first commit, per ADR 0002. Prototype urgency does not license leaking them upward.

## Authorization and prerequisites

Implementation of P1b remains forbidden until all of the following are true:

- P1a is complete, with its frame/origin model selected and recorded;
- `docs/project_status.md` explicitly marks the planning gate **Approved** and implementation authorization **Granted**;
- the single writer, branch, and base are recorded;
- required baseline GPU hardware is available, or the user accepts a documented evidence plan for unavailable devices;
- each third-party package has an accepted need and completes ADR 0007's dependency workflow before its declaration is added.

For increment B1 these are satisfied as of 2026-08-13. The hardware prerequisite is discharged by the second branch, not the first: none of the four named device classes is present, and the user accepted the [P1b Reference Hardware Evidence Plan](P1b-Reference-Hardware-Evidence-Plan.md), which is binding on B1's measurement and reporting. The dependency prerequisite is a per-package gate that applies as B1 proceeds.

## Boundaries

### In scope

- a Vulkan 1.2 candidate renderer path in the production tree;
- disposable instrumented craft-physics and resource-network scenarios;
- candidate algorithms or small libraries compared behind narrow boundaries;
- machine-readable results plus concise evidence reports;
- ADR and plan changes justified by measured results.

### Non-goals

- production gameplay, construction UI, campaign progression, polished terrain, final assets, or shippable content;
- production frame-rate gating, which belongs to M2;
- final physics-library, ECS, UI, audio, or terrain-implementation selection beyond what an increment directly proves;
- production support claims for integrated graphics merely because the prototype launches;
- a second graphics backend.

## Shared measurement rules

- Performance measurements use optimized Release builds with diagnostics that do not materially distort the result. Debug and validation-layer runs are reported separately.
- Every report records commit, preset/toolchain, OS build, CPU, GPU, driver, API/device capabilities, quality settings, resolution, scenario seed/input, warm-up, sample count, and raw-output location.
- Frame-time and subsystem-time measurements use at least 10,000 post-warm-up samples and report p50, p95, p99, and maximum. CPU and GPU times are reported separately where available.
- Numerical comparisons record units, reference frame, time scale, reference result, absolute error, and relative error where meaningful.
- Determinism claims are scoped by ADR 0010.
- A threshold may be changed only by a documented planning/ADR update approved by the user. Failed results must not be relabeled as passes.

## Accepted thresholds

| Area | Pass criterion | Gates P1b? |
|---|---|---|
| Render precision | Stationary surface and orbital reference views exhibit no more than 0.25 pixels of screen-space jitter, measured as defined below | Yes |
| Depth behavior | No depth collapse, z-fighting, or near/far discontinuity across the full surface-to-orbit path | Yes |
| LOD continuity | Terrain and atmosphere LOD transitions produce no popping detectable **as defined below** at the recorded quality setting, and no unbounded memory growth over a 30-minute traverse | Yes |
| Capability reporting | Startup enumerates loader, device version, features, extensions, formats, queues, limits, and memory; unsupported devices are rejected with actionable diagnostics | Yes |
| Validation output | Exercised validation-layer output is clean, or every remaining message is explained and accepted | Yes |
| Craft physics | At 300 parts, p95 60 Hz craft-physics work no greater than 4 ms on the accepted baseline CPU classes | Yes |
| Resource networks | At 300 parts, p95 fluid/electrical solve no greater than 1 ms in the representative network mutation scenario | Yes |
| Discrete frame time | Recorded at 1920×1080 low/medium on GTX 1060 6 GB and RX 580 8 GB classes | **No — recorded as data; gated at M2** |
| Integrated frame time | Recorded at 1280×720 low on UHD 630 and Vega 8 classes where accessible | **No — recorded as data; investigation tier** |

None of the four device classes in the last two rows is available. See the [Reference Hardware Evidence Plan](P1b-Reference-Hardware-Evidence-Plan.md) for what is measured instead, why the gating rows above still gate, and why the capability-reporting row needs a negative control to be testable at all.

Peak process memory, committed GPU memory, allocation counts, and upload volume are mandatory measurements. Production memory caps are deferred until representative assets exist.

### Screen-space jitter measurement method

The original plan asserted a 0.25-pixel gate without defining how to measure it. The accepted method is:

1. Place a small high-contrast reference marker at a fixed position in the authoritative world frame.
2. Hold the camera stationary in the same frame. Disable temporal antialiasing, jittered sampling, motion blur, and any other deliberate sub-pixel perturbation.
3. Render at least 600 consecutive frames.
4. Compute the marker's screen-space centroid per frame to sub-pixel accuracy.
5. Jitter is the maximum absolute deviation of the per-frame centroid from the mean centroid, reported per axis in pixels. Report both the maximum and the p99.

Run this at two reference views: the surface anchor, and a 200 km orbital vantage. Both must satisfy the gate.

### LOD continuity measurement method

**Approved by the user on 2026-08-14**, under the rule above that a threshold may be changed only by a documented planning update. Like the jitter gate before it, "popping that a fixed observer path can detect" was unmeasurable as written: it named no statistic, no threshold, and no way to tell a renderer that does not pop from an instrument that cannot see popping.

The accepted method measures **one transition at a time**, not a descent:

1. Locate an altitude at which the drawn patch count actually changes, by scanning the traverse rather than deriving a band from the subdivision arithmetic. If no transition is found, the gate reports that and fails; it does not sweep an arbitrary band.
2. Sweep ±6% of that altitude in 300 steps, capturing the presented frame at each.
3. Between consecutive frames, compute the fraction of pixels whose Rec. 601 luma changes by more than 16 of 255 — a change large enough to be visible on adjacent frames.
4. **Pass criterion:** that fraction must stay below **0.002** at every step — about 1 843 pixels of a 1280×720 frame, a visible amount of the screen changing visibly in one frame.
5. **Control:** the same sweep with morphing disabled must exceed the production path by at least **3×**. A pass measured against a control that does not respond certifies nothing.

Measured at the shipping 60° vertical field of view, at the recorded quality setting, on the stress scene. Field of view is part of the recorded setting because screen-space error is an angle: a narrower view silently makes the test stricter.

**Why the descent is not the instrument.** A local-outlier test over a continuous descent cannot work, and this was established by measurement rather than argued: at a 20° field of view, which magnifies screen-space error about threefold, every other statistic separated further while both configurations still recorded exactly zero pops. An instrument that is merely insensitive responds when the signal triples. A transition must be isolated to be detectable as an event, and on a descent with hundreds of patches on screen they overlap continuously.

**What a pass under this method does and does not mean.** If the control is itself below 0.002, the scene does not pop visibly with or without morphing, and the pass means the production path sits well under the limit and the metric responds strongly to disabling the morph — a margin and a response, not a demonstration that morphing rescues a visibly broken picture. The gate must print that qualification whenever it holds. Strengthening it requires a scene where the abrupt scheme pops visibly at a quality setting where the morph is well-conditioned; none has been found.

**The memory clause is unchanged.** The 30-minute traverse stands exactly as written above and has never been run. What the harness reports is device allocation flat over 600 steps, which is structural rather than the stated criterion, and it says so in its own output. Enabling the gate does not close that clause.

## Time boxes

Each increment carries a cost gate. Exceeding the box is itself a result: it triggers a narrow-or-reject decision with the user, not silent continuation.

| Increment | Box | Trigger on exceeding |
|---|---|---|
| B1 — Vulkan large-world renderer | 6 weeks | Cut atmosphere to a simple analytic shell and terrain to a single non-adaptive LOD scheme; the precision and depth gates still apply |
| B2 — Constructed craft and resource networks | 4 weeks | Reduce to the single leading craft representation and record the untested alternative as open |

## Increment B1 — Vulkan large-world renderer

### Deliverables

- a Vulkan 1.2 path that queries all required device capabilities and rejects unsupported configurations clearly;
- minimal full-scale Earth, atmosphere/sky transition, depth stress geometry, and camera-relative rendering built on the frame model selected in P1a increment A2;
- representative scalable quality settings, including a conservative low tier;
- validation-layer and graphics-capture workflow;
- the screen-space jitter harness described above;
- capability and performance reports for both discrete baseline GPU classes and both integrated investigation classes when accessible;
- an evidence-based Direct3D 12 comparison analysis sufficient to close ADR 0002.

### Done criteria and validation

- the render-precision, depth, LOD-continuity, capability-reporting, and validation-output gates above are satisfied;
- frame time is recorded with the full distribution on every accessible device class and compared against the M2 targets as forward-looking data;
- peak CPU/GPU memory and allocation/upload behavior are recorded;
- the frame model selected in A2 is confirmed to support the jitter gate, or A2's decision is explicitly reopened with evidence;
- Vulkan types do not appear in any interface outside the renderer module;
- ADR 0002 is accepted, rejected, or superseded from evidence before this increment closes.

### Direct3D 12 disposition

The original plan required a Direct3D 12 comparison spike. That requirement is withdrawn. A spike costs weeks and cannot realistically change the decision for a Windows-only single-player title, where the difference between the two APIs is tooling and driver ergonomics rather than achievable performance.

ADR 0002 closes instead on a documented analysis covering driver coverage on the baseline GPU classes, tooling maturity, shader toolchain, and the cost of a future backend swap behind the renderer interface. If B1 encounters a Vulkan driver or capability failure on a baseline device that has no workaround, that is the trigger to reconsider — and it is a real result, not a reason to have built the spike preemptively.

### Documentation trigger

Update ADR 0002, `docs/architecture.md`, hardware support claims, graphics non-goals, and any selected shader/allocator/window/Vulkan-loading dependencies.

## Increment B2 — Constructed craft and resource networks

### Deliverables

- representative 150-part and 300-part staged craft graphs with fixed inputs and seeds;
- **two candidate craft representations compared under the same scenarios** (see below);
- staging separation, deterministic failure events, and instrumentation for both;
- explicit logical propellant/fluid and electrical graphs with disconnection, valve/switch, depletion, and staging mutations;
- timing breakdowns that separate physics, collision/joint work, fluid/electrical solving, and harness overhead.

### Craft representation comparison

The original plan set a 4 ms budget without naming the representation under test, which left the most consequential architectural decision in the increment implicit. Both of the following must be measured:

1. **Per-part dynamic bodies with joints.** Every part is an independent rigid body connected by constraints. Maximum fidelity and emergent structural behavior; the known failure mode is joint softness under load and superlinear solver cost with part count.
2. **Welded aggregate with breakable constraint groups.** Connected parts merge into a single rigid body; staging, decoupling, and structural failure re-partition the aggregate at runtime. Far cheaper and rigid by construction; the cost is that structural flex is modelled rather than emergent, and re-partitioning must be correct and fast.

The comparison must report, for each: p95 physics time at 150 and 300 parts, structural behavior under a representative launch load, staging correctness, re-partition cost where applicable, and determinism.

### Done criteria and validation

- at least one representation meets the 4 ms p95 craft-physics and 1 ms p95 resource-network gates at 300 parts on the baseline CPU classes;
- one representation is recommended with measured justification, and the rejected one's evidence is retained;
- staging preserves or changes mass, momentum, resources, connectivity, and identity according to the scenario oracle;
- repeated runs preserve event ordering and remain bit-identical per ADR 0010;
- invalid networks are reported deterministically and do not silently create craft-wide resource pools;
- any physics or graph library recommendation completes ADR 0007 dependency review before architectural acceptance.

### Documentation trigger

Record the recommended craft representation and resource-network boundaries, measured part-count limits, and dependency decisions in `docs/architecture.md` and a new ADR. Do not turn the 300-part test into an unlimited support claim.

## Exit criteria

- every P1b-gating threshold has a reproducible result or an approved documented revision;
- ADR 0002 has a clear disposition;
- the craft representation has a recommendation backed by a two-way comparison;
- integrated-graphics status is honest and does not block closure;
- frame-time data is recorded and carried forward as the M2 baseline without being treated as a P1b pass or fail;
- one index links raw results, scenario definitions, hardware/toolchain metadata, accepted failures, and conclusions from P1a and P1b;
- a P2 recommendation identifies reusable contracts and explicitly disposable prototype code;
- `complete-milestone` review verifies evidence before project status marks P1b complete.

## Risks and recovery

- **Unavailable reference hardware:** do not substitute stronger hardware and extrapolate. Mark the device unavailable and obtain user-approved remote or borrowed evidence before making its support claim.
- **Renderer scope creep:** B1 is the increment most likely to expand without limit. The time box and its narrowing action exist specifically for it.
- **Instrumentation distortion:** compare instrumented and minimally instrumented runs; report overhead.
- **Production-tree contamination:** B1 is production code and must not accumulate prototype shortcuts behind a "this is just a prototype" justification. B2 is disposable and must not leak into production interfaces.
- **Threshold failure:** preserve the raw evidence, reject or narrow the candidate, and update ADR/status. Do not tune away simulation fidelity to disguise a graphics failure.
- **Dependency dead end:** keep the candidate behind a narrow boundary and retain a dependency-free fixture/scenario so another candidate can be evaluated.
- **Time-box overrun:** apply the narrowing action in the time-box table and record the untested alternatives as open.

Rejected prototype code may be removed only after its evidence and decision record are retained and the exact removal scope is reviewed. No cleanup may touch production or unrelated user work.

## Handoff contract

Full handoff records are required at **increment closure**, not on every commit. See the lightweight-lane rule in `AGENTS.md`.

Each increment closure records:

- goal and current outcome;
- single owner, branch, and base;
- changed files and dependency changes;
- literal configure/build/test/scenario commands and results;
- hardware, driver, toolchain, settings, samples, raw evidence, and summary metrics;
- failed or waived criteria — waivers require prior user-approved plan changes;
- ADR/documentation changes, remaining risks, disposable code, and the smallest next action.

## Decisions

| Decision | Status | Why |
|---|---|---|
| P1b gates on precision and correctness, not prototype frame rate | Confirmed | User approved 2026-08-12; prototype frame time without representative assets is weakly predictive |
| Discrete 16.67 ms gate moved to P2/M2 | Confirmed | User approved; M2 is where the scene is real |
| Direct3D 12 spike withdrawn in favor of documented analysis | Confirmed | User approved; a spike cannot realistically change a Windows-only decision |
| Renderer built in the production tree; craft physics disposable | Confirmed | User approved 2026-08-12; a working Vulkan bootstrap is never discarded in practice |
| Screen-space jitter measurement method defined above | Confirmed | The original gate was unmeasurable as written |
| LOD continuity measurement method defined above; verdict measured on an isolated transition rather than a descent | Confirmed | User ratified 2026-08-14. The original gate was unmeasurable as written, and the descent instrument was shown by measurement to be blind rather than insensitive |
| A LOD pass whose control is also below the perceptual limit is accepted as a margin and a response, not as a rescue | Confirmed | User ratified 2026-08-14; no scene has been found where the abrupt scheme pops visibly at a well-conditioned quality setting, and the gate prints the qualification |
| 30-minute LOD memory traverse | Open | Unchanged by the 2026-08-14 ratification and never run; what is reported is structural flatness over 600 steps |
| Two-way craft representation comparison required | Confirmed | User approved 2026-08-12; the representation choice outweighs the physics-library choice |
| 0.25-pixel jitter gate | Confirmed | User approved recommendation |
| 300-part 4 ms physics and 1 ms resource-network gates | Confirmed | User approved recommendation |
| Integrated tier remains an investigation target through M2 | Confirmed | User approved 2026-08-12 |
| Per-increment time boxes with narrow-or-reject triggers | Confirmed | User approved 2026-08-12 |
| Increment B1 authorized; Claude sole writer on `feature/p1b-vulkan-renderer` from `dev` | Confirmed | User authorized 2026-08-13, scoped to B1 only |
| Baseline GPU hardware replaced by a documented evidence plan | Confirmed | User accepted 2026-08-13; no named device class is available. See [the plan](P1b-Reference-Hardware-Evidence-Plan.md) |
| Increment B2 authorization | Open | Requires its own explicit user instruction after B1's evidence |
| Vulkan 1.2 candidate floor with queried optional capabilities | Confirmed for P1 | ADR 0002 |
| Exact production libraries | Open by owning increment | Must follow evidence and ADR 0007 |
