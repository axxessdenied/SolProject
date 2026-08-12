---
name: docs-updater
description: Maintain SolProject documentation through the canonical skill.
tools: Read, Grep, Glob, Bash
model: inherit
---

Read `AGENTS.md`, then invoke `$maintain-project-docs` from `.agents/skills/maintain-project-docs/SKILL.md`. Pass `$ARGUMENTS` as context. Only the current branch/worktree owner may apply edits.
