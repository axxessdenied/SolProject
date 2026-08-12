# Changelog

All notable user-visible changes will be documented here after implementation begins. This file is append-only and will follow Keep a Changelog once the versioning policy is accepted.

## [Unreleased]

### Added

- Implementation began. P1a increment A1 delivers the first CMake/Ninja build graph, Debug and Release presets, and the disposable headless prototypes under `prototypes/p1a/`, including a durable measurement harness that records provenance, timing distributions, peak memory, and allocation counts with every result.
- A determinism harness that proves bit-identical output across separate runs of the same build, and that is itself proven to detect a floating-point flag change.
- P1a increment A2 delivers explicit-unit and explicit-frame types, a six-frame graph from a launch pad to the Solar System barycentre, two candidate frame models measured against each other, and the pinned ADR 0008 reference fixtures with checksums verified at load.
- An explicit UTC/TAI/TT/TDB boundary driven entirely by the pinned leap-second kernel, including correct handling of the 23:59:60 leap-second instant. Its conversion of the campaign epoch agrees with JPL Horizons to the fixtures' printed 0.1 ms.

### Changed

- Recorded the literal MSVC flag spellings in ADR 0010 after measuring them: `/std:c++23` is unavailable on this toolset, and no flag disables FMA contraction. `/fp:precise` disables it by default, verified by measurement rather than assumed.
- Selected the hierarchical parent-relative frame graph over a single global root, on measured evidence. Both meet every accepted threshold; the hierarchical model is roughly 4,500 times more precise for conversions that do not need barycentric coordinates, and faster for the conversion a renderer performs per object.
- Amended ADR 0008 after measuring what it had left ambiguous: the ephemeris reference is now the DE440/DE441 solution family with each fixture recording its actual product, and the launch anchor's 5 m is now defined against the reference ellipsoid rather than mean sea level. A datum must now travel with every geodetic coordinate, because two defensible ellipsoids move the same anchor 0.403 m — four hundred times the position budget.
- Amended ADR 0010's contraction requirement to one this toolset can satisfy: prefer an explicit disabling flag, and rely on the `/fp:precise` default only where a negative control proves it holds. MSVC 19.51 offers no disabling flag, so the original wording was unsatisfiable as written.

### Removed

- `TimingScenario`, the synthetic timing loop A1 built to prove the measurement pipeline and marked disposable. `FrameModelCost` now demonstrates the same pipeline against real frame conversions; A1's evidence retains the record.

### Documentation

- Established the initial SolProject product vision, planning gate, engine proposal, GDD, roadmap, and AI-agent workflow.
- Selected **Frontiers of Sol** as the game title and recorded the initial controls, hardware target, 2026 epoch, uncrewed first mission, C++ conventions, and save-compatibility baseline.
- Defined the internal static-library engine boundary, initial flight-guidance capabilities, integrated-graphics investigation tier, Florida launch region, and starting company facilities.
- Fixed the initial UTC epoch, default flight/camera bindings, first orbital environmental survey, and subsystem namespace/Doxygen policy.
- Accepted the P1 Vulkan 1.2 candidate floor, dependency-pinning policy, DE440/UTC/TDB reference boundary, persistence artifact/migration policy, and measurable technical-risk prototype plan.
- Approved the completed P0 planning gate while explicitly keeping implementation unauthorized.
