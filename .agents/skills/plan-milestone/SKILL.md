---
name: plan-milestone
description: Convert an approved SolProject roadmap item into an implementation-ready milestone with boundaries, prerequisites, acceptance evidence, risks, and documentation triggers. Use when scoping a new milestone or revising a milestone plan; do not use to implement it.
---

# Plan a milestone

1. Read `AGENTS.md`, current project status, the engine plan, the relevant GDD notes, accepted ADRs, and prior milestone evidence.
2. State the player/developer outcome, owner, prerequisites, dependencies, explicit non-goals, and affected engine/game boundaries.
3. Divide work into runnable increments. Each increment needs deliverables, objective done criteria, validation commands/scenarios, numerical tolerances where relevant, and documentation triggers.
4. Identify simulation-transition, precision, persistence, content compatibility, performance, and UX risks. Put unknown architectural claims behind a measured prototype or ADR.
5. Include rollback/recovery considerations and the handoff contract. Do not predeclare files/classes that are not needed to explain an accepted boundary.
6. Update roadmap status only if the user has changed priority/state. A milestone plan does not open the planning gate or authorize implementation.
