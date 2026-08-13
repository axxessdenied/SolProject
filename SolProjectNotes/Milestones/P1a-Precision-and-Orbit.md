# P1a — Precision and Orbit Prototypes

**Status:** See [`docs/project_status.md`](../../docs/project_status.md), which is authoritative for milestone state and implementation authorization. This plan defines scope and acceptance criteria; it does not authorize implementation.

**Outcome owner:** Claude, as the named single writer. One Codex or Claude feature owner must hold the branch/worktree and remain the only writer.

**Planning source:** User approval on 2026-08-12; scope split and revision approved 2026-08-12

**Successor:** [P1b — Renderer and Craft Prototypes](P1b-Renderer-and-Craft.md)

## Outcome

P1a produces measured evidence for the technical claims that shape the simulation data model: astronomical-scale precision, reference-frame conversion, and local-to-analytical orbital transitions under time warp.

P1a is deliberately headless. It answers the questions whose answers constrain every later subsystem, at the lowest cost and before any renderer, asset, or physics-library commitment exists. Its executables are disposable; its scenarios, fixtures, raw measurements, conclusions, and ADR updates are durable.

P1a succeeds by selecting, constraining, or rejecting candidate approaches.

## Relationship to P1b

P1a and P1b were separated from the original combined P1 plan so that the cheap, high-information headless work completes before the expensive graphical and physics work begins.

The original P1 increment 2 contained both a numerical precision component and a screen-space render-jitter component. Screen-space jitter cannot be measured without a renderer, so:

- the **numerical** frame-conversion and precision work is increment A2 below;
- the **screen-space jitter gate** moves to P1b increment B1, where it is evaluated against the frame model that A2 selects.

A2 therefore closes on numerical evidence alone. If B1 later shows that the selected frame model cannot meet the jitter gate, that is a P1b result that reopens A2's decision rather than a P1a failure.

The original P1 increment 6 (versioned persistence round trips) moved out of technical-risk prototyping entirely and into P2/M1. Versioned JSON and chunked-binary round trips are well-understood engineering rather than an open technical risk, and ADRs 0004 and 0009 stand without prototype evidence.

## Authorization and prerequisites

Implementation remains forbidden until all of the following are true:

- `docs/project_status.md` explicitly marks the planning gate **Approved**;
- the user explicitly authorizes implementation and identifies the single writer;
- the intended branch and base are recorded under the repository Git policy;
- each third-party package has an accepted need and completes ADR 0007's dependency workflow before its declaration is added;
- Debug and Release preset names, the exact supported MSVC toolset, and the ADR 0010 floating-point flags are recorded during increment A1.

P1a requires no reference GPU hardware. Its measurements run on the development machine and on the baseline CPU classes where accessible.

## Boundaries

### In scope

- minimal instrumented headless executables and scenarios needed to answer the three areas below;
- reference fixtures derived from ADR 0008 with provenance, units, frames, epochs, and checksums;
- candidate algorithms or small libraries compared behind prototype-local boundaries;
- machine-readable results plus concise evidence reports;
- ADR and plan changes justified by measured results.

### Non-goals

- rendering of any kind, including diagnostic visualization beyond text and machine-readable output;
- production gameplay, construction, campaign progression, or content;
- a reusable general engine framework designed around disposable prototypes;
- final gravity-model, integrator, ECS, or serialization-library selection beyond what an increment directly proves;
- carrying rejected prototype code into P1b or P2 for sunk-cost reasons.

## Shared measurement rules

- Performance measurements use optimized Release builds with diagnostics that do not materially distort the result. Debug and validation runs are reported separately.
- Every report records commit, preset/toolchain, OS build, CPU, scenario seed/input, warm-up, sample count, and raw-output location.
- Numerical comparisons record units, reference frame, time scale, reference result, absolute error, and relative error where meaningful.
- Determinism claims are scoped by ADR 0010: bit-exact on the same build and machine, tolerance-based across machines.
- A threshold may be changed only by a documented planning/ADR update approved by the user. Failed results must not be relabeled as passes.

## Accepted thresholds

| Area | Pass criterion |
|---|---|
| Frame conversion | Round-trip conversion through the full frame chain preserves position within 1 mm and velocity within 1 µm/s at the surface anchor, and within the measured floating-point budget at orbital distances |
| Conversion drift | 10^6 repeated conversions show bounded, non-accumulating error; any growth is characterized and attributed to a named frame boundary |
| Local ↔ analytical handoff | State discontinuity no greater than 1 m position and 1 mm/s velocity per transition |
| Reference orbit | After one approximately 200 km circular-orbit period, the integrated position is within 100 m of the Kepler analytic reference at the same campaign time |
| Warp equivalence | Accelerated and real-time runs of the same elapsed campaign time agree within the handoff and reference-orbit tolerances above |
| Determinism | Identical inputs reproduce event ordering and bit-identical numerical output on the same build and machine |

Peak process memory and allocation counts are mandatory measurements.

## Time boxes

Each increment carries a cost gate. Exceeding the box is itself a result: it triggers a narrow-or-reject decision with the user, not silent continuation.

| Increment | Box | Trigger on exceeding |
|---|---|---|
| A1 — Harness | 1 week | Reduce to a single headless target; defer metrics formatting |
| A2 — Frames and precision | 2 weeks | Reduce candidate frame models to the single leading option and record the others as untested |
| A3 — Hybrid orbit and warp | 3 weeks | Narrow to a single integrator candidate and defer the warp-equivalence scenario to P2/M5 |

## Increment A1 — Measurement and build harness

### Deliverables

- the smallest C++23/MSVC/CMake/Ninja build graph needed for independent headless prototypes;
- separate Debug and Release presets with explicit source lists;
- the ADR 0010 floating-point and architecture flags applied to project-owned targets;
- a repeatable metrics output format and scenario metadata record;
- capability reporting for the host toolchain.

### Done criteria and validation

- a clean configure/build/test succeeds through the checked-in presets using the documented toolchain and dependency bootstrap;
- a small conformance target compiles using each required C++23 facility, per ADR 0001;
- one headless timing scenario emits complete metadata and machine-readable measurements;
- Debug and Release artifacts cannot overwrite one another;
- a determinism smoke scenario reproduces bit-identical output across repeated runs on the same build;
- the handoff records the literal configure, build, test, and scenario commands. Until preset names are accepted, command contracts are `cmake --preset <preset>`, `cmake --build --preset <build-preset>`, and `ctest --preset <test-preset>`.

### Documentation trigger

Update project status, implemented architecture, the selected MSVC minimum, preset names, the applied floating-point flags, and every dependency record actually introduced.

## Increment A2 — Reference frames and numerical precision

### Deliverables

- explicit-unit and explicit-frame types sufficient to compare candidate hierarchical origins and double-precision authoritative state;
- at least two candidate frame-graph models compared on conversion cost, precision, and ergonomics;
- ADR 0008 fixtures for the 2026 epoch and the 28.0° N, 80.5° W, 5 m launch anchor, with recorded provenance and checksums;
- a numerical surface-to-200-km trajectory sampling harness reporting per-boundary conversion error;
- a documented precision budget from surface millimetre scale to orbital distance.

### Done criteria and validation

- frame conversions meet the round-trip and drift thresholds above;
- every failure identifies the frame boundary responsible;
- the precision budget is stated as measured numbers rather than asserted;
- one frame/origin model is selected, or alternatives are explicitly kept open with the specific evidence needed to decide them;
- the selected model's conversion cost is recorded so P1b can budget against it.

### Documentation trigger

Record the selected frame/origin model in `docs/architecture.md`, or record the open alternatives and their deciding evidence.

## Increment A3 — Hybrid orbit and time warp

Per ADR 0011, the gravity baseline is patched conics with spheres of influence. Analytical coast is Kepler propagation within a single SOI. Neither atmospheric drag nor perturbations enter the propagation.

### Deliverables

- a deterministic headless two-body scenario using the ADR 0011 model;
- a candidate fixed-step local numerical integrator, a Kepler analytical coast, explicit eligibility rules, and bidirectional handoff;
- SOI-crossing entry and exit handling with defined state ownership on each side;
- normal-time and accelerated-coast runs of the same approximately 200 km circular orbit over the same elapsed campaign time;
- metrics for position, velocity, orbital elements, conserved quantities, transition counts, and event chronology.

### Done criteria and validation

- every local/analytical transition meets the 1 m and 1 mm/s discontinuity limits;
- the one-orbit integrated result is within 100 m of the Kepler analytic reference at the same campaign time;
- accelerated and real-time runs agree within the warp-equivalence threshold;
- identical inputs reproduce event ordering and bit-identical numerical results per ADR 0010;
- invalid transition conditions are rejected explicitly rather than producing a silent discontinuity;
- SOI crossings preserve state within the handoff tolerance and are ordered deterministically under warp;
- the result identifies which powered-warp, attitude-control-under-warp, and encounter-prediction questions remain for P2.

### Documentation trigger

Record the selected transition contract in `docs/architecture.md`. Update the open questions for powered warp, attitude control under warp, encounters, and final gameplay tolerances.

## Exit criteria

- every accepted threshold has a reproducible result or an approved documented revision;
- the frame/origin strategy and the hybrid transition contract each have a clear architectural disposition;
- ADR 0010 and ADR 0011 are confirmed, amended, or superseded from evidence;
- no unresolved result can silently become a P1b or P2 implementation assumption;
- one index links raw results, scenario definitions, toolchain metadata, accepted failures, and conclusions;
- `complete-milestone` review verifies evidence before project status marks P1a complete.

## Risks and recovery

- **Instrumentation distortion:** compare instrumented and minimally instrumented runs; report overhead.
- **Prototype contamination:** keep production-facing APIs out of disposable experiments. Reuse requires an explicit review, not file copying by default.
- **Threshold failure:** preserve the raw evidence, reject or narrow the candidate, and update ADR/status. Do not relax a numerical tolerance to make a candidate pass.
- **Precision budget failure:** if no candidate frame model meets the budget, the correct response is a documented architecture change, not a reduction in world scale, before P1b begins.
- **Dependency dead end:** keep any candidate behind a narrow boundary and retain a dependency-free fixture/scenario so another candidate can be evaluated.
- **Time-box overrun:** apply the narrowing action in the time-box table and record the untested alternatives as open.

Rejected prototype code may be removed only after its evidence and decision record are retained and the exact removal scope is reviewed.

## Handoff contract

Full handoff records are required at **increment closure**, not on every commit. See the lightweight-lane rule in `AGENTS.md`.

Each increment closure records:

- goal and current outcome;
- single owner, branch, and base;
- changed files and dependency changes;
- literal configure/build/test/scenario commands and results;
- hardware, toolchain, settings, samples, raw evidence, and summary metrics;
- failed or waived criteria — waivers require prior user-approved plan changes;
- ADR/documentation changes, remaining risks, disposable code, and the smallest next action.

## Decisions

| Decision | Status | Why |
|---|---|---|
| P1 split into headless P1a and graphical/physics P1b | Confirmed | User approved 2026-08-12; sequences the cheap high-information work first |
| Persistence round trips moved to P2/M1 | Confirmed | User approved; not an open technical risk, and ADRs 0004/0009 stand without prototype evidence |
| Screen-space jitter gate moved to P1b increment B1 | Confirmed | Cannot be measured without a renderer; A2 closes on numerical evidence |
| P1a is evidence-oriented and may use disposable code | Confirmed | User approved risk-first vertical slicing and measurable criteria |
| Patched conics with SOI as the A3 gravity baseline | Confirmed | ADR 0011 |
| Same-machine bit-exact determinism | Confirmed | ADR 0010 |
| 1 m/1 mm/s handoff and 100 m one-orbit gates | Confirmed | User approved recommendation |
| Per-increment time boxes with narrow-or-reject triggers | Confirmed | User approved 2026-08-12; the original plan had quality gates but no cost gates |
| Hybrid transition contract: one authoritative regime, state-anchored coast, refined crossing events, named rejections | Confirmed | A3 evidence; recorded in `docs/architecture.md` |
| RK4 for the local numerical regime | Confirmed | A3 evidence: clears the 100 m gate at 3× less cost than the nearest symplectic candidate, and the hybrid contract never integrates a stable orbit long enough for secular energy error to matter |
| Earth atmosphere limit of 140 km | Confirmed for P1a, tuning deferred to P2/M5 | A3 evidence; recorded in ADR 0011's validation section. A tolerance-derived boundary would be ~450 km and would make the first playable's contract orbit un-warpable |
| Warp ticks constrained to integer multiples of the local physics step | Confirmed | A3 evidence: this is what makes the local regime reproducible under warp, and it replaces the assumption that powered warp is unsafe |
| Coast anchor representation: Cartesian state or classical elements | Open, and safe either way | A3 measured both inside tolerance by six orders of magnitude, with the element path reaching a bitwise fixed point. The decision is storage and legibility, not numerics |
| Exact production libraries and reusable code | Open by owning increment | Must follow evidence and ADR 0007 rather than be selected globally |
