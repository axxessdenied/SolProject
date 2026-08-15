# SolProject

Simulating humanity expanding into the Solar system.

A single-player 3D space sandbox game (working title: **Sol**) built on a from-scratch C++20 engine (**Sol Engine**), inspired by X4: Foundations, Elite Dangerous, Starsector, EVE Online, Stellaris, and Avorion.

- **Game design**: [docs/gdd.md](docs/gdd.md)
- **Engine architecture & roadmap**: [docs/engine-plan.md](docs/engine-plan.md)
- **Contributor / agent rules**: [AGENTS.md](AGENTS.md)

## Building

Requirements: Windows, Visual Studio 2022+ (MSVC toolset), CMake ≥ 3.28, Ninja, and the [Vulkan SDK](https://vulkan.lunarg.com/) (`VULKAN_SDK` env var set). Run from a *x64 Native Tools* developer prompt (or any shell where `cl` is on PATH):

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The game executable lands at `build/dev/bin/sol.exe`. GPU-dependent tests are excluded from the default preset; run them locally with `ctest --preset dev-gpu`.

## Status

Phase 1 complete (window + Vulkan bootstrap + first triangle, validation-clean) — next up: Phase 2, renderer foundations (math library, asset cooker v0, textured meshes, ImGui). See the [roadmap](docs/engine-plan.md#4-roadmap).
