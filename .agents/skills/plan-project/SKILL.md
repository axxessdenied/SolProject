---
name: plan-project
description: Develop, challenge, or revise SolProject's engine plan, GDD, product scope, architecture proposals, roadmap, open questions, or planning gate. Use for pre-production planning, game-design changes, architecture proposals, roadmap changes, and major scope decisions before implementation.
---

# Plan the project

1. Read `AGENTS.md`, `docs/project_status.md`, `SolProjectNotes/Engine-Plan.md`, `SolProjectNotes/GDD.md`, `SolProjectNotes/Open-Questions.md`, and each focused note affected by the request.
2. Separate user-confirmed facts, proposals, open questions, deferred scope, and implementation truth. Do not turn a proposal into an accepted decision silently.
3. Trace every recommendation to the product priority order and the first-playable loop. Prefer playable vertical slices and measurable risk reduction over general engine breadth.
4. Check engine/game boundaries, prerequisites, seamless-scale and time-warp risks, persistence/modding consequences, solo-project scope, and testability.
5. Update the authoritative plan/GDD note, its Decisions table, the open-question register, and project status only where their triggers apply. Avoid duplicate status or schemas.
6. Keep the planning gate closed until its checklist is complete and the user explicitly approves the gate. Treat implementation authorization as a separate explicit status; gate approval alone never permits C++, CMake, assets, dependencies, or implementation branches. Do not add implementation through this skill.
7. Summarize changed decisions, unresolved questions, risks, and the smallest next planning action.
