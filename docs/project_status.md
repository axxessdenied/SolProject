# Project Status

**Phase:** P1a — Precision and orbit prototypes: **complete**, reviewed and closed on 2026-08-12. P1b is the next stage and is not yet authorized.

**Planning gate:** Approved on 2026-08-12

**Implementation authorization:** **Granted on 2026-08-12** by user direction, scoped to the [P1a milestone plan](../SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md). Extended to increment A2, and then to increment A3, on 2026-08-12 by direct user instruction. **That scope is now spent:** P1a is closed, and P1b requires a new and separate authorization.

**Single writer:** Claude. A1 landed as PR #1 from `feature/p1a-precision-and-orbit`; A2 landed as PR #2 from the `dev` working tree. A3 and the milestone-review corrections are on `feature/p1a-hybrid-orbit-and-warp`, branched from `dev` at the user's explicit request and **not yet committed**.

**Implementation:** P1a complete — increments A1 (measurement and build harness), A2 (reference frames and numerical precision), and A3 (hybrid orbit and time warp), all closed against their accepted thresholds with evidence indexed at [`evidence/p1a/Index.md`](../evidence/p1a/Index.md)

This is the single source of truth for project phase, milestone state, blockers, planning-gate state, and implementation authorization. Design intent belongs in `SolProjectNotes/`; implemented technical truth will belong in `docs/architecture.md`.

## Current state

- Product priorities and long-term direction have been captured from the initial design interview.
- SolEngine, **Frontiers of Sol**, the `sol` C++ namespace, the C++ naming conventions in ADR 0003, and the subsystem/API documentation policy in ADR 0006 are confirmed.
- Windows x64, single-player, real-scale seamless surface-to-space travel, modular part construction, and a hybrid simulation model are confirmed.
- The initial campaign epoch is fixed at 2026-01-01 00:00:00 UTC in the real Solar System, using real astronomical names/data and fictional companies and politics. DE440/DE441 and NAIF data and an explicit UTC-to-TDB boundary own reference fixtures. P1 uses a fixed launch anchor at 28.0° N, 80.5° W, 5 m above the reference ellipsoid; final fictional terrain placement and regulatory context remain open (ADR 0008).
- The first playable uses external third-person flight, instruments, and an orbital map. Cockpit/IVA is later; walking inside ships is deferred beyond the current roadmap.
- C++23, MSVC, CMake, and Ninja are accepted. Vulkan 1.2 is the accepted P1 candidate floor with per-device capability queries; final production adoption remains Proposed until ADR 0002 receives P1 evidence.
- The baseline PC is an Intel Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, and an SSD, targeting 60 FPS at 1080p on low/medium settings. Intel UHD 630 and AMD Vega 8-class integrated graphics are investigation targets for 30 FPS at 720p/low.
- Keyboard/mouse is the first input target. The remappable defaults use W/S for pitch, A/D for yaw, Q/E for roll, Shift/Ctrl for throttle increase/decrease, Z/X for full/cut throttle, Space for staging, T for stability assist, M for the orbital map, right-mouse drag to orbit the external camera, and the wheel to zoom. Initial capability also includes attitude-rate damping, throttle hold, heading/prograde/retrograde indicators, maneuver guidance, and staging warnings. Automated launch, maneuver execution, rendezvous/docking, and mission scripting are research-gated.
- The first contract, **Orbital Environmental Survey**, uses an uncrewed craft to reach an approximately 200 km by 200 km orbit, remain in a stable orbit for one complete revolution, collect radiation, magnetic-field, and upper-atmosphere observations, and transmit valid data. Reentry and recovery are not required.
- SolEngine begins as internal static libraries with no promised C++ binary ABI. Future native extension must use a stable C interface or scripting/data boundary rather than engine internals.
- The company initially owns a small assembly hangar, mission-control room, one launch pad, and limited test equipment, while leasing major manufacturing and tracking services.
- Persistent formats are versioned from first use. Internal pre-alpha saves may be disposable; version 1.0 migrates all supported public-alpha saves/blueprints, and later releases support their current major series plus the final schema of the previous major. Artifact families are accepted in ADR 0009.
- vcpkg manifest mode with a reviewed pinned baseline is the accepted dependency acquisition policy. No package or dependency declaration has been selected or added (ADR 0007).
- Determinism is bit-exact on the same build and machine and tolerance-based across machines, using `/fp:precise` and `/arch:AVX2` (ADR 0010). `/arch:AVX2` places a hard CPU floor at Haswell-era Intel and Zen-era AMD.
- The authoritative orbital model is patched conics with spheres of influence, with no perturbations, drag, or orbital decay (ADR 0011). Aerodynamic forces still apply to active craft inside the atmosphere. Lagrange points and perturbation-driven mission design are unavailable while this ADR stands.
- Assets are authored in Blender, interchanged as glTF 2.0, generated procedurally where parametric, and baked at build time; binary source assets use Git LFS (ADR 0012). Procedural geometry generation is an M3 engine deliverable.
- The technical-risk prototype scope is split across the [P1a milestone plan](../SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md) (headless: harness, frames/precision, hybrid orbit and warp) and the [P1b milestone plan](../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) (renderer and constructed craft). Renderer frame-time gating is deferred to M2; versioned persistence round trips moved to M1.
- The working time budget is 40+ hours per week.
- FactoryProject has been consulted for workflow patterns and remains reference-only.
- Agent contracts and the initial planning-document set are established.
- P1a increment A1 is building the first CMake project and disposable headless prototype targets under `prototypes/p1a/`. No runtime assets and no dependency declarations exist; P1a is deliberately dependency-free, so `vcpkg.json` remains absent.
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

**This scope is now spent.** Authorization never extended to committing, pushing, merging, tagging, or opening a pull request, and it does not extend to P1b. Each remains a separate explicit request under `AGENTS.md`.

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
- [x] Vulkan 1.2 P1 candidate floor, capability strategy, hardware tiers, and conditional fallback accepted; final production Vulkan adoption remains Proposed in ADR 0002 until P1 evidence.
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
| P1a — Precision and orbit prototypes | **Complete** — reviewed and closed 2026-08-12; implementation-complete, not integrated into `dev`, not released | Headless evidence for frame conversion, precision budget, and hybrid propagation under warp |
| P1b — Renderer and craft prototypes | Blocked on P1a; not authorized | Vulkan surface-to-orbit precision evidence and constructed-craft feasibility |
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

**P1b is not authorized.** P1a's authorization scope is spent, and P1b needs a new and explicit grant naming its own single writer and branch. P1b also needs the reference-hardware evidence plan, which P1a did not require. Product and architecture questions belonging to later milestones remain tracked in [Open Questions](../SolProjectNotes/Open-Questions.md); they do not authorize implementation-time guessing.

**Nothing has been committed.** A3 and the review corrections sit uncommitted on `feature/p1a-hybrid-orbit-and-warp`. Commit, push, merge, tag, and pull request each remain a separate explicit user request under `AGENTS.md`.

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
