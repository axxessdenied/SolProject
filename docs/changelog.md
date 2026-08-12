# Changelog

All notable user-visible changes will be documented here after implementation begins. This file is append-only and will follow Keep a Changelog once the versioning policy is accepted.

## [Unreleased]

### Added

- Implementation began. P1a increment A1 delivers the first CMake/Ninja build graph, Debug and Release presets, and the disposable headless prototypes under `prototypes/p1a/`, including a durable measurement harness that records provenance, timing distributions, peak memory, and allocation counts with every result.
- A determinism harness that proves bit-identical output across separate runs of the same build, and that is itself proven to detect a floating-point flag change.

### Changed

- Recorded the literal MSVC flag spellings in ADR 0010 after measuring them: `/std:c++23` is unavailable on this toolset, and no flag disables FMA contraction. `/fp:precise` disables it by default, verified by measurement rather than assumed.

### Documentation

- Established the initial SolProject product vision, planning gate, engine proposal, GDD, roadmap, and AI-agent workflow.
- Selected **Frontiers of Sol** as the game title and recorded the initial controls, hardware target, 2026 epoch, uncrewed first mission, C++ conventions, and save-compatibility baseline.
- Defined the internal static-library engine boundary, initial flight-guidance capabilities, integrated-graphics investigation tier, Florida launch region, and starting company facilities.
- Fixed the initial UTC epoch, default flight/camera bindings, first orbital environmental survey, and subsystem namespace/Doxygen policy.
- Accepted the P1 Vulkan 1.2 candidate floor, dependency-pinning policy, DE440/UTC/TDB reference boundary, persistence artifact/migration policy, and measurable technical-risk prototype plan.
- Approved the completed P0 planning gate while explicitly keeping implementation unauthorized.
