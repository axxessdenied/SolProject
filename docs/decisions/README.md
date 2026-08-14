# Architectural Decision Records

Use ADRs for accepted, cross-cutting technical decisions that would otherwise be repeatedly debated or silently assumed. Game-design decisions belong in the relevant GDD note.

Each ADR records status, context, decision, alternatives, consequences, validation, and superseded decisions. Use sequential names such as `0001-cpp-language-standard.md`.

Allowed status values are **Proposed**, **Accepted**, **Superseded**, and **Rejected**. Only the user or an explicitly authorized milestone plan can move a foundation ADR to Accepted during pre-production.

## Index

| ADR | Status | Decision |
|---|---|---|
| [0001](0001-cpp23-msvc-cmake-ninja.md) | Accepted | C++23, MSVC, CMake, and separate Ninja builds |
| [0002](0002-vulkan-graphics-api.md) | Accepted | Vulkan is the SolEngine graphics API. Accepted 2026-08-14 on P1b B1 evidence, on one device, and not on baseline-class or AMD driver behaviour |
| [0003](0003-cpp-naming-conventions.md) | Accepted | `sol` namespace and project-owned C++ naming conventions |
| [0004](0004-persistence-versioning-and-compatibility.md) | Accepted | Version from first persistence; guarantee migrations from public alpha |
| [0005](0005-engine-linkage-and-extension-boundary.md) | Accepted | Internal static engine libraries; no stable C++ ABI for mods |
| [0006](0006-subsystem-namespaces-and-source-api-documentation.md) | Accepted | Owned subsystem namespaces and Doxygen source-API contracts |
| [0007](0007-dependency-acquisition-and-pinning.md) | Accepted | vcpkg manifest mode with a reviewed pinned baseline |
| [0008](0008-astronomical-reference-data-and-time-boundary.md) | Accepted, amended 2026-08-12 | DE440/DE441 reference data, UTC/TDB boundary, and a fixed P1 launch anchor defined against the reference ellipsoid |
| [0009](0009-persistence-artifacts-and-migration-window.md) | Accepted | JSON-facing artifacts, chunked campaign saves, and migration window |
| [0010](0010-determinism-and-floating-point.md) | Accepted | Same-machine bit-exact determinism; `/fp:precise` and `/arch:AVX2` |
| [0011](0011-gravity-and-orbit-baseline.md) | Accepted, amended 2026-08-12 | Patched conics with spheres of influence; no perturbations or decay. Confirmed from P1a evidence; the sphere-of-influence radius clause corrected to state that radii are derived at load rather than stored as fixtures |
| [0012](0012-asset-pipeline.md) | Accepted | Blender authoring, glTF 2.0, procedural geometry, build-time bake |
