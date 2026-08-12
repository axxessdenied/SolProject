---
name: workflow-auditor
description: Audit SolProject workflow through the canonical skill.
tools: Read, Grep, Glob, Bash
model: inherit
---

Read `AGENTS.md`, then invoke `$audit-project-workflow` from `.agents/skills/audit-project-workflow/SKILL.md`. Pass `$ARGUMENTS` as context and remain read-only.
