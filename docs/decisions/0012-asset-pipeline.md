# ADR 0012 — Asset authoring and pipeline

**Status:** Accepted

**Date:** 2026-08-12

## Context

No planning document addressed where 3D assets come from. For a solo developer this is a top-tier schedule risk: the first playable needs a curated set of fixed functional parts covering command/control, engines, decoupling, power, communications, and science, plus procedural tanks and structural pieces, plus a launch facility, plus terrain and atmosphere presentation for one high-detail region.

The pipeline also has architectural consequences that are cheap to decide now and expensive later: interchange format, unit and axis convention, material model, attachment-node representation, level-of-detail convention, texture compression, and whether assets are processed at build time or load time. GDD note 03 already commits to procedural tanks and structural pieces, which makes procedural geometry an engine capability rather than only a content strategy.

## Decision

### Authoring

- **Author meshes in Blender.** No purchased asset packs and no commissioned art for the first playable. The curated part set is small, and consistent style from a single author is worth more than coverage.
- **Generate procedurally wherever the shape is parametric.** Tanks, structural elements, adapters, fairings, and trusses are generated from parameters rather than authored as fixed meshes. This is the primary lever that keeps the art bill affordable and is already the committed construction design.
- Hand-authored meshes are reserved for shapes that carry identity: engines, command modules, science instruments, decouplers, and the launch facility.

### Interchange and conventions

- **glTF 2.0** is the interchange format from Blender into the pipeline.
- **Metallic-roughness PBR** is the material model. Materials that need engine-specific behavior carry it in named extras rather than in a bespoke exporter.
- **Units are metres.** The engine's authoritative frame convention is right-handed, Z-up. The importer converts from glTF's Y-up convention explicitly at the boundary and records the conversion; no mesh may rely on an implicit axis swap.
- **Attachment nodes are exported as named empties** following a documented naming convention, so part topology comes from the authored asset rather than a hand-maintained side table.
- **Level of detail is explicit.** LOD meshes are authored or generated as named siblings under a documented convention; the importer does not infer them.

### Processing

- **Assets are processed at build time, not load time.** A bake step converts glTF plus textures into an engine-ready runtime format with BCn-compressed textures, generated mipmaps, and validated attachment metadata.
- The bake step is a project-owned tool target invoked by the build, with its inputs and outputs explicitly listed. It follows ADR 0001's prohibition on `file(GLOB ...)`.
- The runtime loads only baked assets. Loading raw glTF at runtime is a development convenience that must never become the shipping path.
- Bake failures are hard errors with actionable diagnostics, consistent with the content-validation principle in `docs/architecture.md`.

### Repository storage

- Source `.blend` files and source textures are checked in.
- Baked runtime assets are build output and are **not** checked in.
- Binary asset storage uses **Git LFS**, configured before the first `.blend` file lands. Retrofitting LFS after binaries are in history requires a history rewrite, which is why this is decided now rather than at M3.

## Alternatives considered

- **Purchased asset packs:** fast coverage, but rejected for the first playable. Style consistency work, retopology for the performance baseline, and license review across packs would consume much of the time saved, and part meshes need attachment-node metadata that packs do not carry.
- **Commissioned art:** rejected for the first playable on budget and schedule-dependency grounds. It remains the obvious option for a later visual pass once the part set has stopped changing.
- **FBX or a custom exporter:** rejected. glTF 2.0 is an open standard with a maintained Blender exporter and a clear PBR material model; a custom exporter is maintenance the project does not need.
- **Runtime asset processing:** rejected. It moves compression and validation cost to every launch, hides content errors until load, and makes the shipping path differ from the tested one.
- **USD:** richer and increasingly standard for pipelines, but heavier than a solo project with a small curated part set requires.

## Consequences

- Procedural geometry generation becomes a **P2 engine deliverable**, owned by M3 alongside construction. It is not merely a content decision, and M3's scope must account for it.
- The asset bake tool is an additional CMake target and an additional thing to keep working. It should stay small.
- Blender version and the glTF exporter version become pinned toolchain inputs under ADR 0007's rule for non-vcpkg tools, recorded with installation instructions.
- Git LFS becomes a required part of repository setup and must be documented in the README before the first binary asset is committed.
- Art throughput is bounded by one person. If the part set grows beyond what the schedule supports, the correct response is fewer distinct parts with more procedural variation — not a late pivot to purchased packs with incompatible conventions.
- P1a requires no assets at all. P1b's renderer needs only placeholder geometry and can proceed before this pipeline is built, which is why this ADR does not block P1.

## Validation

- The first baked asset must round-trip: authored in Blender, exported to glTF, baked, loaded at runtime, and rendered with correct scale, orientation, and material response against a reference of known dimensions.
- A part with attachment nodes must import with its nodes at the authored positions, verified numerically rather than visually.
- A deliberately malformed asset must fail the bake with an actionable diagnostic and must not produce a runtime asset.
- Git LFS must be verified to be tracking the intended patterns before the first `.blend` file is committed.

## Sources

- The Khronos Group publishes the [glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html), including the metallic-roughness material model and the Y-up coordinate convention.
- Blender documents its bundled [glTF 2.0 importer and exporter](https://docs.blender.org/manual/en/latest/addons/import_export/scene_gltf2.html), including axis conversion and custom-property export.
- Microsoft documents [BCn texture compression formats](https://learn.microsoft.com/en-us/windows/win32/direct3d11/texture-block-compression-in-direct3d-11) and their tradeoffs.
- GitHub documents [Git Large File Storage](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-git-large-file-storage) and the cost of retrofitting it onto existing history.
