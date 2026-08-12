---
name: finish-change
description: Finish a SolProject implementation or documentation change with focused review, simulation checks, documentation decisions, validation, and a structured single-writer handoff. Use when work is complete but needs final quality checks.
---

# Finish a change

1. Confirm the goal, sole owner, branch/base, changed files, accepted plan/ADR, and intended completion state.
2. Run `$review-cpp-change` and `$audit-project-workflow`; also run `$review-simulation-change` when results depend on units, time, deterministic state, or simulation transitions. Parallel read-only review requires user permission.
3. Let only the owner apply fixes, then repeat focused checks.
4. Run `$maintain-project-docs` conditionally and validate against milestone criteria, including build/test/scenario/smoke/performance evidence appropriate to risk.
5. Return the handoff: goal, owner, branch/base, changed files, exact validation/results, remaining risks, and next action.
6. Do not commit, push, merge, tag, open a PR, or alter the planning gate without explicit user direction.
