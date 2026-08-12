---
name: audit-project-workflow
description: Audit SolProject planning-gate compliance, scope, ownership, Git safety, CMake/dependencies, documentation truth, and validation evidence. Use for repository hygiene, workflow review, pre-handoff audits, or suspected scope/documentation drift.
---

# Audit project workflow

1. Read `AGENTS.md`, project status, task scope, branch/base, status/diff, and relevant plans/ADRs.
2. Use `workflow_auditor` for an independent read-only pass when the user permits subagents.
3. Check the planning gate, separate implementation authorization, single-writer ownership, unrelated dirty-work preservation, scope boundaries, accepted-decision traceability, explicit CMake/dependency policy when applicable, and documentation triggers.
4. Verify that claims distinguish proposed, confirmed, deferred, implemented, tested, merged, and released states.
5. Match every validation claim to an exact command/result and every milestone completion claim to objective evidence.
6. Report blockers, risks, drift, and exact next actions with file references. Do not mutate the repository.
