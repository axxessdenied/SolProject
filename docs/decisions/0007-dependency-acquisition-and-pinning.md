# ADR 0007 — Dependency acquisition and pinning

**Status:** Accepted

**Date:** 2026-08-12

## Context

SolProject will use focused third-party libraries for commodity capabilities, but selecting the whole stack before its owning milestones would increase build, licensing, replacement, and maintenance risk. Windows x64, MSVC, CMake, Ninja, a solo developer, and multiple AI agents also require one reproducible acquisition path.

## Decision

- Use vcpkg manifest mode as the default acquisition mechanism for C/C++ libraries.
- Check in the top-level manifest and a `builtin-baseline` commit when the first dependency is approved. Record the tested vcpkg tool revision or bootstrap method separately so the registry baseline is not mistaken for the package-manager version.
- Add a dependency only for an accepted requirement in its owning milestone. Each addition or upgrade must record rationale, alternatives, license, direct version requirement where needed, baseline revision, features, target boundary, and validation evidence.
- Let the checked-in baseline provide the default resolved version set. Use minimum-version constraints only for demonstrated requirements and overrides only for reviewed exceptional pins.
- Keep third-party APIs behind narrow project-owned boundaries where practical. Public SolEngine interfaces must not expose a dependency merely for convenience.
- Do not use floating Git branches, implicit global packages, unreviewed source copies, or CMake `FetchContent` as a parallel default.
- If vcpkg cannot provide an acceptable package, require a dependency-specific ADR before using a pinned upstream archive, a small reviewed vendored copy, or another acquisition mechanism. Submodules remain disallowed without explicit acceptance.
- Treat platform SDKs and development tools that are not vcpkg packages as pinned toolchain inputs with documented installation and version checks.

## Consequences

- This ADR selects the acquisition policy, not any runtime library.
- `vcpkg.json` and other dependency declarations remain forbidden until implementation authorization is granted and an owning milestone accepts its first dependency.
- Updating the vcpkg baseline is a reviewed dependency change, not routine formatting or unattended maintenance.
- Binary caching may improve local/CI iteration later, but cache configuration must not become the only reproducible source of packages.

## Validation

- The first dependency change must configure and build from a clean tree using only the documented toolchain/bootstrap path and checked-in manifests.
- Debug and Release presets must resolve the same reviewed dependency graph unless a documented configuration-specific feature requires otherwise.
- Dependency review verifies licenses, resolved versions, features, target leakage, and clean rebuild behavior.

## Sources

- Microsoft documents manifest mode, registries, and baseline-based version resolution in the [vcpkg overview](https://learn.microsoft.com/en-us/vcpkg/get_started/overview).
- The [vcpkg versioning reference](https://learn.microsoft.com/en-gb/vcpkg/users/versioning) defines `builtin-baseline`, minimum-version constraints, and overrides.
