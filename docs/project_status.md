# Project Status

**Phase:** P1a — Precision and orbit prototypes; increments A1 and A2 complete, A2 awaiting review

**Planning gate:** Approved on 2026-08-12

**Implementation authorization:** **Granted on 2026-08-12** by user direction, scoped to the [P1a milestone plan](../SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md). Extended to increment A2 on 2026-08-12 by direct user instruction.

**Single writer:** Claude. A1 landed as PR #1 from `feature/p1a-precision-and-orbit`; A2 is uncommitted in the `dev` working tree, because branch creation was not requested.

**Implementation:** Started — P1a increments A1 (measurement and build harness) and A2 (reference frames and numerical precision)

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

## Planning gate

**Approved by the user on 2026-08-12.** This approval closed P0 and accepted the planning foundation only.

**Implementation authorization followed on 2026-08-12**, separately and explicitly, scoped to P1a. The user granted authorization, named Claude as the single writer, and authorized creating `feature/p1a-precision-and-orbit` from `dev`. Authorization does not extend to committing, pushing, merging, tagging, or opening a pull request; each still requires explicit user direction per `AGENTS.md`.

Scope granted now covers **P1a increments A1 and A2**. The user instructed implementation of A2 directly on 2026-08-12; that instruction is the authorization, and it superseded the earlier note that A2 would be scoped only after A1's evidence was reviewed. Increment A3 is planned but not started and is **not** authorized.

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
| P1a — Precision and orbit prototypes | A1 and A2 complete; A3 not started | Headless evidence for frame conversion, precision budget, and hybrid propagation under warp |
| P1b — Renderer and craft prototypes | Blocked on P1a; not authorized | Vulkan surface-to-orbit precision evidence and constructed-craft feasibility |
| P2 — First-playable production | Not started | Design-build-fly-explore-research-company loop |
| P3 — Orbital company | Not started | Persistent operations, stations, people, maintenance, logistics, and manufacturing |
| P4 — Solar economy | Not started | Mining, markets, corporations, nations, and strategic command |
| P5 — Settlement era | Not started | Habitats, off-world colonies, independence, and deeper politics |
| P6 — Contested system | Not started | Piracy, security, fleets, and optional combat |
| P7 — Far future | Not started | Fusion, antimatter, outer-system scale, and later speculative technologies |

## Current blockers and open decisions

P0 has no remaining planning blockers, and P1a's authorization prerequisites are satisfied: implementation is authorized, Claude is the named single writer, and the branch/base are recorded above.

P1a increments A1 and A2 are complete and awaiting user review of their evidence ([A1](../evidence/p1a/A1/Index.md), [A2](../evidence/p1a/A2/Index.md)). A3 is not authorized. P1a needs no GPU hardware; the reference-hardware evidence plan is a P1b prerequisite. Product and architecture questions that belong to later milestones remain tracked in [Open Questions](../SolProjectNotes/Open-Questions.md); they do not authorize implementation-time guessing.

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
