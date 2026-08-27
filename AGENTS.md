# AGENTS.md — Rules for AI Agents Working on SolProject

This file governs how AI agents (Claude Code and others) work in this repository. Read it fully before making changes. When these rules conflict with a request, surface the conflict instead of silently picking one.

## 1. What This Project Is

SolProject is a single-player 3D space sandbox game — **The Stars Don't Wait** (*Carve out a life in a galaxy already in motion.*) — built on a from-scratch C++20 engine (working name: **Sol Engine**). The player directly pilots a ship (X4 / Elite Dangerous style) in a procedurally generated galaxy with a living, simulated economy and factions.

**Sources of truth** — do not contradict these documents; if a change requires deviating from them, update the document in the same change set and call it out:

- `docs/engine-plan.md` — engine architecture, layering, and phased roadmap
- `docs/gdd.md` — game design: vision, pillars, systems, and scope guardrails

## 2. Build & Verify

- Build system: **CMake ≥ 3.28**, out-of-source builds only, into `build/`.
- Generator: **Ninja** preferred; MSVC (cl) is the primary compiler on Windows, GCC on Linux. **The Linux port landed in Phase 21** — `linux-dev` / `linux-dev-gpu` presets, guarded by `hostSystemName == Linux` so they are invisible to the Windows CMake. A clean MSVC build is not evidence of a clean GCC build, or the reverse: the first Linux build produced five classes of diagnostic `/W4 /WX` had never mentioned.
- Once `CMakePresets.json` exists, always use presets:
  ```
  cmake --preset dev
  cmake --build --preset dev
  ctest --preset dev
  ```
- **Definition of done**: the project configures, builds with zero warnings (warnings are errors), and all tests pass **locally**. Run these yourself before declaring work complete. For renderer/gameplay changes with no test coverage, run the app and state what you observed.
- **CI is advisory, never a gate.** Do not wait on GitHub Actions results to declare work done, merge, or push — local verification is authoritative. (Actions on this repo may be unavailable due to account limits.)
- Never commit anything under `build/` or other generated output.

## 3. Language & Style

- **C++20.** Use concepts, `constexpr`, designated initializers, `std::span`, ranges where they clarify. No modules for now (tooling maturity). Assume an experienced C++ reader — no tutorial comments.
- Formatting and naming are enforced by `.clang-format` and `.editorconfig`. Run clang-format on files you touch. Do not hand-format against it.
- **The whole first-party tree is clang-format clean and must stay that way** (`engine/`, `game/`, `tools/` — every `.cpp`/`.hpp`; `third_party/` is excluded per §5). It drifted for many phases and was brought back in one pass; a `git diff` that reformats code your change never touched means someone skipped this step, not that the config is wrong.
- **Formatted with clang-format 22.1.3** (the VS 18 toolchain: `.../Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-format.exe`). Older versions differ — 19.1.5 disagrees on array-reference parameter spacing (`const float (&v)[3]`) — so **use the VS 18 binary, not whatever is first on `PATH`**, or you will re-churn a file per pass.
- **`IncludeBlocks: Regroup` sorts across blank lines, so a blank line does NOT protect include order.** Where order is load-bearing — the three `win32/` files where `<windows.h>` must precede `<audioclient.h>`, `<vulkan/vulkan_win32.h>` or `<imgui_impl_win32.h>` — the block is wrapped in `// clang-format off` / `// clang-format on` with a comment saying why. **Getting this wrong fails inside an SDK header and names no file of ours**, which is why the guard is there rather than a convention.
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
        └── platform (window, input, fs, time)
              └── core (math, memory, jobs, log, events)
```

- A module may depend only on modules **below** it. No upward or sideways includes; no cycles.
- **Platform-specific code lives only in the platform layer**, behind portable interfaces. No `#include <windows.h>` or `#ifdef _WIN32` anywhere else, with two sanctioned exceptions: native Vulkan surface creation in `engine/rhi/src/<os>/` and the Dear ImGui platform-backend bridge in `engine/ui/src/<os>/`. **Both now have a `win32/` and a `linux/` half** (Phase 21), and the rule is the directory: an OS `#ifdef` outside a `src/<os>/` directory is a defect, and there are currently zero.
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
| glslang (or shaderc) | GLSL → SPIR-V, **build-time only** — and it stays that way now that mods can carry shaders: a mod ships `.spv`, and no compiler is distributed (`docs/decisions/011-mod-shaders-spirv.md`) |
| Lua 5.4 (vendored source) | Scripting runtime |
| Dear ImGui (vendored) | **Dev/debug tooling only** — never shipping game UI. ⚑ The Forge itself IS now distributed (`docs/decisions/010-forge-ships.md`), so ImGui reaches end users inside a tool; the rule this row protects is that **the GAME's UI is first-party**, and that is unchanged. |
| stb_vorbis (vendored) | Ogg Vorbis decode, **cooker only** — the shipping binary carries no decoder (`docs/decisions/009-audio-decoder.md`) |

Everything else — math, ECS, containers/allocators, asset formats, image/mesh importing in the cooker, platform abstraction — is written in this repo. **Adding, upgrading, or expanding the scope of any dependency requires explicit approval from the user first.** Do not add a library "temporarily," and do not copy-paste library source into the tree as a workaround.

Vendored third-party code lives in `third_party/`, is never modified in place, and is excluded from formatting/linting.

**System libraries are not dependencies under this section, and Phase 21 leaned on that standing rather than widening the table.** `ole32` (WASAPI) on Windows, and `libwayland-client`, `libwayland-cursor`, `libxkbcommon` and `wayland-protocols`/`wayland-scanner` on Linux, have the same standing this project already gives the Vulkan loader: they ship with the OS, no source enters the tree, and `third_party/` gains nothing. ⚑ The generated `wayland-scanner` output is machine-written C and is **deliberately not linked to `sol_options`** — it holds third_party's standing, and `-Werror` over it would let a `wayland-protocols` upgrade break the build for reasons no first-party file could be edited to fix. **SDL3 was offered as an explicit change to this table and declined; do not re-litigate it.**

## 6. Git Workflow

Branching model:

- **`main` is releases only.** It never receives direct commits or feature merges; it only advances when the user explicitly asks to cut a release from `dev`.
- **`dev` is the integration branch** (and the GitHub default). All work lands here.
- **All development happens on feature branches** cut from `dev` (`feat/<topic>`, `fix/<topic>`, `chore/<topic>`) and merges back into `dev` once locally verified (build + tests green). Do not commit directly to `dev` or `main`.

Commit conventions:

- Small, focused commits; imperative mood subject lines ≤ 72 chars (e.g. `Add swapchain recreation on resize`), body explains *why* when non-obvious.
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
