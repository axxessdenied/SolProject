---
name: review-simulation-change
description: Review SolProject astrodynamics, physics, time warp, reference-frame, resources, economy, population, and simulation-LOD changes for numerical and systemic correctness. Use for any change whose result depends on units, time, integration, deterministic state, conservation, or transitions between simulation regimes.
---

# Review a simulation change

1. Read `AGENTS.md`, the owning design/ADR, the diff, equations/algorithms, constants/data sources, and scenario tests.
2. Use `simulation_reviewer` for an independent read-only pass when the user permits subagents. Reviewers never edit.
3. Verify explicit units, dimensions, coordinate frames, epoch/time domain, sign/axis conventions, reference-frame transformations, precision, overflow/underflow, and boundary conditions.
4. Check timestep/integrator stability, determinism, invariants/conservation, time-warp behavior, scheduled events, promotion/demotion between simulation regimes, and save/load round trips.
5. Require declared tolerances and representative scenarios, including zero/extreme values and transition boundaries. Compare trusted reference cases where applicable.
6. Separate correctness defects, model limitations, tuning choices, and missing evidence. Report findings by severity with file/line evidence; do not edit unless separately assigned.
