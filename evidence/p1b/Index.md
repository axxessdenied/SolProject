# P1b — milestone evidence index

**Milestone:** P1b, renderer and craft prototypes
**State:** **In progress.** Increment B1 is authorized and under way; increment B2 is **not
authorized** and requires its own explicit user instruction.

This is the milestone-level record. Per-increment detail lives below it, and the authoritative
statement of phase and authorization is [`docs/project_status.md`](../../docs/project_status.md).

| Increment | Scope | State | Evidence |
|---|---|---|---|
| B1 | Vulkan large-world renderer | **In progress** — not closed | [B1/Index.md](B1/Index.md), [B1/Handoff.md](B1/Handoff.md) |
| B2 | Constructed craft and resource networks | **Not authorized** | — |

## What P1b can and cannot close

The milestone's own [reference-hardware evidence
plan](../../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) was accepted on
2026-08-13 in place of hardware the project does not have, and it binds what may be reported.

- **No named baseline device class is available.** The only machine carries an RTX 4060 Laptop GPU
  and Intel UHD Graphics. There is no GTX 1060, RX 580, UHD 630 or Vega 8, and no AMD device or
  driver stack of any kind. **No measurement here is a baseline-tier result**, and none may be
  reported as a proxy for one.
- **The gating thresholds still gate.** Jitter, depth, LOD continuity and validation output are
  properties of the implementation rather than of GPU throughput, so the absent hardware does not
  excuse them.
- **ADR 0002 may close** on precision, capability, tooling and the documented Direct3D 12 analysis.
  It **may not close** on any clause asserting AMD driver behaviour.
- **Frame time is non-gating in P1b** by user decision, and every figure recorded is a laptop
  measurement subject to Dynamic Boost, variable TGP and thermal limits — not a fixed-hardware
  quantity.

## Milestone-level state, 2026-08-14

Three of B1's gating thresholds are met on the single device used; one is met in part; one cannot
close yet. The LOD continuity threshold's measurement method was undefined as originally written
and was **ratified by the user on 2026-08-14**, recorded in the [milestone
plan](../../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) beside the screen-space jitter
method it parallels. That ratification deliberately did **not** extend to the threshold's
30-minute memory clause, which stands as written. That traverse has since been run, on
2026-08-14; because the clause names no statistic and no limit, it yielded a measurement rather
than a verdict, recorded in [B1's evidence index](B1/Index.md).

No P1b increment is closed, and no milestone review has run.
