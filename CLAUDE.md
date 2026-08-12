# Claude Code overlay

Read and follow [AGENTS.md](AGENTS.md); it is the canonical repository contract.

Claude and Codex are peer feature owners, but only the explicitly selected branch/worktree owner writes for a task. Project commands and specialist wrappers delegate to the canonical workflows in `.agents/skills/` and must not duplicate their rules.

Use Antigravity for research, brainstorming, planning, digesting, and review. A delegated Antigravity call does not transfer branch ownership and must not autonomously edit core C++.

During pre-production, respect both the planning gate and separate implementation authorization in `docs/project_status.md`: gate approval or an implementation-ready plan alone never authorizes implementation.
