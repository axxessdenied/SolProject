# Architecture

**Status:** Proposed pre-production architecture. Nothing described here is implemented. Accepted decisions will be recorded in `docs/decisions/`; this document becomes implemented technical truth only after code exists.

## Architectural goal

SolEngine supplies the focused services needed to build one demanding 3D space simulation game. It is not intended to become a general-purpose engine. The game remains the validation surface for every engine feature.

## Accepted foundation

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
- JPL DE440 supplies initial astronomical reference states, with player-facing UTC converted at a pinned-data TDB ephemeris boundary; the P1 launch anchor is 28.0° N, 80.5° W, 5 m above mean sea level (ADR 0008).
- Persistence uses human-readable UTF-8 JSON for settings/content, JSON-manifest blueprint packages, and JSON-manifest chunked binary campaign containers, with concrete encodings selected later (ADRs 0004 and 0009).

Vulkan is the preferred graphics direction, not yet an accepted production implementation dependency. P1 uses Vulkan 1.2 as the candidate floor, queries actual device capabilities, and treats later capabilities as optional. It must establish baseline discrete-GPU support, UHD 630/Vega 8-class status, validation/capture workflow, frame pacing, and whether Vulkan remains preferable to Direct3D 12 for this Windows-first project. The discrete baseline p95 gate is 16.67 ms at 1080p low/medium; the integrated investigation p95 target is 33.3 ms at 720p/low. See ADR 0002 and the P1 milestone plan.

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

The initial authoritative campaign epoch is displayed as 2026-01-01 00:00:00 UTC. DE440 and pinned NAIF generic kernels provide the initial reference data; ephemeris fixtures are evaluated at a TDB boundary with complete provenance (ADR 0008). P1 uses 28.0° N, 80.5° W, and 5 m elevation as its reproducible launch anchor. The final facility remains fictional and geographically distinct from real launch complexes; its name, authored terrain placement, and regulatory/operating arrangements remain game-design decisions.

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

P1 prototype tolerances, benchmark hardware, scenarios, and measurement rules are defined in the [P1 milestone plan](../SolProjectNotes/Milestones/P1-Technical-Risk-Prototypes.md). Later production tolerances remain attached to their owning milestones.

## Unresolved foundation choices

Production renderer adoption/abstraction, validation of the integrated investigation tier, window/input library, UI, physics integration, ECS/data model, audio, concrete serialization/archive libraries, test framework, terrain implementation, campaign-time storage, Earth orientation, and mod packaging require explicit decisions before their owning milestones.
