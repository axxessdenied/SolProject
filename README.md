# SolProject

SolProject is the planning and development repository for **SolEngine** and
**Frontiers of Sol**, a single-player 3D spaceflight and Solar System expansion game.

The game begins with a small private spaceflight company. Players design and
fly spacecraft across a real-scale Solar System, conduct scientific exploration,
unlock technology, and grow into a system-spanning industrial and political power.

## Current status

Pre-production planning is complete and the planning gate is approved. The plan was
reviewed and revised on 2026-08-12; implementation has not started and remains
explicitly unauthorized. See
[project status](docs/project_status.md), the [engine plan](SolProjectNotes/Engine-Plan.md),
and the [game design document](SolProjectNotes/GDD.md). The next planned stage is
documented in the
[P1a precision and orbit plan](SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md),
followed by
[P1b renderer and craft](SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md).

## Project boundaries

- Engine: SolEngine, purpose-built for this game while retaining clean module boundaries.
- Platform: Windows x64 first, targeting 60 FPS at 1080p on the baseline discrete-GPU PC and scalable higher-quality options. Selected integrated graphics are a 30 FPS/720p-low investigation tier, not yet a support promise.
- Mode: single-player.
- Language/toolchain: C++23 in namespace `sol`, MSVC, CMake, and Ninja.
- Graphics: Vulkan 1.2 is the P1 candidate floor; production adoption remains pending prototype evidence. UHD 630/Vega 8-class graphics form a 30 FPS/720p-low investigation tier.
- Orbital model: patched conics with spheres of influence. No perturbations, drag, or orbital decay in the propagation; aerodynamic forces still act on craft inside the atmosphere.
- Determinism: bit-exact on the same build and machine, tolerance-based across machines. `/arch:AVX2` sets a hard CPU floor at Haswell-era Intel and Zen-era AMD.
- Assets: authored in Blender, interchanged as glTF 2.0, generated procedurally where parametric, baked at build time. Binary sources will use Git LFS.
- Reference project: FactoryProject supplies workflow ideas only. SolProject does not copy its code or assumptions.

No build instructions exist yet because implementation has not started. Git LFS must be
configured before the first binary asset is committed.
