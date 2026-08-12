# Documentation Rules

## Sources of truth

- `docs/project_status.md` owns phase, milestone state, blockers, the planning gate, and implementation authorization.
- `docs/architecture.md` and `docs/architecture/` own accepted and implemented technical structure.
- `docs/decisions/` owns accepted cross-cutting technical decisions.
- `docs/changelog.md` owns append-only user-visible history.
- `SolProjectNotes/Engine-Plan.md` owns proposed engine direction and sequencing.
- `SolProjectNotes/Milestones/` owns implementation-ready milestone boundaries, increments, evidence criteria, and handoff contracts.
- `SolProjectNotes/GDD.md` and `SolProjectNotes/GDD/` own game design.
- `SolProjectNotes/Open-Questions.md` owns unresolved product and architecture questions.

Link instead of copying details between these documents.

## Update triggers

Update `docs/project_status.md` when a phase/milestone changes, a blocker or known limitation appears, priorities change, the planning gate changes, or implementation authorization changes.

Update architecture when a subsystem, public contract, data flow, time/reference-frame rule, persistence schema, or dependency changes. During pre-production, clearly label proposals; after implementation, describe only repository truth.

Append the changelog for player-visible features/fixes, breaking changes, dependencies, save/mod format changes, or releases. Never rewrite history.

Update the README only for the high-level product, status, supported platform, top-level structure, build/launch path, or major links.

Update plans/GDD when scope or design intent changes. Every focused plan or GDD note must end with a decisions table that records what was decided, why, status, and date or source.

Update a milestone plan when its outcome, boundaries, prerequisites, increments, numerical gates, evidence requirements, or handoff contract changes. A milestone plan never opens the planning gate, grants implementation authorization, or changes milestone status by itself.

## Truth labels

Use these consistently:

- **Confirmed:** explicitly chosen by the user.
- **Proposed:** recommended but not accepted.
- **Open:** requires a decision.
- **Deferred:** intentionally outside the current planning horizon.
- **Implemented:** exists and has verification evidence.

Never label a design or partial implementation as shipped.
