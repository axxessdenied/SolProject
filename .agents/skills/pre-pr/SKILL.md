---
name: pre-pr
description: Prepare a SolProject branch for pull-request review through read-only review, sequential owner fixes, documentation checks, and final validation. Use when asked for pre-PR readiness; it does not create or publish a PR.
---

# Prepare for a pull request

1. Confirm the branch targets `dev`, inspect status/diff/base, identify the sole owner, and preserve unrelated work.
2. Run `$review-cpp-change` plus `$review-simulation-change` where applicable and `$audit-project-workflow`. Parallel read-only review requires user permission.
3. Let only the owner apply fixes; repeat focused reviews after edits.
4. Run `$maintain-project-docs` and all accepted build/test/scenario/smoke/performance checks. Ensure dependency/license and save/mod compatibility effects are documented.
5. Summarize readiness, findings resolved, exact validation, unresolved risks, and the handoff contract.
6. PR creation, commits, pushes, merges, and tags remain user-directed.
