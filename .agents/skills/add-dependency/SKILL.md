---
name: add-dependency
description: Add, remove, or upgrade a SolProject C++ dependency with an accepted need, pinned acquisition, licensing review, narrow boundaries, synchronized documentation, and verification. Use whenever a third-party library or version is proposed or changed.
---

# Manage a dependency

1. Read `AGENTS.md`, the owning milestone, dependency policy/ADR, and affected architecture. Stop if the planning gate is closed, implementation authorization is not granted, or the need is unapproved.
2. Verify current C++/MSVC/Windows support, maintenance, license/distribution terms, and stable releases from primary sources. Compare credible alternatives and a small in-house boundary where realistic.
3. Record why the dependency is needed now, its pinned tag/commit and integrity source, transitive/build impact, replacement cost, and whether its types leak through public APIs.
4. Add it through the accepted CMake mechanism, link only the narrowest target, keep source lists explicit, and isolate third-party APIs where practical.
5. Update the dependency ADR/history, architecture, README tech stack, changelog, notices/licenses, and lock/pin records when applicable.
6. Clean-configure, build, test, and run targeted smoke checks. Report warnings and the exact coverage honestly.
