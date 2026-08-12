# Project Status

**Phase:** Pre-production planning complete; awaiting implementation authorization

**Planning gate:** Approved on 2026-08-12

**Implementation authorization:** Not granted

**Implementation:** Not started

This is the single source of truth for project phase, milestone state, blockers, planning-gate state, and implementation authorization. Design intent belongs in `SolProjectNotes/`; implemented technical truth will belong in `docs/architecture.md`.

## Current state

- Product priorities and long-term direction have been captured from the initial design interview.
- SolEngine, **Frontiers of Sol**, the `sol` C++ namespace, the C++ naming conventions in ADR 0003, and the subsystem/API documentation policy in ADR 0006 are confirmed.
- Windows x64, single-player, real-scale seamless surface-to-space travel, modular part construction, and a hybrid simulation model are confirmed.
- The initial campaign epoch is fixed at 2026-01-01 00:00:00 UTC in the real Solar System, using real astronomical names/data and fictional companies and politics. DE440/NAIF data and an explicit UTC-to-TDB boundary own reference fixtures. P1 uses a fixed launch anchor at 28.0° N, 80.5° W, 5 m elevation; final fictional terrain placement and regulatory context remain open (ADR 0008).
- The first playable uses external third-person flight, instruments, and an orbital map. Cockpit/IVA is later; walking inside ships is deferred beyond the current roadmap.
- C++23, MSVC, CMake, and Ninja are accepted. Vulkan 1.2 is the accepted P1 candidate floor with per-device capability queries; final production adoption remains Proposed until ADR 0002 receives P1 evidence.
- The baseline PC is an Intel Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, and an SSD, targeting 60 FPS at 1080p on low/medium settings. Intel UHD 630 and AMD Vega 8-class integrated graphics are investigation targets for 30 FPS at 720p/low.
- Keyboard/mouse is the first input target. The remappable defaults use W/S for pitch, A/D for yaw, Q/E for roll, Shift/Ctrl for throttle increase/decrease, Z/X for full/cut throttle, Space for staging, T for stability assist, M for the orbital map, right-mouse drag to orbit the external camera, and the wheel to zoom. Initial capability also includes attitude-rate damping, throttle hold, heading/prograde/retrograde indicators, maneuver guidance, and staging warnings. Automated launch, maneuver execution, rendezvous/docking, and mission scripting are research-gated.
- The first contract, **Orbital Environmental Survey**, uses an uncrewed craft to reach an approximately 200 km by 200 km orbit, remain in a stable orbit for one complete revolution, collect radiation, magnetic-field, and upper-atmosphere observations, and transmit valid data. Reentry and recovery are not required.
- SolEngine begins as internal static libraries with no promised C++ binary ABI. Future native extension must use a stable C interface or scripting/data boundary rather than engine internals.
- The company initially owns a small assembly hangar, mission-control room, one launch pad, and limited test equipment, while leasing major manufacturing and tracking services.
- Persistent formats are versioned from first use. Internal pre-alpha saves may be disposable; version 1.0 migrates all supported public-alpha saves/blueprints, and later releases support their current major series plus the final schema of the previous major. Artifact families are accepted in ADR 0009.
- vcpkg manifest mode with a reviewed pinned baseline is the accepted dependency acquisition policy. No package or dependency declaration has been selected or added (ADR 0007).
- The P1 technical-risk prototype scope, increments, measurement rules, and pass/fail thresholds are accepted in the [P1 milestone plan](../SolProjectNotes/Milestones/P1-Technical-Risk-Prototypes.md).
- FactoryProject has been consulted for workflow patterns and remains reference-only.
- Agent contracts and the initial planning-document set are established.
- No C++, CMake project, runtime assets, dependency declarations, or build artifacts exist.

## Planning gate

**Approved by the user on 2026-08-12.** This approval closes P0 and accepts the planning foundation only. The user explicitly did not authorize implementation. No C++, CMake targets, runtime assets, dependency declarations, branch operations, or other implementation work may begin until **Implementation authorization** above is explicitly changed to **Granted** by user direction and the P1 prerequisites are satisfied.

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
- [x] Technical risk prototypes and measurable pass/fail criteria accepted in the P1 milestone plan.
- [x] Astronomical reference source, UTC/TDB boundary, and reproducible P1 launch anchor accepted through ADR 0008.
- [x] Save/versioning compatibility baseline and artifact/migration policy accepted through ADRs 0004 and 0009.

## Roadmap state

| Stage | State | Outcome |
|---|---|---|
| P0 — Product and architecture planning | Complete | Planning foundation reviewed and gate approved; no implementation delivered |
| P1 — Technical risk prototypes | Ready; not authorized | Evidence for precision, seamless scale, hybrid propagation, and assembled-vehicle physics |
| P2 — First-playable production | Not started | Design-build-fly-explore-research-company loop |
| P3 — Orbital company | Not started | Persistent operations, stations, people, maintenance, logistics, and manufacturing |
| P4 — Solar economy | Not started | Mining, markets, corporations, nations, and strategic command |
| P5 — Settlement era | Not started | Habitats, off-world colonies, independence, and deeper politics |
| P6 — Contested system | Not started | Piracy, security, fleets, and optional combat |
| P7 — Far future | Not started | Fusion, antimatter, outer-system scale, and later speculative technologies |

## Current blockers and open decisions

P0 has no remaining planning blockers. P1 remains inactive because implementation authorization was explicitly not granted. Before P1 begins, the user must authorize implementation, identify the single writer, authorize the required branch/base operations, and accept the evidence plan for accessible reference hardware. Product and architecture questions that belong to later milestones remain tracked in [Open Questions](../SolProjectNotes/Open-Questions.md); they do not authorize implementation-time guessing.

## Known risks

- Seamless surface-to-space rendering and physics can dominate the project unless constrained by explicit fidelity and content boundaries.
- Time warp and switching between numerical and analytical simulation can produce discontinuities or divergent outcomes.
- Per-part spacecraft physics plus individual people and a long-running economy creates interacting simulation scales.
- Save compatibility and modding constrain entity identity, schemas, and content registration from the first persistent prototype.
- A solo, AI-assisted project requires aggressive vertical slicing and honest deferral of editor, colony, politics, and combat scope.
