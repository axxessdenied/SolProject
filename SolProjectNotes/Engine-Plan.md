# SolEngine Plan

**Status:** Initial proposal for review

**Engine:** SolEngine

**Product:** Frontiers of Sol

**Implementation gate:** Closed; see `docs/project_status.md`

## 1. Mission

SolEngine exists to make the game possible, testable, and maintainable. It should solve the unusual problems of seamless astronomical scale, constructed spacecraft, direct flight, long-duration simulation, and a growing economic world without accumulating features that the game does not exercise.

The engine and game will be developed together through vertical slices. An engine subsystem is “done” only when a runnable game or tool workflow proves it useful.

## 2. Success criteria

The architecture succeeds when it can eventually support:

- uninterrupted player-controlled travel from a planetary surface into orbit;
- individual-part craft with reusable modular assemblies and portable blueprints;
- stable flight near a body and analytical propagation across long coasts;
- safe time acceleration with understandable restrictions and transitions;
- data-driven content and mods without exposing third-party implementation details;
- long-running single-player saves with explicit versions and migrations;
- ships with up to 32 people, stations with up to 128, and colonies with up to 256 as initial design targets;
- a headless simulation path for fast automated tests and long-duration validation;
- Windows x64 at 60 FPS and 1080p on the discrete baseline (Core i5-8400/Ryzen 5 2600, GTX 1060 6 GB/RX 580 8 GB, 16 GB RAM, SSD), with a 30 FPS/720p-low investigation tier for UHD 630 and Vega 8-class integrated graphics and scalable higher-quality options for stronger hardware.

Early player craft target roughly 150–300 parts. Object counts beyond the confirmed people targets, terrain detail, actual support status of the integrated investigation tier, and simultaneous active-physics craft remain open and must not be invented by implementation.

## 3. Engine boundaries

### SolEngine responsibilities

- application lifecycle, platform abstraction, input, windows, and devices;
- time domains, task scheduling, diagnostics, logging, profiling, and crash context;
- math, explicit-unit support, transforms, reference-frame primitives, and spatial queries;
- renderer, materials, cameras, terrain/planet rendering support, UI integration, audio, and assets;
- persistence primitives, stable identifiers, reflection/registration support if justified, and content validation;
- narrow integration boundaries for third-party physics and other libraries;
- test harnesses and deterministic scenario infrastructure.

SolEngine modules begin as internal static library targets linked into the game and only the tools/tests that need them. They expose Doxygen-documented source-level interfaces in owned subsystem namespaces such as `sol::core`, `sol::render`, `sol::platform`, and `sol::assets`; implementation-only symbols stay under the owning subsystem's `detail` namespace (ADR 0006). They expose no stable C++ binary ABI. Modding begins with data; scripting or a versioned opaque-handle C interface requires later demonstrated need (ADR 0005).

### Game responsibilities

- celestial catalogues, ephemerides, gravity models, atmospheres, and astrodynamics policy;
- parts, assemblies, craft, staging, propulsion, resources, aerodynamics, thermals, damage, and control;
- construction and flight workflows, mission planning, science, research, and progression;
- company finances, contracts, mining, logistics, manufacturing, maintenance, workforce, and markets;
- people, habitats, stations, settlements, corporations, nations, politics, piracy, and combat;
- game rules, content definitions, balance, saves, and mods.

The game may drive new engine abstractions, but game concepts should not migrate into SolEngine merely to make the engine appear reusable.

## 4. Load-bearing technical model

### 4.1 Space, frames, and precision

One global floating-point transform cannot safely represent millimetre-to-astronomical scales. The proposed model combines:

- a hierarchy or graph of named reference frames;
- authoritative double-precision positions/velocities in explicitly identified frames;
- local, recentered physics spaces for active interactions;
- camera-relative floating-point render transforms;
- explicit and tested frame conversions at subsystem boundaries.

The technical prototype must compare candidate frame graphs, origins, precision budgets, and conversion costs before the data model is frozen.

### 4.2 Hybrid object simulation

Objects can occupy different simulation regimes:

| Regime | Intended use | Candidate method |
|---|---|---|
| Active local | Piloted craft, docking, collisions, atmosphere | Fixed/sub-stepped rigid-body and force integration |
| Active orbital | Maneuver planning and non-contact flight | Numerical or conic propagation at controlled accuracy |
| Inactive/coasting | Stable trajectories away from encounters | Analytical propagation and scheduled events |
| Strategic aggregate | Later fleets, markets, remote people | Event-driven or coarse deterministic updates |

Promotion/demotion is a contract, not an optimization detail. Each transition must define preserved state, eligibility, tolerances, wake-up events, and failure handling.

### 4.3 Seamless planets

The visual and control experience should remain continuous from surface to space. The implementation may use layered terrain LOD, atmosphere shells, multiple render passes, dynamic near/far strategies, impostors, and frame changes. “Seamless” does not imply globally simulated ground detail or every distant body at full precision.

The game uses real Solar System names, planetary radii, orbital distances, and vetted astronomical data with an initial campaign epoch displayed as 2026-01-01 00:00:00 UTC. DE440 and pinned NAIF generic kernels provide reference states at an explicit TDB ephemeris boundary. P1 uses a reproducible anchor at 28.0° N, 80.5° W, 5 m elevation; the final scope uses one fictional commercial launch facility in that region, one high-detail launch region, and one primary body while retaining full scale (ADR 0008). Final terrain placement and regulatory/operating context remain open. Planet-wide authored detail, arbitrary cities, caves, oceans, and deformable terrain are out of scope until explicitly added.

### 4.4 Time and scheduling

A central time service should make clock use explicit. Local physics, trajectories, economy, people, contracts, and rendering will not all update at the same cadence. The design must support pause, bounded warp, scheduled events, warp interruption, and deterministic catch-up without iterating every skipped frame.

Unresolved policies include thrust under warp, attitude control under warp, collision prediction, atmosphere/encounter warp limits, and how remote automation behaves.

### 4.5 Constructed craft

A craft is a persistent design plus a runtime instance. A blueprint contains parts, attachment topology, symmetry, action/control bindings, staging, assemblies, metadata, and content-version references. The runtime craft adds physical state, resources, damage, crew, ownership, and mission state.

Reusable assemblies should be nestable only if dependency/cycle rules remain understandable. Import must validate missing mods/content and never silently substitute parts that alter performance.

The initial construction target combines fixed functional parts with procedural tanks and structural pieces, node attachment, and limited surface attachment. Early craft should remain usable around 150–300 parts; prototype budgets must test that range rather than treating it as an unlimited guarantee.

Fluid/plumbing and electrical topology are real construction systems. The proposed first implementation uses explicit, editable logical networks—propellant/other fluid routes, valves/crossfeed, electrical buses, sources, storage, and loads—without requiring the player to place every hose or wire as collision geometry. The visual routing and internal-component-placement depth remain open.

### 4.6 Persistence and modding

Save compatibility is a later delivery focus, but its foundations begin early: stable IDs, version fields, explicit registries, deterministic content resolution, and migrations. “We will add versioning later” is not compatible with a persistent sandbox.

Blueprints, settings, content packs/mods, and campaign saves are separate artifacts with different format versions and compatibility rules. All are versioned from first persistence. Internal pre-alpha data may be invalidated deliberately; the first public alpha begins guaranteed cross-release migrations for supported saves and blueprints (ADR 0004). Settings/content use UTF-8 JSON, blueprints use versioned JSON-manifest packages, and campaign saves use JSON-manifest chunked binary containers. Version 1.0 migrates all supported public-alpha saves/blueprints; subsequent support covers the current major series and final previous-major schema (ADR 0009).

## 5. Dependency strategy

The engine is owned by the project, not necessarily every low-level implementation. Focused third-party libraries are acceptable for window/input, graphics access, UI, physics, audio, serialization, testing, profiling, and other commodity capabilities.

Each dependency proposal must document:

- the milestone and requirement that needs it;
- credible alternatives, including a small in-house option where realistic;
- C++/MSVC/Windows compatibility and maintenance activity;
- license and distribution implications;
- version pin and acquisition method;
- public API leakage and replacement cost;
- build, debug/release, and test impact.

The accepted foundation is C++23, MSVC, CMake, and separate Debug/Release Ninja presets (ADR 0001). vcpkg manifest mode with a reviewed `builtin-baseline` is the default C/C++ acquisition/pinning path, but this policy does not select any package (ADR 0007). Vulkan 1.2 is the accepted P1 candidate floor; Vulkan remains a Proposed production renderer until evidence closes ADR 0002. Prefer proving the hardest constraints before selecting convenience libraries that prematurely lock the remaining architecture.

## 6. Development stages

Every stage ends in runnable evidence. Milestone numbering will be finalized only after P0 approval.

### P0 — Product and architecture planning

Deliver the repository contract, GDD, engine plan, open-question register, ADR process, first-playable scope, risk prototypes, and technology decision matrix.

**Exit:** the planning checklist in `docs/project_status.md` is complete and the user explicitly approves the planning gate. Implementation authorization is a separate control.

### P1 — Technical risk prototypes

The implementation-ready scope, increments, measurement rules, and thresholds are authoritative in [P1 — Technical Risk Prototypes](Milestones/P1-Technical-Risk-Prototypes.md). P1 is planned but cannot begin until the approved planning gate is accompanied by explicit implementation authorization and the milestone prerequisites are satisfied.

Build disposable, instrumented prototypes for:

1. precision and reference-frame conversion from surface scale to orbit;
2. Vulkan surface-to-orbit planet/atmosphere rendering, depth behavior, frame pacing, device-capability reporting, and scalable quality on the discrete baseline plus UHD 630/Vega 8-class investigation systems;
3. local integration ↔ analytical orbit transition and time warp;
4. assembled 150–300-part rigid-body behavior, staging, deterministic failure handling, and explicit fluid/electrical networks;
5. serialization round trips for stable IDs, blueprints, and versioned content references.

These prototypes answer ADR questions. Production code is not required to reuse them. Accepted headline gates include 1 m/1 mm/s hybrid handoffs, 100 m one-orbit reference error, 0.25-pixel stationary render jitter, p95 renderer frame times of 16.67 ms on the discrete baseline and 33.3 ms on the integrated investigation tier, and p95 300-part budgets of 4 ms for craft physics plus 1 ms for resource networks.

**Exit:** measured results choose or reject candidate architectures against accepted tolerances and performance budgets.

### P2 — First-playable production

#### M1: Foundation and headless harness

Application lifecycle, explicit time services, logging/diagnostics, math/units, testing, content validation skeleton, and a headless scenario runner.

**Playable proof:** a deterministic scenario can run headlessly and the graphical shell displays the same authoritative clock/state summary.

#### M2: Large-world rendering and reference frames

Camera-relative rendering through the P1-selected production API, one full-scale primary body, a bounded high-detail launch region, sky/space transition, reference-frame diagnostics, capability-based graphics tiers, and initial terrain/atmosphere presentation.

**Playable proof:** the camera travels continuously from the launch surface to orbital altitude without visible coordinate jitter, a loading screen, or depth collapse, while meeting 60 FPS/1080p on the discrete baseline. UHD 630 and Vega 8-class results are documented against the 30 FPS/720p-low investigation target; unsupported status must be explicit rather than hidden by lowering simulation fidelity.

#### M3: Parts, assemblies, and construction

Data-driven fixed functional parts, procedural tanks/structures, node and limited surface attachment, symmetry, modular assemblies, mass/resource aggregation, staging/action groups, logical plumbing/fuel and electrical networks, blueprint save/load/import, and construction validation.

**Playable proof:** build and reload a valid staged chemical launch vehicle; invalid or missing-content designs fail with actionable diagnostics.

#### M4: Direct atmospheric flight

Active craft physics, external third-person camera, instrumentation, keyboard/mouse controls, propulsion, explicit propellant flow, gravity, simplified aerodynamics/heating, basic structural limits, telemetry, and deterministic damage/failure state. Remappable defaults use W/S for pitch, A/D for yaw, Q/E for roll, Shift/Ctrl for throttle increase/decrease, Z/X for full/cut throttle, Space for staging, T for stability assist, M for the orbital map, right-mouse drag to orbit the external camera, and the wheel to zoom. Initial guidance also includes attitude-rate damping, throttle hold, heading/prograde/retrograde indicators, maneuver guidance, and staging warnings. Automated launch, maneuver execution, rendezvous/docking, and mission scripting are progression-gated.

**Playable proof:** pilot a designed rocket from ignition through controlled ascent or a recoverable failure, with deterministic headless checks for force/resource rules.

#### M5: Orbit, map, and time warp

Trajectory prediction, local/analytical transition, orbital map, maneuver planning, safe warp, encounter alarms, save/load of flight state, and limited mission automation if approved.

**Playable proof:** launch, establish a stable orbit, coast under warp, perform a planned maneuver, and return to active control without unacceptable state discontinuity.

#### M6: Science, research, and contracts

A small private company at a fictional commercial facility near 28 degrees north on Florida's Atlantic coast, funds, reputation/access if selected, contract generation, instruments, experiments, orbital data transmission, research unlocks, and a deliberately small technology tree. The company owns a small assembly hangar, mission-control room, one launch pad, and limited testing equipment while leasing major manufacturing and tracking services. The first contract is **Orbital Environmental Survey**: an uncrewed craft reaches an approximately 200 km by 200 km orbit, remains stably in orbit for one complete revolution, gathers radiation, magnetic-field, and upper-atmosphere observations with the appropriate instruments, and transmits valid data.

**Playable proof:** accept a scientific contract, design and fly the mission to stable orbit, gather and transmit valid data, receive payment/research progress, and unlock a meaningful new design option. Reentry and physical recovery are not required for first-playable completion.

#### M7: First-playable integration

Onboarding, save/resume, failure/retry economy, essential audio/UI, settings/difficulty, performance pass, content balance, and packaging.

**Exit:** the complete design → build → fly → explore → earn → research loop is enjoyable and repeatable. It is the scope gate for all strategic expansion.

### P3 — Orbital company

Add cockpit/IVA flight after exterior flight is stable, persistent mission scheduling, hiring and people, life support at the chosen fidelity, maintenance, multiple craft, docking, construction of stations/depots/spaceyards, manufacturing, transport logistics, and strategic command tools. Walking inside craft remains deferred beyond this stage.

### P4 — Solar economy

Add prospecting and asteroid mining, resource geography, industrial chains, remote operations, supply/demand, contracts and private competitors, broader mission planning, and Earth-nation relationships.

### P5 — Settlement era

Add designed habitats, station populations, off-world colonies, local institutions, independence movements, and corporations whose influence can eventually exceed Earth nations.

### P6 — Contested system

Introduce piracy/security first, then optional tactical combat, fleets, and political conflict only after the peaceful economy creates valuable routes and infrastructure worth contesting.

### P7 — Far future

Extend from advanced fission/fusion through antimatter and later speculative technology, outer-system infrastructure, megaprojects, and optional finite victory conditions with continued play.

Earlier historical starting eras are a later content/system expansion after the modern-start simulation is proven.

## 7. Scope controls

Before the first playable, presume these are out of scope unless explicitly promoted:

- multiplayer or networking architecture;
- a general-purpose public engine/SDK;
- a full visual world editor;
- interiors that allow unrestricted first-person walking;
- individual-person daily-life simulation beyond the needs of early craft;
- complete Earth geography/cities and globally detailed terrain;
- full n-body integration for every object at every timestep;
- mining, manufacturing, markets, colonies, politics, piracy, or combat;
- historical start eras and speculative technologies;
- production-quality mod distribution or indefinite save compatibility.

They remain long-term goals where confirmed; deferral protects the spacecraft-first core.

## 8. Decisions

| Decision | Status | Rationale/source |
|---|---|---|
| Build SolEngine for this game rather than as a general engine | Confirmed | User accepted focused third-party boundaries; game drives engine needs |
| Develop through vertical slices | Proposed | Controls solo-project scope and validates every subsystem |
| Hybrid local/analytical/aggregate simulation | Confirmed direction | User selected a hybrid system |
| Seamless surface-to-space is a player-facing contract | Confirmed direction | User requires seamless movement; internal LOD remains allowed |
| Windows x64 and 1080p+ first | Confirmed | User response |
| 60 FPS at 1080p on an older-PC baseline, with higher tiers later | Confirmed goal | User response; exact hardware remains open |
| C++23, MSVC, CMake, and Ninja | Confirmed | User response and ADR 0001 |
| C++ naming conventions and `sol` namespace | Confirmed | User response and ADR 0003 |
| Vulkan as preferred graphics direction | Proposed pending prototype | User preference; ADR 0002 owns acceptance criteria |
| Vulkan 1.2 as the P1 candidate floor with queried optional capabilities | Confirmed for P1 | User accepted recommendation; production adoption remains Proposed |
| Discrete reference hardware listed above; UHD 630/Vega 8-class at 30 FPS/720p-low as investigation tier | Confirmed target | User accepted recommendation; support remains validation-dependent |
| Frontiers of Sol | Confirmed | User selected title |
| Epoch fixed at 2026-01-01 00:00:00 UTC | Confirmed | User accepted recommendation |
| Fictional commercial facility near 28 degrees north on Florida's Atlantic coast | Confirmed direction | User selected region; exact longitude/terrain placement and regulatory context remain open |
| Remappable keyboard/mouse defaults and external orbit-camera behavior described in M4 | Confirmed | User accepted recommendation |
| Named initial guidance set with improved automation gated by technology | Confirmed | User accepted recommendation |
| First orbital environmental-survey mission is uncrewed, targets approximately 200 km by 200 km for one orbit, and transmits three observation types | Confirmed | User accepted recommendation |
| Version all persistence immediately; guarantee migrations from first public alpha | Confirmed | User response and ADR 0004 |
| Internal static SolEngine libraries; no stable C++ ABI | Confirmed | User accepted recommendation and ADR 0005 |
| Owned subsystem namespaces and Doxygen public source-API contracts | Confirmed | User accepted recommendation and ADR 0006 |
| vcpkg manifest mode with a reviewed pinned baseline | Confirmed policy | User accepted recommendation and ADR 0007; no libraries selected |
| DE440/NAIF reference fixtures, UTC/TDB boundary, and fixed P1 launch anchor | Confirmed | User accepted recommendation and ADR 0008 |
| JSON-facing persistence artifacts, chunked campaign container, and bounded migration window | Confirmed | User accepted recommendation and ADR 0009 |
| P1 prototype thresholds and increments | Confirmed | User accepted recommendation; detailed milestone plan is authoritative |
| Own small hangar, mission control, one pad, and limited testing; lease major manufacturing/tracking | Confirmed initial scope | User accepted recommendation |
| Real-scale Solar System and present-day context | Confirmed | User accepted recommendation |
| Fixed functional parts plus procedural tanks/structures; 150–300 early parts | Confirmed direction | User accepted recommendation |
| Explicit plumbing/fuel and electrical systems | Confirmed | User response; interaction depth remains open |
| External third-person first; cockpit/IVA later; walking deferred | Confirmed | User response |
| Deterministic early failures; configurable crew death and revert | Confirmed | User response |
| First mission ends in stable orbit and transmitted science | Confirmed | User response; recovery is later |
| Defer editor until workflow demonstrates need | Confirmed | User response |
| First playable ends with a repeatable science-contract loop | Confirmed | User accepted orbit-and-transmit boundary |
| Remaining technology stack | Open | Requires dependency ADRs and risk evidence |
