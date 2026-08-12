---
name: maintain-project-docs
description: Synchronize SolProject's status, architecture, ADRs, changelog, README, engine plan, GDD, and open-question register after scope, design, implementation, dependency, or milestone changes. Use whenever a change triggers project documentation updates.
---

# Maintain project documentation

1. Read `AGENTS.md`, the change/diff, and `SolProjectNotes/Documentation-Rules.md`.
2. Update only the authoritative documents whose triggers apply. Link to details instead of copying status, schemas, or decisions.
3. Keep `docs/project_status.md` accurate for phase/milestones/blockers/gate; architecture accurate for accepted/implemented technical truth; and the changelog append-only for user-visible history.
4. Update plans/GDD and their Decisions tables when scope or intent changes. Move resolved items out of `Open-Questions.md` into the correct authority.
5. Use Confirmed, Proposed, Open, Deferred, and Implemented honestly. Never upgrade a proposal or partial change to completion without authority/evidence.
6. Cross-check names, links, dates, versions, dependency pins, validation claims, and current repository state. Preserve unrelated author-written material.
