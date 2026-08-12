# ADR 0005 — Engine linkage and extension boundary

**Status:** Accepted

**Date:** 2026-08-12

## Context

SolEngine exists to serve Frontiers of Sol rather than provide a general third-party engine SDK. A shared C++ library would add symbol export, runtime deployment, ownership, allocator, exception, compiler, and ABI-compatibility obligations before the game benefits from them. Modding remains a confirmed goal, but a compiler-specific C++ ABI is a fragile long-term mod contract.

## Decision

- Organize SolEngine as focused project-internal static library targets linked into the Frontiers of Sol executable and relevant tools/tests.
- Do not promise a stable C++ binary ABI and do not provide native mods access to internal C++ headers or object layouts.
- Keep module dependencies explicit and narrow so static linkage does not permit arbitrary cross-layer coupling.
- Prefer data-defined content for ordinary mods.
- Add scripting when content iteration or mod behavior demonstrates a concrete need.
- If native extensions become necessary, define a small versioned C ABI using opaque handles and explicit ownership instead of exposing the engine's C++ ABI.
- Do not add an export macro or shared-library deployment path until a later ADR demonstrates a requirement.

## Alternatives considered

- **Shared SolEngine C++ library:** rejected initially because runtime/ABI complexity has no current player-facing payoff.
- **Header-only engine:** rejected because it increases build coupling and weakens module boundaries.
- **Native C++ plugin ABI:** rejected as the default mod interface because compiler/toolset and object-layout compatibility are difficult to guarantee across releases.
- **One monolithic executable target:** rejected because focused static targets improve dependency enforcement and headless test/tool reuse.

## Consequences

- “Public API” initially means a documented source-level boundary between project modules, not an external binary contract.
- Target boundaries and include visibility must prevent the game from reaching renderer/platform internals directly.
- Static linkage may increase link time and binary size; measure before optimizing.
- A future public SDK, shared library, scripting runtime, or native-plugin layer requires its own ADR and compatibility policy.

## Validation

- The first build graph must demonstrate that game-domain targets depend only on intended SolEngine interfaces.
- Headless tests/tools link only required engine modules and do not pull renderer/platform dependencies accidentally.
- Build output contains no undocumented shared SolEngine runtime dependency.
