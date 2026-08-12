# ADR 0006 — Subsystem namespaces and source-API documentation

**Status:** Accepted

**Date:** 2026-08-12

## Context

SolEngine begins as several project-internal static libraries. Those targets need source-level interfaces that are easy for a solo developer and multiple AI agents to discover and review without implying that engine internals form a stable external SDK or C++ binary ABI.

## Decision

- Keep `sol` as the root namespace and give each engine or game subsystem an owned child namespace. Initial examples include `sol::core`, `sol::render`, `sol::platform`, and `sol::assets`; further names are introduced with their owning modules rather than through a speculative global catalogue.
- Keep a module's supported source interface directly in its owned subsystem namespace. Use `sol::<subsystem>::detail` only for implementation that consumers must not depend on.
- Align target, include-path, and namespace ownership where practical. A symbol must have one clear owning subsystem even when its implementation spans multiple source files.
- Require Doxygen documentation for project-owned public source-API types and non-trivial functions. Contracts document purpose plus applicable ownership/lifetime, units and reference frames, valid ranges/preconditions, failure behavior, and threading or clock expectations.
- Document self-evident accessors briefly and document private implementation only when the reason, invariant, or hazard is not clear from code. Do not generate comments that merely repeat an identifier.
- Treat “public” as a supported source boundary between project targets. It does not promise binary stability, external SDK compatibility, or native-mod access.

## Examples

```cpp
namespace sol::render {

/// Camera-relative pose consumed by the render world.
struct RenderPose;

namespace detail {
class FrameAllocator;
} // namespace detail

} // namespace sol::render
```

## Alternatives considered

- **Put all symbols directly in `sol`:** rejected because ownership and collision risk become unclear as the engine and game grow.
- **Deep namespace trees mirroring every directory:** rejected because filesystem organization should not create verbose, unstable API names.
- **Document every symbol equally:** rejected because repetitive comments obscure the contracts that matter.
- **Treat internal headers as an external SDK:** rejected by the focused-engine scope and ADR 0005.

## Consequences

- Each future module plan must name its namespace and intended public headers.
- Cross-module review can reject dependencies on another subsystem's `detail` namespace.
- APIs involving simulation quantities must expose and document units, frames, time domains, and ownership explicitly.
- Doxygen-style source comments are required, but selecting or packaging a documentation generator is deferred until a workflow needs generated reference pages.

## Validation

- The initial build graph and header layout must demonstrate that public headers do not require another subsystem's `detail` headers.
- Code review checks public source interfaces for useful contracts and rejects comment-only restatements.
- Headless targets must be able to consume intended core/game interfaces without renderer or platform implementation leakage.
