# Project Status

**Phase:** P1b — Renderer and craft prototypes: **authorized and in progress**. Both increments are now authorized — **B1 is the active increment**, and **B2 was authorized on 2026-08-14** to start after B1 closes. P1a is **complete**, reviewed and closed on 2026-08-12 and integrated into `dev` on 2026-08-13.

**Planning gate:** Approved on 2026-08-12

**Implementation authorization:** **Granted for increment B1 on 2026-08-13**, and **for increment B2 on 2026-08-14**, each by separate explicit user direction, against the [P1b milestone plan](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md). B1 is the Vulkan large-world renderer; B2 is constructed craft and resource networks. **B1 remains the active increment** — the user directed at B2's authorization that B1 be finished first, so B2 is authorized and not started. The earlier P1a grant is spent and closed.

**Single writer:** Claude — on branch **`feature/p1b-vulkan-renderer`** from base `dev` for B1, and on **`feature/p1b-craft-and-resources`** from base `dev` for B2. The B2 branch is authorized as of 2026-08-14 and **has not been created**, because B1 is finished first. Prior P1a history: A1 landed as PR #1 from `feature/p1a-precision-and-orbit`; A2 landed as PR #2 from the `dev` working tree; A3 and the milestone-review corrections landed as PR #3 from `feature/p1a-hybrid-orbit-and-warp`, merged into `dev` on 2026-08-13 as `bf18c33`.

**Implementation:** P1a complete — increments A1 (measurement and build harness), A2 (reference frames and numerical precision), and A3 (hybrid orbit and time warp), all closed against their accepted thresholds with evidence indexed at [`evidence/p1a/Index.md`](../evidence/p1a/Index.md). **P1b increment B1 is in progress**; it is the first code written **in the production tree** rather than under `prototypes/`. Its state is recorded in the [B1 progress and handoff](#p1b-increment-b1-progress-and-handoff) section below. **Increment B2 is authorized as of 2026-08-14 but not started**; it returns to disposable prototype code by plan, and its authorization terms are in the [B2 authorization section](#p1b-increment-b2-implementation-authorization-2026-08-14).

This is the single source of truth for project phase, milestone state, blockers, planning-gate state, and implementation authorization. Design intent belongs in `SolProjectNotes/`; implemented technical truth will belong in `docs/architecture.md`.

## Current state

- Product priorities and long-term direction have been captured from the initial design interview.
- SolEngine, **Frontiers of Sol**, the `sol` C++ namespace, the C++ naming conventions in ADR 0003, and the subsystem/API documentation policy in ADR 0006 are confirmed.
- Windows x64, single-player, real-scale seamless surface-to-space travel, modular part construction, and a hybrid simulation model are confirmed.
- The initial campaign epoch is fixed at 2026-01-01 00:00:00 UTC in the real Solar System, using real astronomical names/data and fictional companies and politics. DE440/DE441 and NAIF data and an explicit UTC-to-TDB boundary own reference fixtures. P1 uses a fixed launch anchor at 28.0° N, 80.5° W, 5 m above the reference ellipsoid; final fictional terrain placement and regulatory context remain open (ADR 0008).
- The first playable uses external third-person flight, instruments, and an orbital map. Cockpit/IVA is later; walking inside ships is deferred beyond the current roadmap.
- C++23, MSVC, CMake, and Ninja are accepted. **Vulkan is the accepted SolEngine graphics API — ADR 0002 was accepted on 2026-08-14** on P1b increment B1 evidence, with Vulkan 1.2 as the candidate floor and per-device capability queries. Acceptance rests on precision, capability, tooling and the documented Direct3D 12 analysis, measured on one device; it asserts nothing about baseline-class or AMD driver behaviour and does not close the P2/M2 frame-time gates.
- The baseline PC is an Intel Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, and an SSD, targeting 60 FPS at 1080p on low/medium settings. Intel UHD 630 and AMD Vega 8-class integrated graphics are investigation targets for 30 FPS at 720p/low.
- Keyboard/mouse is the first input target. The remappable defaults use W/S for pitch, A/D for yaw, Q/E for roll, Shift/Ctrl for throttle increase/decrease, Z/X for full/cut throttle, Space for staging, T for stability assist, M for the orbital map, right-mouse drag to orbit the external camera, and the wheel to zoom. Initial capability also includes attitude-rate damping, throttle hold, heading/prograde/retrograde indicators, maneuver guidance, and staging warnings. Automated launch, maneuver execution, rendezvous/docking, and mission scripting are research-gated.
- The first contract, **Orbital Environmental Survey**, uses an uncrewed craft to reach an approximately 200 km by 200 km orbit, remain in a stable orbit for one complete revolution, collect radiation, magnetic-field, and upper-atmosphere observations, and transmit valid data. Reentry and recovery are not required.
- SolEngine begins as internal static libraries with no promised C++ binary ABI. Future native extension must use a stable C interface or scripting/data boundary rather than engine internals.
- The company initially owns a small assembly hangar, mission-control room, one launch pad, and limited test equipment, while leasing major manufacturing and tracking services.
- Persistent formats are versioned from first use. Internal pre-alpha saves may be disposable; version 1.0 migrates all supported public-alpha saves/blueprints, and later releases support their current major series plus the final schema of the previous major. Artifact families are accepted in ADR 0009.
- vcpkg manifest mode with a reviewed pinned baseline is the accepted dependency acquisition policy (ADR 0007). **P1b increment B1 added the project's first dependencies on 2026-08-13:** `vulkan-headers`, `volk`, `vulkan-memory-allocator`, and `glfw3`, against baseline `2273a28f`, plus the Vulkan SDK 1.4.357.0 as a pinned toolchain input. Reviewed in [dependencies](dependencies.md).
- Determinism is bit-exact on the same build and machine and tolerance-based across machines, using `/fp:precise` and `/arch:AVX2` (ADR 0010). `/arch:AVX2` places a hard CPU floor at Haswell-era Intel and Zen-era AMD.
- The authoritative orbital model is patched conics with spheres of influence, with no perturbations, drag, or orbital decay (ADR 0011). Aerodynamic forces still apply to active craft inside the atmosphere. Lagrange points and perturbation-driven mission design are unavailable while this ADR stands.
- Assets are authored in Blender, interchanged as glTF 2.0, generated procedurally where parametric, and baked at build time; binary source assets use Git LFS (ADR 0012). Procedural geometry generation is an M3 engine deliverable.
- The technical-risk prototype scope is split across the [P1a milestone plan](../SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md) (headless: harness, frames/precision, hybrid orbit and warp) and the [P1b milestone plan](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) (renderer and constructed craft). Renderer frame-time gating is deferred to M2; versioned persistence round trips moved to M1.
- The working time budget is 40+ hours per week.
- FactoryProject has been consulted for workflow patterns and remains reference-only.
- Agent contracts and the initial planning-document set are established.
- P1a increment A1 built the first CMake project and the disposable headless prototype targets under `prototypes/p1a/`. P1a is deliberately dependency-free and remains so; `vcpkg.json` was introduced by P1b increment B1, not by P1a, and nothing under `prototypes/p1a/` consumes it. No runtime assets exist.
- **The named baseline GPU classes are unavailable.** The only machine is an i7-12650H laptop with an RTX 4060 Laptop GPU and Alder Lake-P Intel UHD Graphics; no GTX 1060, RX 580, UHD 630, or Vega 8 — and no AMD device or driver stack of any kind — is present. The user accepted a documented [reference-hardware evidence plan](../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) on 2026-08-13 in place of the hardware. **Baseline-tier support claims are therefore unverified on hardware** until borrowed or remote evidence exists, and no RTX 4060 measurement may be reported as a baseline-class result.
- P1a increment A2 selected the **hierarchical parent-relative frame graph** over a single global root, on measured evidence recorded in [`docs/architecture.md`](architecture.md) and [`evidence/p1a/A2/Index.md`](../evidence/p1a/A2/Index.md). Both candidates met every accepted threshold; the selection turned on a ~4,500× precision gap for conversions below barycentric magnitude.
- The pinned ADR 0008 reference fixtures now exist under `fixtures/p1a/` with SHA-256 digests verified at load and full [provenance](../fixtures/p1a/Provenance.md). The UTC/TAI/TT/TDB boundary is driven entirely by the pinned leap-second kernel and agrees with JPL Horizons to the fixtures' printed 0.1 ms.
- One ULP of a `double` is 0.98 mm at Neptune's distance. A global-root double therefore has no millimetre headroom beyond roughly Jupiter, which is the measured constraint on how far the roadmap can extend before a frame decision would have to be revisited. The selected model is not subject to it.
- P1a increment A3 selected the **hybrid transition contract** — one authoritative regime at a time, an analytical coast anchored on the state it is handed, sphere-of-influence crossings as discrete refined events, and eligibility rejected by named reason — recorded in [`docs/architecture.md`](architecture.md) with evidence in [`evidence/p1a/A3/Index.md`](../evidence/p1a/A3/Index.md). The local-to-analytical handoff is exactly lossless, and an anchored coast is bit-identical at every warp factor.
- **ADR 0011 is confirmed from measurement and was not amended.** Every validation item it named is met, including no secular altitude change over 100 days of warped coast. The atmosphere limit it assigned to A3 is recorded at 140 km, chosen as a physics-regime boundary rather than derived from a tolerance.
- **RK4 is selected for the local numerical regime.** The symplectic candidates keep their energy error bounded where RK4's accumulates, but the hybrid contract never integrates a stable orbit for long, and RK4 clears the accepted one-orbit gate at three times less cost.
- Time warp is safe under a **quantisation** rule rather than a prohibition: warp ticks must be integer multiples of the fixed local physics step, and the analytical coast must be anchored rather than stepped. Warp under thrust is not a determinism problem, which is what the open-questions register previously assumed.
- The **campaign clock is exact to 104.25 days** and no further: the nanosecond count accumulates exactly without bound, but its conversion to seconds resolves individual nanoseconds only to 2^53 ns. Every P1a measurement is inside that window and determinism is unaffected either side of it, but a multi-year campaign will need a coarser tick, a split representation, or anchor-relative elapsed times. This is a P2 design input rather than an open question.
- The P1a milestone review corrected three claims that measurement did not support: velocity Verlet's relative cost is **16×, not 32×** (the prototype's stateless integrator API cannot reuse an acceleration a real loop would, and the implementation's count had been quoted as the method's); the Sun and Moon had been carrying **zero surface radii**, which silently disabled their below-surface eligibility check, on the mistaken belief the pinned kernel did not supply them; and ADR 0011 described sphere-of-influence radii as fixtures when they are derived at load. RK4's selection and every threshold verdict are unaffected.

## Planning gate

**Approved by the user on 2026-08-12.** This approval closed P0 and accepted the planning foundation only.

**Implementation authorization followed on 2026-08-12**, separately and explicitly, scoped to P1a. The user granted authorization, named Claude as the single writer, and authorized creating `feature/p1a-precision-and-orbit` from `dev`. Authorization does not extend to committing, pushing, merging, tagging, or opening a pull request; each still requires explicit user direction per `AGENTS.md`.

Scope granted covered **P1a increments A1, A2, and A3**. The user instructed implementation of A2, and later of A3, directly on 2026-08-12; each instruction is the authorization for its increment. The A2 instruction superseded the earlier note that A2 would be scoped only after A1's evidence was reviewed, and the A3 instruction superseded the note that A3 was unauthorized.

For A3 the user additionally and explicitly requested the branch `feature/p1a-hybrid-orbit-and-warp` from `dev`. On 2026-08-12 the user instructed the `complete-milestone` review, then directed that its findings be fixed and P1a marked complete; that instruction authorized the review's corrections, which touched the A3 prototype code, the frame library's reference-data loader, ADR 0011's radius clause, and the documentation set.

**That P1a scope is now spent.** It never extended to committing, pushing, merging, tagging, or opening a pull request; each remains a separate explicit request under `AGENTS.md`.

### P1b implementation authorization, 2026-08-13

**Granted by the user on 2026-08-13.** The user authorized P1b and named Claude the single writer. In the same instruction the user set three terms:

- **Scope: increment B1 only** — the Vulkan large-world renderer, under its 6-week time box and its narrow-or-reject trigger. **Increment B2 (constructed craft and resource networks) was not authorized** by this grant, matching how P1a was run one increment at a time. *(B2 was authorized separately on 2026-08-14; this bullet records the 2026-08-13 grant's terms and is not the current state.)*
- **Branch: `feature/p1b-vulkan-renderer` from base `dev`.**
- **Reference hardware:** the user accepted a documented evidence plan rather than acquiring baseline hardware or amending the baseline classes. That plan is [P1b — Reference Hardware Evidence Plan](../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) and it is binding on B1's reporting.

This satisfies every authorization prerequisite in the P1b plan: P1a is complete with its frame model selected and recorded; the gate is Approved and authorization is Granted; the writer, branch, and base are recorded above; and the hardware prerequisite is discharged through the accepted evidence plan. The remaining prerequisite — that each third-party package complete ADR 0007's dependency workflow before its declaration is added — is a per-package gate that applies as B1 proceeds. B1 is the first increment to need dependencies at all; `vcpkg.json` and the four reviewed packages were added under that workflow on 2026-08-13 and are recorded in [dependencies](dependencies.md).

**This authorization did not extend to** increment B2, to committing, pushing, merging, tagging, or opening a pull request, or to any change beyond B1's declared deliverables. Each remained a separate explicit request under `AGENTS.md`. B2 received its own grant a day later, recorded immediately below; the commit/push/merge/tag/PR exclusion is unchanged by it.

### P1b increment B2 implementation authorization, 2026-08-14

**Granted by the user on 2026-08-14.** The instruction was to authorize increment B2 — constructed craft and resource networks. This is the separate explicit instruction the B1 grant, the P1b plan, and the reference-hardware evidence plan all said B2 would require. Three terms were set in the same exchange, in answer to questions the plans deliberately left open until this moment:

- **Branch: `feature/p1b-craft-and-resources` from base `dev`.** A new branch rather than a continuation of `feature/p1b-vulkan-renderer`, because B2 is disposable prototype code by plan and B1's branch carries production-tree renderer code. B2 does not build on B1: it is headless craft physics. The branch is authorized and **not yet created**, since B1 is finished first.
- **Sequencing: B1 stays open and is finished first.** Authorizing B2 does not close B1, waive any B1 gate, or start B2 work. B1's outstanding items at the time of this instruction — the unmeasured Intel UHD across every gate, the capability-reporting gate that could not close until the synthetic profiles were reconciled, quality tiers, and both-device performance evidence — are worked to closure before B2 begins. This keeps one active increment at a time, as P1a ran. *(Later the same day the first two were resolved: both devices are measured and the capability gate is closed. Quality tiers and per-device performance evidence remain.)*
- **CPU evidence: the reference-hardware evidence plan is extended with a CPU section.** See below.

**Scope granted** is increment B2 as the [P1b milestone plan](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) defines it, including its 4-week time box and its narrow-or-reject trigger, and specifically including the **two-way craft representation comparison** — per-part dynamic bodies against welded aggregates with breakable groups — which is the increment's most consequential deliverable and not an optional half.

**This authorization does not extend to** committing, pushing, merging, tagging, or opening a pull request, to creating the B2 branch before B1 closes, or to any change beyond B2's declared deliverables. Each remains a separate explicit request under `AGENTS.md`. A physics or graph library recommendation still completes ADR 0007's dependency workflow before architectural acceptance, exactly as B1's four packages did.

#### B2's CPU-class evidence approach

B2's two gating thresholds — 4 ms p95 craft physics and 1 ms p95 resource-network solve, both at 300 parts — are specified against the accepted baseline CPU classes, i5-8400 and Ryzen 5 2600. Neither is present; the available i7-12650H is materially faster than both, and the P1b plan's no-substitution rule applies on the CPU side exactly as it does on the GPU side. Both the P1b plan and the [reference-hardware evidence plan](../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) recorded this as **Open**, to be settled when B2 was authorized rather than pre-decided.

**The user settled it on 2026-08-14** by extending the reference-hardware evidence plan with a CPU section, mirroring how the GPU side was handled: measure on the i7-12650H under its own name, never as a baseline-class result, and make the gate testable by a **constrained-CPU control** — a pinned core count and capped frequency — as the analogue of the synthetic capability profiles. The full method is owned by [that plan's CPU section](../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md#cpu-evidence-for-increment-b2) and is binding on B2's reporting.

Stated with the decision rather than left to be inferred: **a constrained run is not an i5-8400.** It bounds the budget from a direction the unconstrained machine cannot, and it is reported as a bound, not a proxy. Amending the baseline CPU classes was **not** chosen, consistent with the user declining the equivalent move on the GPU side on 2026-08-13.

### Required before approval

- [x] Product vision, player-priority order, platform, and mode recorded.
- [x] Initial engine architecture and risk model drafted.
- [x] Initial GDD and first-playable loop drafted.
- [x] Engine/game roadmap drafted with playable acceptance criteria.
- [x] AI-agent ownership, review, and documentation workflows established.
- [x] Game title selected; public trademark/storefront clearance remains a release task.
- [x] C++ namespace selected.
- [x] C++ naming conventions selected through ADR 0003.
- [x] Engine link, binary ABI, and export policy selected through ADR 0005.
- [x] Nested subsystem namespace and public source-API documentation details selected through ADR 0006.
- [x] Default realism model and difficulty-assist boundaries selected at the product level.
- [x] Initial flight presentation scope selected.
- [x] Initial input device and assistance/progression model selected.
- [x] Initial guidance feature set selected.
- [x] Exact initial flight bindings and external camera behavior selected.
- [x] Initial world identity, scale, and present-day start selected.
- [x] C++ standard, compiler, and build generator accepted through ADR 0001.
- [x] Vulkan 1.2 P1 candidate floor, capability strategy, hardware tiers, and conditional fallback accepted; **production Vulkan adoption accepted in ADR 0002 on 2026-08-14** on P1b B1 evidence, on one device and not on baseline-class or AMD driver behaviour.
- [x] Initial dependency acquisition and pinning policy accepted through ADR 0007; individual libraries remain milestone-owned.
- [x] First-playable scope and non-goals accepted.
- [x] Technical risk prototypes and measurable pass/fail criteria accepted in the P1a and P1b milestone plans.
- [x] Astronomical reference source, UTC/TDB boundary, and reproducible P1 launch anchor accepted through ADR 0008.
- [x] Save/versioning compatibility baseline and artifact/migration policy accepted through ADRs 0004 and 0009.
- [x] Determinism guarantee level and floating-point policy accepted through ADR 0010.
- [x] Gravity and orbit baseline accepted through ADR 0011.
- [x] Asset authoring and pipeline accepted through ADR 0012.

### Post-approval plan revision, 2026-08-12

The planning set was reviewed after gate approval and revised with user authorization. The gate remains **Approved**; implementation authorization remains **Not granted**. Changes:

- P1 split into P1a (headless) and P1b (renderer and craft), with per-increment time boxes.
- Renderer frame-time gating moved from P1b to M2; ADR 0002 rewritten to close on precision and capability evidence.
- Direct3D 12 comparison spike withdrawn in favor of a documented analysis.
- Versioned persistence round trips moved from P1 to P2/M1.
- Screen-space jitter gate given a defined measurement method and moved to P1b, since it cannot be measured without a renderer.
- Craft representation comparison — per-part dynamic bodies versus welded aggregates with breakable groups — made an explicit P1b deliverable.
- ADRs 0010, 0011, and 0012 added, closing the determinism, gravity-baseline, and asset-pipeline gaps.
- Lightweight-lane rule added to `AGENTS.md` so full handoff records attach to increment closure rather than every commit.
- Planning corpus committed to Git and the `dev` integration branch created.

## Roadmap state

| Stage | State | Outcome |
|---|---|---|
| P0 — Product and architecture planning | Complete | Planning foundation reviewed, gate approved, and plan revised 2026-08-12; no implementation delivered |
| P1a — Precision and orbit prototypes | **Complete** — reviewed and closed 2026-08-12; **integrated into `dev`** 2026-08-13 via PR #3; not released | Headless evidence for frame conversion, precision budget, and hybrid propagation under warp |
| P1b — Renderer and craft prototypes | **In progress** — B1 authorized 2026-08-13 and active; **B2 authorized 2026-08-14**, starting after B1 closes | Vulkan surface-to-orbit precision evidence and constructed-craft feasibility |
| P2 — First-playable production | Not started | Design-build-fly-explore-research-company loop |
| P3 — Orbital company | Not started | Persistent operations, stations, people, maintenance, logistics, and manufacturing |
| P4 — Solar economy | Not started | Mining, markets, corporations, nations, and strategic command |
| P5 — Settlement era | Not started | Habitats, off-world colonies, independence, and deeper politics |
| P6 — Contested system | Not started | Piracy, security, fleets, and optional combat |
| P7 — Far future | Not started | Fusion, antimatter, outer-system scale, and later speculative technologies |

## Current blockers and open decisions

P0 has no remaining planning blockers, and P1a's authorization prerequisites are satisfied: implementation is authorized, Claude is the named single writer, and the branch/base are recorded above.

**P1a is closed.** All three increments are complete and the `complete-milestone` review the plan requires ran on 2026-08-12. The milestone record, including the review itself, is [`evidence/p1a/Index.md`](../evidence/p1a/Index.md); per-increment detail is at [A1](../evidence/p1a/A1/Index.md), [A2](../evidence/p1a/A2/Index.md), and [A3](../evidence/p1a/A3/Index.md).

The review re-ran both configurations from the checked-in presets — **20/20 tests passing in each** — and re-derived the reported measurements from fresh output rather than reading them from the evidence documents. Three of the four A3 scenarios reproduced byte-identically; the fourth differed only in its timing fields. It raised eight findings, **all resolved before closure and none invalidating a threshold result or reversing a selection**. Three changed code, and A2's and A3's raw evidence was regenerated; no physical number moved in either increment.

**P1b increment B1 is authorized and has no blockers.** Its prerequisites are recorded in the [P1b authorization section](#p1b-implementation-authorization-2026-08-13) above. Product and architecture questions belonging to later milestones remain tracked in [Open Questions](../SolProjectNotes/Open-Questions.md); they do not authorize implementation-time guessing.

**P1b increment B2 is authorized as of 2026-08-14 and has no blockers, but is deliberately not started.** The user directed that B1 be finished first, so B2's state is *authorized, queued* rather than *in progress*. Its one open prerequisite at authorization — the CPU-class substitution question — was settled in the same instruction and is recorded in the [B2 authorization section](#p1b-increment-b2-implementation-authorization-2026-08-14). B2's authorization does **not** relax any B1 gate; nothing about B1's outstanding work changes because a later increment was authorized.

**B1 carries two known limitations from the start, neither of which blocks it:**

- **No baseline-class or AMD hardware evidence is obtainable.** The accepted [evidence plan](../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) keeps every gating threshold in force — jitter, depth, LOD continuity, and validation output are properties of the implementation, not of GPU throughput — but the capability-reporting gate needs a negative control and synthetic baseline profiles to be testable at all, since both present GPUs exceed the Vulkan 1.2 floor. ADR 0002 **was accepted on 2026-08-14** on precisely that basis — precision, capability, tooling, and the documented Direct3D 12 analysis — and it asserts no clause about AMD driver behavior.
- **The frame-time rows are laptop measurements.** Dynamic Boost, variable TGP, thermal limits, and hybrid adapter selection mean the RTX 4060 result is not a fixed-hardware quantity. Frame time was already non-gating in P1b by user decision, so this affects the quality of the M2 baseline rather than any P1b verdict.

## P1b increment B1 progress and handoff

Recorded 2026-08-13, mid-increment. **B1 is not complete and this is not a stopping point** —
it is a handoff record written so the work survives a fresh session, per the ownership-transfer
rule in `AGENTS.md`. The lightweight lane puts full evidence records at increment closure; this
is the smaller thing: enough state that nobody has to re-derive it from commit messages.

**Owner:** Claude. **Branch:** `feature/p1b-vulkan-renderer`, 7 commits ahead of `dev`.
**Validation at time of writing:** 29/29 tests pass in both configurations, none disabled.

### Gating thresholds

| Threshold | State |
|---|---|
| Screen-space jitter, 0.25 px | **Passes on both available devices**, as of 2026-08-14. 0.000000 px at both required views on each, frames bit-identical, with a sub-pixel response control confirming precision rather than only stability. Does **not** confirm A2's frame model — see [architecture](architecture.md). |
| Depth behaviour | **Passes on both available devices**, as of 2026-08-14. No collapse, linearly-scaling resolution from 1 m to 10 000 km, matching the analytic prediction to 1e-7, with a conventional-projection control failing as expected on each. Guaranteed separation is **9.4 cm at 1 000 km and 15.0 cm at Earth's radius** on the RTX 4060. *(These two figures previously read "~7 cm" and "~69 cm" here; both were wrong. Debug, the original Release run, and a fresh 2026-08-14 Release run all print 0.0937507 m and 0.150005 m, which is also what [B1's evidence index](../evidence/p1b/B1/Index.md) has always recorded. The error was in this summary only.)* |
| LOD continuity | **Both halves pass on both available devices as of 2026-08-14.** The gated popping statistics are identical to three significant figures across the two devices — production 0.000101, control 0.000447, 4.4× — and the transition scan locates the same 140 866 m altitude on each, because patch selection is CPU-side and the pixel metric's threshold is coarser than the rasterisation difference. `render.lod-gate` is enabled and passing on both devices in both configurations. Popping is certified against an isolated transition by the [method now written into the milestone plan](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md): the production path moves 0.000101 of the frame at its worst step against a 0.0020 perceptual limit — a 20× margin — with the control separating by 4.4×. The user ratified both the method and its narrower reading: because the control is itself below the limit, the pass is a margin and a response rather than a rescue. The **30-minute memory traverse passes on both devices** under a method ratified the same day and separately from the popping one — RTX 4060 second-half trend 15.9 KiB/min and 0.98 MiB growth, Intel UHD 0.4 KiB/min and 0.00 MiB, against a 64 KiB/min limit and a 2 MiB backstop. Both negative controls were re-run on the Intel rather than inherited — 8 bytes/frame fails at 531.2 KiB/min and 10.09 MiB there against 271.7 and 6.47 on the RTX 4060 — so neither device's pass rests on the other's instrument. That the same control's sensitivity moves 2× between two devices running the same binary is itself the finding: frame rate is a term in it. A pass bounds the settled growth rate; it does not prove an asymptote. See [architecture](architecture.md). |
| Capability reporting | **Closed 2026-08-14 by user decision**, on: enumeration implemented and exercised on both available devices; a negative control proving the rejection path fires; and reconciliation against real-derived profiles. That last is a second profile family from the pinned SDK's `VP_LUNARG_desktop_baseline.json` — four *intersections of real gpuinfo.org report collections*, at a recorded SHA-256, spanning Vulkan 1.1 to 1.4. An intersection is conservative, so a requirement it satisfies is satisfied by every device in its collection, and the 2022 profile is **required to be rejected**, giving the check both signs from real-derived data. `render.capability-check` goes 25 → 35 checks. **Residual, recorded and not waived:** the four *named* device classes are not individually verified — LunarG does not enumerate its collections, and vulkan.gpuinfo.org returns HTTP 403 to this environment on every path including its documented API. A profile check remains not a driver test. |
| Validation output | Clean from this project **on both available devices**, verified 2026-08-14. Three `LLP_LAYER_3` loader warnings come from a third-party overlay layer (`GalaxyOverlayVkLayer`) installed on the machine, which falls in ADR 0002's "explained and accepted" category; they are loader messages and appear identically on both devices. A capture workflow **is established** as of 2026-08-14, using GFXReconstruct from the pinned Vulkan SDK rather than RenderDoc, verified end to end by a 240-frame capture, inspection and clean replay. |

### Where the LOD investigation stands

**Resolved.** The two pops were caused by the morph factor being computed **per patch rather
than per vertex**. CDLOD requires each vertex to derive its factor from its own distance, or
adjacent patches disagree along shared edges and a near-side child is short of full morph when
its parent hands over. Fixed in the vertex shader using `length(inPosition)`.

The inference previously recorded here — "the pops occur at the same steps in both runs,
morphing is the only difference, therefore morphing is not the cause" — was **invalid**. An
incomplete morph leaves a residual at the same step in both runs, large without morphing and
small with it, which is exactly what 0.2822 against 0.0030 showed.

**Closed on 2026-08-14. The gate is enabled and passing, and the diagnosis it was blocked on was
wrong.** The recorded explanation was that fixing the morph forced `subdivisionFactor` above ~2.8
and that at that factor transitions are sub-pixel, so nothing could pop and the control could not
fire — visibility and validity pulling against each other through one parameter.

That is not what was happening. The zero pop counts were a property of the *instrument*, not of
the scene. A local-outlier test needs a transition to be an isolated event, and on a descent with
hundreds of patches on screen they are not: patches cross their boundaries continuously, so an
abrupt scheme raises the whole baseline instead of spiking and there is nothing to flag. The
decisive measurement was re-running at a 20° field of view, which magnifies screen-space error
threefold: every other statistic separated further — the sweep's concentration went from 4.4× to
12×, its mean from 3.2× to 11.8× — and **both configurations still recorded exactly zero pops**.
An instrument that is merely insensitive responds when the signal is tripled. This one did not,
because the quantity it measures is not present on a descent.

The verdict therefore moved to the isolated-transition sweep, which was built for exactly this and
had been unusable for an unrelated reason: it located its band by the largest patch-count change
and kept landing on horizon patches that are tiny on screen. **Frustum culling fixed that as a
side effect** — off-screen patches are no longer drawn, so they can no longer attract the scan,
and the band it selects is now a transition that is genuinely in view. The two pieces of work were
not planned together; the second happened to unblock the first.

**What is certified is narrower than the threshold's plain reading, and the gate prints the
qualification itself.** The control is below the perceptual limit too, so this scene does not pop
visibly with or without morphing. The pass therefore means the production path sits 20× under the
limit and the metric responds 4.4× to switching the morph off — a margin and a response, not a
rescue.

**Ratified by the user on 2026-08-14.** Both the measurement method and the narrower reading of
what a pass means are now accepted, under the P1b rule that a threshold may be changed only by a
documented planning update the user approves. The method is written into the [P1b milestone
plan](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) beside the screen-space jitter
method it parallels, with three entries in that plan's decisions table. **The ratification does
not extend to the 30-minute memory clause**, which stands exactly as originally written. That
traverse has since been run — 2026-08-14, under `render.memory-traverse` — but the clause names
no statistic and no limit, so what exists is a measurement and not a verdict. See [Outstanding
for B1](#outstanding-for-b1).

Two earlier candidates were ruled out by measurement and should not be retried: the horizon cull
(genuinely broken, dimensionally wrong, now fixed — but the pops were unchanged), and morphed
per-vertex normals (measured strictly worse; a child's coarse-normal stencil clamps at its patch
boundary, so it needs a one-vertex skirt to do properly).

### Review findings applied, 2026-08-13

Three canonical reviewers ran against PR #6. Corrections made to **published claims**:

- The jitter **control was defective** — it stepped an axis whose magnitude cancels exactly, so
  a fully `float` pipeline passed it to within 4e-9 px. Now steps the large axis. The precision
  claim rested on nothing until this was fixed.
- "**A2's frame model is confirmed**" was overstated by two steps and is withdrawn. `WorldVec3`
  is a bare `double` triple; nothing walks a frame graph or checks an epoch.
- **Depth figures were best-case draws**, not guaranteed separations, and the 4× spread
  rationale was wrong (binades give 1.6×).
- The **memory pass is withdrawn** — the 30-minute criterion was never run, and the figure was
  arithmetically identical to the sum of fixed allocations, so it could not vary.
- "**The GPU never receives a world coordinate**" is false for the reference-object path.

Defects fixed: a terrain-buffer **data race** (two frames in flight, one buffer, fence waiting
on the wrong frame — invisible because every pixel harness accidentally serialises), a **fence
wedge** on reachable error paths, a missing **flush**, a depth **write-after-write** hazard, and
terrain capacities that left ~55 MB unreachable (now 43.03 MiB total, down from 125.22).

### Second review round, 2026-08-13

A `review-cpp-change` pass over the whole open PR #6 diff — including `59e29dc`, which the three
earlier reviewers never saw, since it *was* their output. **Ten findings, all ten applied.**

**Fixed.** Two of the three renderer fixes are covered by a new test, `render.renderer-contract`,
each verified against the defect by reverting the fix and confirming the test fails. The third is
harness code in a disabled gate and is not covered; it is named below rather than glossed:

- **Root terrain patches were pinned to full coarse morph.** A zero-width morph band — the CPU's
  way of saying "level 0 has no parent" — was widened to a `1e-6` epsilon before dividing, so the
  factor pinned to 1. Detail in [architecture](architecture.md). Unreachable by any harness;
  reachable in the renderer from geostationary altitude or beyond.
- **A minimised window returned the previous frame's pixels from the capture path**, as a
  well-formed non-empty frame. `FrameStats::presented` now distinguishes a skipped frame, and the
  capture path discards on it. Same defect class as the dropped-frame fix in `59e29dc`: that one
  closed the consumer side, this one the producer side.
- **The LOD gate's transition scan trusted skipped frames and a scan that found nothing.** A
  skipped frame reports zero patches, which against a real previous count is the largest delta the
  scan can see, so the sweep would centre wherever the window was minimised; and a scan finding no
  transition returned altitude zero, putting the sweep camera underneath the terrain and printing
  the result as data. The scan now skips unpresented frames, drops the stale previous sample, and
  returns no value rather than zero.
- **The public terrain quality default was 2.5, below the scheme's own ~2.8 validity floor**, while
  the gate measured at 3.0 and a comment claimed 3.0 *was* the default. The default is now 3.0 and
  a `static_assert` in the gate holds the two together.
- **The swapchain surface format was preferred, not required.** The search preferred sRGB but
  fell back to `formats.front()`, so a surface offering no sRGB format was accepted anyway —
  while the code beside it stated that a UNORM surface would make every pixel gate measure
  through a tone response nothing models, and the capture path assumed 4 bytes per pixel.
  Presenting now requires `B8G8R8A8_SRGB` or `R8G8B8A8_SRGB` and fails device creation otherwise,
  naming both the requirement and what the surface offered. **This was closer than it read:** on
  the RTX 4060 `formats.front()` is `B8G8R8A8_UNORM`, so the fallback was the gamma-doubling
  format, and the surface also offers `A2B10G10R10_UNORM_PACK32`, which would have satisfied the
  4-byte stride while breaking the byte triples. The old code was correct only because the sRGB
  format happened to be offered.
- **The per-frame device-allocation query was the expensive one.** `vmaCalculateStatistics` walks
  every block and allocation under the allocator's mutexes and is documented by VMA for debugging
  use; it ran in `renderFrame`, the function designated as the only valid source of frame-time
  evidence, so the measurement carried a full allocator traversal inside it. Replaced with
  `vmaGetHeapBudgets`, which VMA documents as fast enough to call every frame and which reports
  the same total. Latent rather than harmful — nothing times frames yet — which is why it never
  showed up as a number.

- **A degenerate camera basis rendered an empty frame with no error.** With forward parallel to
  up — a straight-down camera with a default up axis — the side axis is the cross product of
  parallel vectors, `normalise` returns zero by design, and the basis collapses silently.
  `renderFrame` now refuses it with a diagnostic. Refused rather than resolved with a substitute
  up axis, because a substitute renders a plausible image at an arbitrary roll and a centroid
  measured in a rotated frame is still a number. Covered by `render.renderer-contract`, which
  also pins that the guard does not reject the steeply-down camera the harnesses actually use.
- **The LOD gate's `sweepDiscriminates` was named after a measurement it does not read.** It is
  computed from the descent's pop counts; `sweepProduction` and `sweepControl` sit a few lines
  above, computed, printed, and read by no verdict. Renamed `descentControlDiscriminates`. The
  value was always the intended one — the name pointed at the wrong instrument, in the one file
  whose job is stopping a reader from confusing two measurements.
- **Terrain recomputed every coarse sample from scratch.** A vertex's coarse counterpart is the
  same vertex snapped to even indices, and `surfacePoint` there evaluates the identical
  expression on identical operands as the fine sample already computed at that position — so it
  was a second ten-octave noise evaluation for a value already in memory, and only 25 of a 9×9
  patch's coarse samples are distinct while each was recomputed up to four times. The fine grid
  is now computed once and the coarse sample read out of it: **162 `surfacePoint` calls per patch
  become 81**, exactly half, with bit-identical output. Measured on the LOD gate: **8 m 29.6 s to
  5 m 26.0 s**, a 36% reduction in total wall time on a run that is substantially GPU work and
  readback. Output byte-identical across the change.
- **Nothing recorded which shader binary produced a published number.** Debug compiles `-g -O0`
  and Release `-O`, `spirv-opt` may reassociate floating-point arithmetic, and ADR 0010 governs
  MSVC and says nothing about the GPU. Both configurations run the full suite, so every gate
  result exists in two variants from two different shader binaries. `sol::render::shaderBuildDescription()`
  bakes the flags in from the same CMake variable that builds the `glslc` command line — so the
  reported and invoked flags cannot drift — and all three gates print it beside the device, e.g.
  `glslc --target-env=vulkan1.2 -g -O0 (Debug)`. This records the divergence rather than removing
  it; the Debug shaders stay unoptimised so that a capture remains readable, which the
  GFXReconstruct workflow established on 2026-08-14 now exercises.

- **Terrain was culled against the horizon but not the view frustum**, so patches behind the
  camera were selected, subdivided to full depth, uploaded and drawn. Now culled against both,
  from planes extracted out of the frame's own view-projection, and culled *before* the
  subdivision decision so a rejected node takes its subtree with it.

  This was initially deferred on the argument that a visibility cull is exactly the kind of
  change that makes patches appear and disappear, and that adding one while the pop detector
  cannot fire is the wrong order of work. The user directed it be done. The concern is answered
  by construction rather than waived: a conservative cull removes only what was never visible, so
  **the image must be unchanged**, and that is checkable without a pop detector. Across the
  gate's 600-step descent in both configurations every image-derived statistic is identical —
  pops, worst ratio 5.53×, medians 0.0832 and 0.0885, maxima 0.2014 and 0.3222, both
  concentrations. Only counts moved: **peak patches 1 008 → 240**, vertices 81 648 → 19 440, gate
  wall time **5 m 26.0 s → 1 m 59.2 s**.

  Second-order effect, recorded because it looks like a regression and is not: the gate's
  transition *sweep* reports different numbers, because `findTransitionAltitude` picks its band
  by the largest patch-count change and the counts moved, so it selected a different transition
  (213 432 m against 140 866 m). The sweep is not stable under changes to patch selection — a
  property of that instrument, and another reason the descent carries the verdict.

  Taken together with the coarse-sample reuse above, the LOD gate went from **8 m 29.6 s to
  1 m 59.2 s** across this round — 4.3×, with byte-identical descent imagery throughout.

**Open: none.** All ten findings from this round are applied, and the frustum-culling half of the
terrain finding with them.

### Outstanding for B1

- ~~**The Intel UHD is unmeasured.**~~ **Closed 2026-08-14. Every gate B1 can reach now has a
  both-device result**, and the reason it could not before was structural rather than a matter of
  effort: `Renderer::create` selected a physical device internally, preferring a discrete GPU, so
  no harness could ask for the other one. Every earlier B1 result is an RTX 4060 result *by
  construction rather than by choice*, and the evidence plan's both-devices requirement was
  unreachable rather than merely outstanding. `DeviceSelection` and a shared `--device` harness
  option close it.

  The design point worth recording is the failure mode: **an unmatched selection fails and never
  falls back.** A run launched as the integrated GPU that silently got the discrete one would emit
  a complete, internally consistent report describing the wrong hardware, and nothing downstream
  could catch it because every field would agree with every other field. `render.renderer-contract`
  pins both directions.

  Two results from the second device are worth carrying forward rather than filing as duplicates.
  **The Intel's memory reading is flatter than the RTX 4060's on every statistic** — second-half
  trend 0.4 KiB/min against 15.9, growth 0.00 MiB against 0.98 — which is precisely the shape the
  *withdrawn* memory claim had, so it was not reported until both negative controls had been run
  on that device and failed there.

  And **the Intel presented more frames than the RTX 4060**, 316 689 against 256 954 over the same
  30 minutes. That says the traverse is not GPU-bound and points at the evidence plan's
  display-topology hazard — the integrated GPU drives the internal panel while the discrete one may
  need a cross-adapter copy — but its sharper consequence is methodological: a per-frame leak
  scales with frame count, so the 8-byte control's sensitivity is **2× different on the two
  devices** running the same binary (531.2 KiB/min against 271.7). A control's sensitivity is a
  property of the run, not of the code, which is the reason B2's constrained-CPU controls are
  specified to run under the same constraint as the measurement they certify.

- The
  memory half is **closed on both devices**: the measurement method was defined and
  **ratified by the user on 2026-08-14**, recorded in the [P1b milestone
  plan](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) beside the popping method it
  parallels, and the traverse passes under it — 256 954 frames over a graded 30.0 minutes,
  second-half trend 15.9 KiB/minute against a 64 limit, 0.98 MiB of growth against a 2 MiB
  backstop, every device-side figure constant, with an 8-byte-per-frame control failing both
  criteria. Details in [B1's evidence index](../evidence/p1b/B1/Index.md).

  Stated with the pass rather than left to be inferred: it bounds the settled growth rate and does
  **not** prove an asymptote, since no finite window can, and it cannot see growth below the
  instrument's noise floor at 30 minutes. The gated statistic also has only three clean
  observations so far — 12.2, 2.1 and 15.9 KiB/minute — all inside the limit, worst case a 4×
  margin.

  The measurement deliberately does **not** use the figure the earlier claim was withdrawn over.
  Device allocation is constant by construction under a fixed-capacity design, so it was replaced
  as the primary instrument by process commit charge, which can move and which is the only thing
  that can see a host-side leak at all.
- The LOD gate's transition scan has no coverage for its no-transition branch. It needs a scan
  that finds nothing, and it lives inside a harness that is itself `DISABLED`, so a test would not
  run even if written. The two renderer defects from the same round are covered by
  `render.renderer-contract`; this one is left deliberately and is the weakest point of that
  round.
- The atmosphere — the P1b plan's narrowing option permits a simple analytic shell. Note that a
  shell has no LOD transitions, so "atmosphere LOD popping" becomes vacuous under that option,
  which should be stated rather than quietly satisfied.
- Representative scalable quality settings including a conservative low tier (ADR 0002).
- Performance and memory evidence on both available devices, recorded under their own names and
  never as baseline-class proxies. Note the depth attachment currently uses `STORE` rather than
  `DONT_CARE` to support readback; that bandwidth is included in any frame-time figure.
- ~~The documented Direct3D 12 comparison analysis, and ADR 0002's disposition.~~ **Done 2026-08-14:** the [analysis](../SolProjectNotes/Milestones/P1b-Direct3D12-Comparison-Analysis.md) is written and ADR 0002 is Accepted. A capture workflow was established alongside it, using GFXReconstruct from the pinned Vulkan SDK rather than RenderDoc, verified by a 240-frame capture and replay.
- ~~Reconciling the four synthetic baseline device profiles against real capability reports.~~
  **Gate closed 2026-08-14 by user decision**, on real-derived intersection profiles; see the
  capability row above. **The per-device residual stays open as a follow-up, not as a gate**: the
  four *named* classes are still not individually verified, and doing so needs vulkan.gpuinfo.org,
  which returns HTTP 403 to this environment on every path including its documented public API. No
  workaround was attempted — evading a server's refusal is not a unilateral step, and a user agent
  that gets past a block would not make the data more authoritative. Discharging it needs either
  that access restored or the four reports fetched by hand. It bears on baseline-class support
  claims, which nothing in P1b makes.

  Two findings from this work outlive it. **ADR 0002's Vulkan 1.2 floor has a measurable cost**:
  LunarG's widely-deployed desktop baseline was still Vulkan **1.1** in its 2022 intersection, and
  2023 is the first that clears the floor — a date where the project previously had an intuition.
  And the `D32_SFLOAT` depth-attachment assumption — the one requirement that can reject a
  *conformant* device, and the value `BaselineDeviceProfiles.h` calls its least trustworthy — is
  now traceable: all four intersections list it.
- Licence notices for the four packages, before anything is distributed.
- `evidence/p1b/B1/` **now exists**, created 2026-08-14: an [index](../evidence/p1b/B1/Index.md), a
  [handoff record](../evidence/p1b/B1/Handoff.md), and `raw/` holding the output of all seven
  render executables in both configurations, captured directly because CTest records pass or fail
  rather than the printed measurements. `raw/` is excluded by `.gitignore`, as P1a's is, and is
  reproducible from the commands in the handoff. There is also a milestone-level
  [P1b index](../evidence/p1b/Index.md). This is the mid-increment form: full closure evidence
  still requires the increment to be finished.

**Increment B2 is authorized as of 2026-08-14** and starts after B1 closes; see the [B2 authorization section](#p1b-increment-b2-implementation-authorization-2026-08-14) for its branch, sequencing, and scope. The open question it inherited — its 4 ms and 1 ms gates being specified against i5-8400 / Ryzen 5 2600 CPU classes while the available i7-12650H is materially faster — was settled at authorization by extending the reference-hardware evidence plan with a CPU section, rather than by amending the baseline classes.

**P1a is integrated, not released.** These are different states and the distinction is deliberate. A3 and the review corrections were committed as `d314dad` and `1e8f6ea`, opened as PR #3, and merged into `dev` on 2026-08-13 as `bf18c33`; a clean rebuild of `dev` passes 20/20 in both configurations. Nothing is tagged, nothing is on `main`, and no release exists. Tagging and any promotion to `main` remain separate explicit user requests under `AGENTS.md`.

A3 discharged the obligation A2 carried forward: celestial origin motion is now ADR 0011 conic propagation rather than linear extrapolation. It was added in the orbit library rather than by editing A2's frame library, so A2's committed evidence stays reproducible and the difference between the two models is itself a measured result.

**No open decision requires a user ruling.** A3 raised six findings; all were resolved within the increment. Three were defects in A3's own code or claims, fixed before the numbers were treated as evidence, and are recorded in [A3's handoff](../evidence/p1a/A3/Handoff.md).

**No open decisions.** The four raised by increments A1 and A2 were all resolved by the user on 2026-08-12:

| Decision | Resolution |
|---|---|
| JPL Horizons serves **DE441** while ADR 0008 named DE440 | [ADR 0008](decisions/0008-astronomical-reference-data-and-time-boundary.md) amended to name the **DE440/DE441 solution family**, with each fixture required to record which product supplied it. Downloading `de440.bsp` was rejected: a 114 MB Git LFS binary plus a reopened CSPICE dependency review, for numbers that agree with what Horizons already serves. |
| ADR 0008's launch anchor was defined "5 m above mean sea level", a geoid statement the project cannot honour | ADR 0008 amended to define the anchor **5 m above the reference ellipsoid** named by `BODY399_RADII` in the pinned kernel, and to require a datum to travel with every geodetic coordinate. A geoid model was rejected as 30 m of machinery for a fictional facility; WGS84 was rejected because no pinned NAIF kernel supplies its constants. |
| ADR 0010 required contraction be "disabled explicitly", which MSVC 19.51 cannot do | [ADR 0010](decisions/0010-determinism-and-floating-point.md) amended in place: prefer an explicit flag, and permit reliance on the `/fp:precise` default **only** where a negative control proves it by measurement. |
| A1's `TimingScenario`, marked disposable and "replaced in A2" | Removed. `FrameModelCost` does its job against real conversions; the evidence and rationale remain in [A1's index](../evidence/p1a/A1/Index.md), which carries a dated addendum. |

## Known risks

- Seamless surface-to-space rendering and physics can dominate the project unless constrained by explicit fidelity and content boundaries.
- Time warp and switching between numerical and analytical simulation can produce discontinuities or divergent outcomes.
- Per-part spacecraft physics plus individual people and a long-running economy creates interacting simulation scales.
- Save compatibility and modding constrain entity identity, schemas, and content registration from the first persistent prototype.
- A solo, AI-assisted project requires aggressive vertical slicing and honest deferral of editor, colony, politics, and combat scope.
