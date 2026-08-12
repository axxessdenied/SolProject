# SolProject

SolProject is the planning and development repository for **SolEngine** and
**Frontiers of Sol**, a single-player 3D spaceflight and Solar System expansion game.

The game begins with a small private spaceflight company. Players design and
fly spacecraft across a real-scale Solar System, conduct scientific exploration,
unlock technology, and grow into a system-spanning industrial and political power.

## Current status

Pre-production planning is complete and the planning gate is approved. Implementation
has not started and remains explicitly unauthorized. See
[project status](docs/project_status.md), the [engine plan](SolProjectNotes/Engine-Plan.md),
and the [game design document](SolProjectNotes/GDD.md). The next planned stage is
documented in the
[P1 technical-risk prototype plan](SolProjectNotes/Milestones/P1-Technical-Risk-Prototypes.md).

## Project boundaries

- Engine: SolEngine, purpose-built for this game while retaining clean module boundaries.
- Platform: Windows x64 first, targeting 60 FPS at 1080p on the baseline discrete-GPU PC and scalable higher-quality options. Selected integrated graphics are a 30 FPS/720p-low investigation tier, not yet a support promise.
- Mode: single-player.
- Language/toolchain: C++23 in namespace `sol`, MSVC, CMake, and Ninja.
- Graphics: Vulkan 1.2 is the P1 candidate floor; production adoption remains pending prototype evidence. UHD 630/Vega 8-class graphics form a 30 FPS/720p-low investigation tier.
- Reference project: FactoryProject supplies workflow ideas only. SolProject does not copy its code or assumptions.

No build instructions exist yet because implementation has not started.
