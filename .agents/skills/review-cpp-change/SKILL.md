---
name: review-cpp-change
description: Review SolEngine and SolProject C++ diffs for correctness, ownership, API/ABI, units and frames, persistence, build integration, performance, and verification gaps. Use for C++ code review, diff review, or implementation handoff.
---

# Review a C++ change

1. Read `AGENTS.md`, the task/milestone, accepted ADRs, the complete diff, and affected callers/data flows.
2. Use `cpp_reviewer` for an independent read-only pass when the user permits subagents. Reviewers never edit.
3. Check correctness/regressions first; then ownership/lifetime, threading, error handling, units/frames, API/ABI/export surface, persistence/content compatibility, dependency leakage, explicit CMake enumeration, and realistic performance.
4. For simulation code, also invoke `$review-simulation-change` or apply its checks.
5. Compare tests and smoke evidence to milestone acceptance criteria; do not claim coverage a command did not exercise.
6. Return findings ordered by severity with file/line evidence, questions/assumptions, and a concise residual-risk summary. Do not edit unless the user assigns implementation to the sole writer.
