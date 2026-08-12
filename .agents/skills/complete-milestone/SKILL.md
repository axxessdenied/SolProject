---
name: complete-milestone
description: Verify and close a SolProject milestone against its playable outcome, numerical and persistence evidence, documentation, and handoff requirements. Use when a milestone is believed complete or ready to change status.
---

# Complete a milestone

1. Compare the implementation and evidence with the milestone plan, prerequisites, non-goals, and objective acceptance criteria.
2. Run `$review-cpp-change`, `$review-simulation-change` where applicable, and `$audit-project-workflow`; independent read-only parallel review requires user permission.
3. Have the sole owner resolve findings and rerun focused checks. Validate builds, deterministic scenarios, transitions, persistence, performance budgets, runtime smoke tests, and packaging only where the milestone requires them.
4. Run `$maintain-project-docs`, including project status and any architecture/ADR/GDD/changelog/README triggers.
5. State whether the milestone is implementation-complete, integrated into `dev`, or released; these are different states.
6. Produce the handoff contract. Do not merge, tag, publish, or open the planning gate without explicit instruction.
