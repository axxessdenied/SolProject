# Changelog

All notable user-visible changes will be documented here after implementation begins. This file is append-only and will follow Keep a Changelog once the versioning policy is accepted.

## [Unreleased]

### Added

- Implementation began. P1a increment A1 delivers the first CMake/Ninja build graph, Debug and Release presets, and the disposable headless prototypes under `prototypes/p1a/`, including a durable measurement harness that records provenance, timing distributions, peak memory, and allocation counts with every result.
- A determinism harness that proves bit-identical output across separate runs of the same build, and that is itself proven to detect a floating-point flag change.
- P1a increment A2 delivers explicit-unit and explicit-frame types, a six-frame graph from a launch pad to the Solar System barycentre, two candidate frame models measured against each other, and the pinned ADR 0008 reference fixtures with checksums verified at load.
- An explicit UTC/TAI/TT/TDB boundary driven entirely by the pinned leap-second kernel, including correct handling of the 23:59:60 leap-second instant. Its conversion of the campaign epoch agrees with JPL Horizons to the fixtures' printed 0.1 ms.
- P1a increment A3 delivers the ADR 0011 hybrid propagator: a universal-variable Kepler coast that works across every conic type, three candidate fixed-step integrators, the sphere-of-influence hierarchy computed from the pinned reference data, an integer-nanosecond campaign clock, and the transition contract that moves a craft between numerical and analytical propagation.
- Time warp that does not change outcomes. An anchored analytical coast reaches a bit-identical state whether campaign time is advanced one second or ten thousand seconds at a time, and sphere-of-influence crossings are placed at the identical nanosecond across warp factors spanning four orders of magnitude.
- Celestial origin motion by conic propagation, replacing the linear extrapolation increment A2 used as a placeholder and explicitly flagged as not an ephemeris.

### Changed

- Recorded the literal MSVC flag spellings in ADR 0010 after measuring them: `/std:c++23` is unavailable on this toolset, and no flag disables FMA contraction. `/fp:precise` disables it by default, verified by measurement rather than assumed.
- Selected the hierarchical parent-relative frame graph over a single global root, on measured evidence. Both meet every accepted threshold; the hierarchical model is roughly 4,500 times more precise for conversions that do not need barycentric coordinates, and faster for the conversion a renderer performs per object.
- Amended ADR 0008 after measuring what it had left ambiguous: the ephemeris reference is now the DE440/DE441 solution family with each fixture recording its actual product, and the launch anchor's 5 m is now defined against the reference ellipsoid rather than mean sea level. A datum must now travel with every geodetic coordinate, because two defensible ellipsoids move the same anchor 0.403 m — four hundred times the position budget.
- Amended ADR 0010's contraction requirement to one this toolset can satisfy: prefer an explicit disabling flag, and rely on the `/fp:precise` default only where a negative control proves it holds. MSVC 19.51 offers no disabling flag, so the original wording was unsatisfiable as written.
- Selected the local-to-analytical transition contract on measured evidence: one regime owns a craft's state at any instant, the analytical coast anchors on the state it is handed rather than recomputing it, and eligibility is checked by named reason before a coast begins and again after every change of gravitational primary. The handoff is exactly lossless as a result — measured as zero, bit for bit, rather than as a small number.
- Selected RK4 for the local numerical regime. The symplectic candidates hold their energy error bounded where RK4's accumulates, but under the hybrid contract a stable orbit is never integrated for long, and RK4 clears the accepted one-orbit gate at three times less cost.
- Confirmed ADR 0011 from measurement without amending it. Every validation item it named is met, and the atmosphere limit it assigned to increment A3 is recorded at 140 km — chosen as a physics-regime boundary, because deriving it from the handoff tolerance gives a value that would make the first playable's own contract orbit un-warpable.
- Narrowed the powered-warp question from "is it safe" to "which warp factors may be offered". Warp under thrust turns out not to be a determinism problem; the local regime reproduces exactly when the warp tick is an integer multiple of the fixed physics step.
- Closed milestone P1a after a `complete-milestone` review that re-ran both build configurations and re-derived the reported measurements from fresh output rather than reading them from the evidence documents. Eight findings were raised and resolved; none invalidated a threshold result or reversed a selection. Corrected in the process: velocity Verlet's cost comparison, which had been overstated by exactly two because the prototype's stateless integrator API cannot reuse an acceleration a real loop would; the Sun's and Moon's surface radii, left at zero on the mistaken belief the pinned kernel did not supply them, which had silently disabled the below-surface eligibility check for both; and the placement of the post-rebase eligibility re-check, now guaranteed by the operation that changes a craft's central body rather than by one call site.
- Amended ADR 0011 in one clause: sphere-of-influence radii are derived at load from the pinned ADR 0008 data, not recorded as fixtures of their own. The model is unchanged and remains confirmed from measurement.
- Recorded the campaign clock's exactness window — the integer nanosecond count converts to seconds exactly only to 104.25 days — as an architectural limit for P2 rather than a comment in a header. Every P1a measurement falls inside it; a multi-year campaign will not, and determinism is unaffected either way.

### Removed

- `TimingScenario`, the synthetic timing loop A1 built to prove the measurement pipeline and marked disposable. `FrameModelCost` now demonstrates the same pipeline against real frame conversions; A1's evidence retains the record.

### Documentation

- Established the initial SolProject product vision, planning gate, engine proposal, GDD, roadmap, and AI-agent workflow.
- Selected **Frontiers of Sol** as the game title and recorded the initial controls, hardware target, 2026 epoch, uncrewed first mission, C++ conventions, and save-compatibility baseline.
- Defined the internal static-library engine boundary, initial flight-guidance capabilities, integrated-graphics investigation tier, Florida launch region, and starting company facilities.
- Fixed the initial UTC epoch, default flight/camera bindings, first orbital environmental survey, and subsystem namespace/Doxygen policy.
- Accepted the P1 Vulkan 1.2 candidate floor, dependency-pinning policy, DE440/UTC/TDB reference boundary, persistence artifact/migration policy, and measurable technical-risk prototype plan.
- Approved the completed P0 planning gate while explicitly keeping implementation unauthorized.
