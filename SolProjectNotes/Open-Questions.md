# Open Questions

This register prevents unresolved choices from becoming accidental implementation decisions. Move settled game decisions into the relevant GDD note and technical decisions into ADRs.

## Planning gate

No design or architecture questions remain on the planning-gate checklist. The user approved the planning gate on 2026-08-12 but explicitly withheld implementation authorization. Implementation readiness and authorization remain separate from gate approval.

## Needed before active-flight implementation

- Finalize the launch-facility name, terrain placement, regulatory/operating arrangement, and starting staff/inventory/cash/service-contract balance. The regional location and owned/leased split are settled.
- Set the Orbital Environmental Survey's acceptable periapsis/apoapsis band, instrument operation durations, data-validity rules, and rewards.
- Define powered flight and attitude control during time warp.
- Set the atmospheric boundary altitude at which propagation switches between drag-affected local integration and drag-free conic coast, and the handoff behavior across it (ADR 0011 fixes the model on each side but not the boundary).
- Specify simplified atmospheric/aerodynamic/heating behavior and whether weather exists.
- Set structural stress, collision, staging, docking, and fuel-transfer fidelity.
- Define how logical fluid/electrical connections are edited and visualized, whether pressure/voltage are simulated, and whether internal component placement appears before walkable interiors.
- Choose input-device support beyond baseline keyboard/mouse, including gamepad, HOTAS, and custom bindings.
- Define the exact first-playable flight instruments/HUD layout and camera comfort options beyond the accepted external orbit/zoom behavior.
- Choose one versus multiple nearby active-physics craft and confirm the 150–300-part performance test cases.
- Choose the craft physical representation: per-part dynamic bodies with joints, or welded aggregates with breakable constraint groups. P1b increment B2 measures both and recommends one.
- Define the procedural geometry parameter sets for tanks, structural elements, adapters, fairings, and trusses, and how much variation the player can control.
- Define the part attachment-node naming convention and the glTF extras schema the asset bake step validates against.
- Decide whether M2 or a later milestone owns renderer optimization if the deferred 16.67 ms gate is missed with representative assets.
- Define configurable crew-death, revert, quicksave, and difficulty-preset behavior for later crewed missions.
- Select concrete window/input, Vulkan-loading, math, UI, physics, audio, serialization, testing, profiling, and asset-processing libraries only when their owning milestones demonstrate a need.

## Needed before strategic expansion

- Which tasks can crews, mission control, and autonomous systems perform without the player.
- How individual people are represented, what needs they have, and how simulation detail changes with distance/time warp.
- Company ownership model, investors/debt, reputation, licenses, insurance, and bankruptcy/recovery.
- Whether markets are local, delayed by transport, and driven by simulated agents or price curves.
- Nation borders/jurisdiction in space and paths by which corporations overtake national power.
- Colony formation, independence, political identity, and whether the player directly governs colonies.
- Piracy emergence, detection/interception, security law, and the eventual tactical combat model.

## Explicitly settled

- Game title **Frontiers of Sol**; repository codename SolProject; engine name SolEngine.
- Canonical C++ namespace `sol` and naming conventions in ADR 0003.
- Owned subsystem namespaces and Doxygen contracts for public source APIs (ADR 0006).
- Windows x64 first.
- Discrete baseline: Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, SSD; target 60 FPS at 1080p low/medium.
- UHD 630 and Vega 8-class integrated graphics are investigation targets for 30 FPS at 720p/low; support depends on P1 evidence.
- C++23, MSVC, CMake, and separate single-configuration Ninja builds (ADR 0001).
- Vulkan is the preferred graphics direction. Vulkan 1.2 is the accepted P1 candidate floor with per-device capability queries; production adoption remains Proposed pending P1 evidence (ADR 0002).
- Single-player only.
- Real Solar System names, dimensions, orbital distances, and vetted astronomical data, with fictional corporations and politics.
- Initial epoch fixed at 2026-01-01 00:00:00 UTC.
- P1 launch anchor at 28.0° N, 80.5° W, 5 m elevation. The fictional final facility remains in this region and distinct from real launch complexes; authored terrain placement and regulatory context remain open (ADR 0008).
- Real-scale seamless surface-to-space player experience with one initially high-detail launch region.
- Hybrid simulation rather than universal full n-body/local physics.
- Keyboard/mouse first; remappable defaults are W/S pitch, A/D yaw, Q/E roll, Shift/Ctrl throttle increase/decrease, Z/X full/cut throttle, Space stage, T stability assist, M orbital map, right-mouse drag external-camera orbit, and wheel zoom.
- Stability hold, attitude-rate damping, throttle hold, heading/prograde/retrograde indicators, maneuver guidance, and staging warnings are initial capabilities. Automated launch, maneuver execution, rendezvous/docking, and mission scripting are research-gated.
- External third-person flight, instruments, and orbital map first; cockpit/IVA later; walking inside ships deferred.
- Normal difficulty uses genuine orbital/propulsion/mass/resource constraints, simplified aerodynamics/heating, and optional piloting/planning assists. Communications/life support arrive later.
- Individual parts plus reusable/shareable modular assemblies; fixed functional parts, procedural tanks/structures, node plus limited surface attachment, and a roughly 150–300-part early-craft target.
- Explicit plumbing/fuel and electrical systems; exact interaction/visualization depth remains open.
- Deterministic design/piloting failures initially; no first-playable random manufacturing defects; crew death and mission revert are configurable for later crewed play.
- First playable begins with the uncrewed Orbital Environmental Survey: approximately 200 km by 200 km stable orbit, one complete revolution, radiation/magnetic-field/upper-atmosphere observations, and valid data transmission. Reentry/recovery is not required.
- DE440/NAIF reference data with player-facing UTC and a TDB ephemeris boundary (ADR 0008).
- Persistent formats are versioned from first use; pre-alpha data may be disposable; guaranteed migrations begin at first public alpha (ADR 0004).
- UTF-8 JSON settings/content, JSON-manifest blueprint packages, and JSON-manifest chunked binary campaign containers. Version 1.0 migrates all supported public-alpha saves/blueprints; later support covers the current major series plus the final previous-major schema (ADR 0009).
- SolEngine uses internal static libraries and exposes no stable C++ binary ABI; data, scripting, or a versioned C interface are future extension paths (ADR 0005).
- The company initially owns a small assembly hangar, mission-control room, one launch pad, and limited testing equipment while leasing major manufacturing and tracking services.
- Editor only when demonstrated necessary.
- vcpkg manifest mode with a reviewed `builtin-baseline` is the default dependency acquisition policy; individual libraries remain milestone decisions (ADR 0007).
- Determinism is bit-exact on the same build and machine and tolerance-based across machines; `/fp:precise` and `/arch:AVX2`, never `/fp:fast` (ADR 0010).
- The orbital model is patched conics with spheres of influence. No perturbations, drag, or decay in the propagation; aerodynamic forces still act on active craft in atmosphere. Lagrange points and perturbation-driven mission design are consequently unavailable (ADR 0011).
- Assets are authored in Blender, interchanged as glTF 2.0, generated procedurally where parametric, and baked at build time. Binary sources use Git LFS. No purchased packs or commissioned art for the first playable (ADR 0012).
- P1 increments and thresholds are accepted in [P1a — Precision and Orbit](Milestones/P1a-Precision-and-Orbit.md) and [P1b — Renderer and Craft](Milestones/P1b-Renderer-and-Craft.md). Renderer frame-time gating belongs to M2; persistence round trips belong to M1.
- Focused third-party libraries are acceptable; FactoryProject is reference-only.
- Mod support is desired.
