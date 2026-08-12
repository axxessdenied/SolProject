# SolProject Repository Contract

This is the canonical instruction source for every AI agent in this repository. Provider overlays and project skills may add workflow detail but must not contradict this file.

## Project and current gate

- SolProject contains SolEngine and **Frontiers of Sol**, a single-player 3D spaceflight and Solar System expansion game.
- The game starts with a small private spaceflight company. Its priority order is spacecraft design, direct flight, scientific exploration, technology, economy, politics, fleets, then colonies.
- Treat planning-gate approval and implementation authorization as separate controls. Do not create C++ implementation, CMake targets, runtime assets, dependency declarations, or implementation branches unless `docs/project_status.md` marks the planning gate **Approved** and implementation authorization **Granted**. Planning documents, agent workflow files, read-only research, and disposable experiments explicitly approved by the user are allowed within their stated scope.
- Treat FactoryProject as reference-only. Do not copy its source, dependencies, product assumptions, or user-specific permissions.
- Read `docs/project_status.md`, `SolProjectNotes/Engine-Plan.md`, the GDD index, and the relevant focused note before changing scope.

## Sources of truth and routing

- `docs/project_status.md`: current phase, milestone state, blockers, planning-gate state, and implementation authorization.
- `docs/architecture.md` and `docs/architecture/`: implemented architecture and accepted technical contracts. Clearly label proposed architecture during pre-production.
- `docs/decisions/`: accepted architectural decision records (ADRs).
- `docs/changelog.md`: append-only user-visible project history after implementation begins.
- `SolProjectNotes/`: plans, design specifications, research, and open questions.
- `SolProjectNotes/Milestones/`: implementation-ready milestone scope, increments, acceptance evidence, and handoff requirements; these plans do not authorize implementation.
- After implementation begins, route generic runtime code to `engine/`, game-specific simulation and content to `game/`, optional authoring tools to `editor/`, tests to `tests/`, and build logic to `cmake/` or explicit CMake files.
- Do not duplicate milestone status, design rules, or technical schemas across documents. Link to the authoritative document.

## Ownership and agents

- Retain a single-writer rule: exactly one branch/worktree owner applies changes for a task. Other agents provide read-only research, planning, or review unless the user explicitly transfers ownership.
- Claude and Codex are peer feature owners. Antigravity may research, brainstorm, plan, digest, and review; delegated Antigravity work must not autonomously edit core C++ or adopt branch ownership.
- Specialists report findings to the owner. They do not edit the same files concurrently.
- Before a handoff, record the goal, owner, branch and base, changed files, validation performed, remaining risks, and next action.
- Do not use multi-agent work unless the user has requested it and the work can be divided without creating multiple writers.

## Design and architecture discipline

- Build a game-specific engine through playable vertical slices. Every engine milestone must enable a measurable player-facing or developer-facing capability.
- Preserve a seamless player experience from planetary surface to space, but permit reference-frame changes, simulation levels of detail, camera-relative rendering, and background analytical propagation behind the scenes.
- Keep generic engine services separate from game-domain systems. Orbital mechanics, spacecraft parts, science, economy, people, factions, and colonies are game-domain code unless an accepted ADR says otherwise.
- Record meaningful technical decisions in an ADR. Record game-design decisions and their rationale in the relevant GDD note.
- Do not select a dependency before its owning milestone and an accepted dependency/ADR review.

## Simulation integrity

- Use explicit units and coordinate frames. Prefer SI units for simulation truth; conversions belong at input, presentation, or data boundaries.
- Distinguish render time, wall time, fixed-step/local physics time, and campaign/orbital time. Time warp must not silently change outcomes beyond documented tolerances.
- Use the UTC/TDB and DE440 reference-data boundary in ADR 0008 for astronomical fixtures. Every reference vector and timestamp must identify units, frame, origin, epoch, time scale, and provenance.
- Treat transitions between local physics, analytical propagation, and aggregate simulation as testable contracts. Preserve identity, mass/resources, chronology, and state within declared tolerances.
- Favor deterministic, headless scenario tests for orbital, resource, economy, and persistence logic. Never hide numerical error behind display rounding.
- Difficulty settings may add assists or simplify exposed constraints, but should share the same authoritative state model wherever practical.

## C++, dependencies, and verification

- Use the canonical C++ namespace `sol`. Use PascalCase for C++ filenames and types, camelCase for functions/locals/parameters, an `m_` prefix with camelCase for private data members, `kPascalCase` for constants, and SCREAMING_SNAKE_CASE only for macros. ADR 0003 owns these conventions.
- Put source-level module interfaces in an owned subsystem namespace such as `sol::core`, `sol::render`, `sol::platform`, or `sol::assets`; reserve `sol::<subsystem>::detail` for non-public implementation. Document public source APIs with Doxygen as specified by ADR 0006. These interfaces are project-internal and do not create a stable C++ ABI.
- C++23, MSVC, CMake, and separate single-configuration Ninja presets are accepted in ADR 0001. Vulkan 1.2 is the accepted candidate floor for P1, with capabilities queried per device; Vulkan remains a proposed production decision pending ADR 0002's renderer evidence.
- SolEngine begins as project-internal static libraries linked into Frontiers of Sol and test/tool executables. Do not promise a stable C++ binary ABI or expose internals for native mods; future extension uses data, scripting, or a deliberately versioned C interface (ADR 0005).
- Physics libraries, ECS/data model, and concrete serialization/packaging libraries remain open until their owning milestones accept them.
- Use vcpkg manifest mode and a reviewed `builtin-baseline` as the default dependency acquisition policy (ADR 0007). Do not create a manifest or add a package until implementation authorization is granted and its owning milestone accepts the need.
- Once selected, pin dependency versions and record licenses and rationale. Keep third-party APIs behind narrow SolEngine boundaries where practical.
- Prefer explicit CMake source lists; do not use `file(GLOB ...)`, floating branches, unreviewed vendored copies, or submodules without an accepted ADR.
- Use `#pragma once`, RAII, clear ownership, fixed-width types at persistence/network boundaries, named constants, and documentation for public source APIs. Do not add a shared-library export macro unless a later accepted ADR introduces a dynamic binary boundary.
- Follow ADRs 0004 and 0009 for persistent versions, artifact families, transactional writes, and migration guarantees. Do not let a serializer's object model define game-domain ownership or identities.
- Validate proportionally to risk and report commands and results exactly. Simulation changes require invariant/scenario tests in addition to ordinary unit tests.

## Documentation and Git

- Follow `SolProjectNotes/Documentation-Rules.md` for update triggers.
- Branch from `dev` after that integration branch is created; use `feature/*`, `fix/*`, `refactor/*`, `docs/*`, or `chore/*`. Never commit directly to `main`.
- Do not create branches, commit, push, merge, tag, or open a pull request unless the user explicitly requests it. Preserve unrelated work.
- Review correctness, numerical stability, reference-frame/time-warp transitions, save compatibility, API/ABI impact, dependency/build impact, documentation drift, and validation evidence before style-only concerns.
