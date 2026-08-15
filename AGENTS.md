# AGENTS.md — Rules for AI Agents Working on SolProject

This file governs how AI agents (Claude Code and others) work in this repository. Read it fully before making changes. When these rules conflict with a request, surface the conflict instead of silently picking one.

## 1. What This Project Is

SolProject is a single-player 3D space sandbox game (working title: **Sol**) built on a from-scratch C++20 engine (working name: **Sol Engine**). The player directly pilots a ship (X4 / Elite Dangerous style) in a procedurally generated galaxy with a living, simulated economy and factions.

**Sources of truth** — do not contradict these documents; if a change requires deviating from them, update the document in the same change set and call it out:

- `docs/engine-plan.md` — engine architecture, layering, and phased roadmap
- `docs/gdd.md` — game design: vision, pillars, systems, and scope guardrails

## 2. Build & Verify

- Build system: **CMake ≥ 3.28**, out-of-source builds only, into `build/`.
- Generator: **Ninja** preferred; MSVC (cl) is the primary compiler on Windows. Keep code Clang-clean for the future Linux port.
- Once `CMakePresets.json` exists, always use presets:
  ```
  cmake --preset dev
  cmake --build --preset dev
  ctest --preset dev
  ```
- **Definition of done**: the project configures, builds with zero warnings (warnings are errors), and all tests pass. Run these yourself before declaring work complete. For renderer/gameplay changes with no test coverage, run the app and state what you observed.
- Never commit anything under `build/` or other generated output.

## 3. Language & Style

- **C++20.** Use concepts, `constexpr`, designated initializers, `std::span`, ranges where they clarify. No modules for now (tooling maturity). Assume an experienced C++ reader — no tutorial comments.
- Formatting and naming are enforced by `.clang-format` and `.editorconfig`. Run clang-format on files you touch. Do not hand-format against it.
- Naming conventions (mirrors `.clang-format` expectations):
  - Types / concepts: `PascalCase`
  - Functions / methods: `camelCase`
  - Variables / parameters: `camelCase`; private members: `m_camelCase`
  - Constants / enum values: `kPascalCase`; macros (rare): `SOL_SCREAMING_CASE`
  - Namespaces: `sol::<layer>` (e.g. `sol::core`, `sol::rhi`, `sol::sim`)
  - Files: `snake_case.hpp` / `snake_case.cpp`
- **Error handling**: exceptions are disabled engine-wide. Use `std::expected`/error codes for recoverable errors and `SOL_ASSERT` / fatal log for programmer errors. No `throw`.
- **Memory**: RAII everywhere. No raw `new`/`delete` outside allocator internals. Ownership via `std::unique_ptr` or engine allocators; raw pointers/references are non-owning views only.
- Headers: `#pragma once`, include-what-you-use, forward-declare where practical. Includes ordered: matching header, same-module, other engine modules, third-party, std.
- `const`/`constexpr` by default; `[[nodiscard]]` on anything returning a status or handle.
- No RTTI (`dynamic_cast`/`typeid`) in engine code; warnings-as-errors on all targets.

## 4. Architecture Rules

The engine is strictly layered (full definitions in `docs/engine-plan.md`):

```
game (C++ glue + Lua scripts)
  └── engine: ui → scripting → sim → ecs → assets → renderer → rhi
        └── core (math, memory, jobs, log, events)
              └── platform (window, input, fs, time)
```

- A module may depend only on modules **below** it. No upward or sideways includes; no cycles.
- **Platform-specific code lives only in the platform layer**, behind portable interfaces. No `#include <windows.h>` or `#ifdef _WIN32` anywhere else. Windows is the first target, but nothing may lock us to it.
- **Engine code never includes game code.** The engine is a library; the game is its client.
- Rendering talks to Vulkan **only through the RHI**. No raw `vk*` calls above `sol::rhi`.
- Game logic (missions, faction decisions, economy tuning, UI flow) belongs in **Lua**; C++ is for the engine, performance-critical simulation, and the binding layer. When in doubt which side logic belongs on, check `docs/engine-plan.md` §Scripting, or ask.
- Simulation runs on a fixed timestep, decoupled from rendering. Never read wall-clock time in sim code; use the sim clock.
- Large-world rules are non-negotiable: positions in simulation are 64-bit (`double`/fixed-point per plan), rendering is camera-relative. Never put absolute world positions in `float`.

## 5. Dependency Policy

This is a from-scratch engine. The **only** approved third-party dependencies are:

| Dependency | Scope |
|---|---|
| Vulkan SDK / headers | Graphics API (runtime) |
| glslang (or shaderc) | GLSL → SPIR-V, **build-time only** |
| Lua 5.4 (vendored source) | Scripting runtime |
| Dear ImGui (vendored) | **Dev/debug tooling only** — never shipping game UI |

Everything else — math, ECS, containers/allocators, asset formats, image/mesh importing in the cooker, platform abstraction — is written in this repo. **Adding, upgrading, or expanding the scope of any dependency requires explicit approval from the user first.** Do not add a library "temporarily," and do not copy-paste library source into the tree as a workaround.

Vendored third-party code lives in `third_party/`, is never modified in place, and is excluded from formatting/linting.

## 6. Git Workflow

- Small, focused commits; imperative mood subject lines ≤ 72 chars (e.g. `Add swapchain recreation on resize`), body explains *why* when non-obvious.
- Multi-commit work happens on a feature branch (`feat/<topic>`, `fix/<topic>`), not `main`.
- Never commit or push unless the user asks. Never force-push. Never rewrite published history.
- Never commit: build output, IDE state, cooked assets, secrets, large binaries (source assets get a policy decision — ask before adding files > 1 MB).

## 7. Testing

- Unit tests are **required** for: `core` (math especially — every math routine gets tests), ECS, serialization, procedural generation (seed determinism), and economy/sim logic. Wired into CTest.
- The test framework is in-repo (a minimal harness, per the dependency policy) — see `engine/test/` once it exists.
- Renderer and platform layers are verified by running the app / sample scenes; say what you ran and saw.
- Procgen and simulation must be deterministic for a given seed — tests should assert this.

## 8. Documentation Maintenance

- Completing a roadmap phase, or changing an architectural/design decision, requires updating `docs/engine-plan.md` or `docs/gdd.md` in the same change set.
- Record significant decisions (chosen alternatives, rejected options, why) in `docs/decisions/` as short numbered notes (`NNN-title.md`) when they arise.
- Keep this file current: if a rule here becomes wrong or obsolete, propose the edit rather than ignoring the rule.

## 9. Agent Conduct

- Prefer editing existing files over creating new ones; follow existing patterns in neighboring code.
- Don't speculate about engine behavior — read the code, build it, run it.
- If a task is ambiguous about scope (engine vs. game, C++ vs. Lua, which layer), ask before writing code.
- Leave the tree buildable. If you must land something incomplete, gate it so `main` still builds and tests pass.
