# ADR 0010 — Determinism and floating-point policy

**Status:** Accepted

**Date:** 2026-08-12

## Context

The P1a and P1b milestone plans assert determinism gates: identical inputs must reproduce event ordering and numerical results. `docs/architecture.md` makes deterministic headless scenario tests the backbone of the verification strategy for trajectories, staging, resources, science, contracts, and economy.

None of that is achievable without deciding what "deterministic" means and which compiler behavior is permitted. Floating-point results under MSVC vary with optimization level, fused-multiply-add contraction, vectorization decisions, and instruction-set target. The two declared baseline CPU classes — Intel Core i5-8400 (Coffee Lake) and AMD Ryzen 5 2600 (Zen+) — are different microarchitectures, and code compiled for one may take different vector paths on the other.

Deciding this late is expensive: the flags belong in the CMake foundation, and a physics or math library selected under a weaker assumption may be impossible to make deterministic afterward.

## Decision

### Guarantee level

- **Same build, same machine: bit-exact.** Identical inputs produce bit-identical floating-point output and identical event ordering. This is the level that scenario tests, regression gates, and the P1a/P1b determinism criteria assert.
- **Across machines: tolerance-based.** Results are compared against documented per-scenario tolerances, not bit-for-bit. Cross-machine bit-exactness is explicitly **not** promised.
- **Across builds: not promised.** A toolset or flag change may alter results. Golden values are regenerated deliberately with a recorded reason, never silently.

Cross-machine bit-exactness would require soft-float or comprehensive discipline over every arithmetic path, would constrain every future physics library choice, and buys nothing this project needs. Its only real use is replay files shared between machines and lockstep multiplayer, and this is a single-player game with no replay-sharing requirement.

### Compiler policy

- Use `/fp:precise`. `/fp:fast` is forbidden on project-owned targets.
- Disable FMA contraction explicitly rather than relying on the default, which has varied across MSVC toolsets.
- Target `/arch:AVX2`. All four declared baseline hosts support it: Coffee Lake and Zen+ both implement AVX2, including the hosts of the UHD 630 and Vega 8 investigation devices.
- Do not rely on `long double`, x87 behavior, or compiler-specific math intrinsics for authoritative simulation values.
- Increment A1 must verify and record the exact flag spellings for the selected toolset. Flag names and defaults in this area have changed between MSVC releases, so the recorded values are authoritative over this ADR's prose.

### Simulation policy

- Authoritative simulation uses `double`. `float` is permitted in rendering and presentation, and at explicitly documented boundaries.
- Iteration order over any container that participates in an authoritative result must be deterministic. Do not iterate unordered containers, pointer-keyed maps, or address-sorted collections where the order affects output.
- Multithreaded work that contributes to authoritative state must produce order-independent results or use a deterministic reduction order. Non-deterministic work-stealing is acceptable only for work whose output cannot affect simulation state.
- Time accumulation uses a fixed integer campaign-time representation with an explicit tick rate rather than repeated floating-point addition of a delta.
- Random number generation uses explicitly seeded, project-owned generators with recorded algorithms. Never use `rand()`, an unseeded default engine, or a generator whose implementation varies by standard library version.

### Dependency policy

Any proposed physics, math, or simulation library must document its determinism characteristics under this policy as part of its ADR 0007 dependency review. A library that cannot meet same-machine bit-exactness is not automatically rejected, but it must be confined to a non-authoritative role.

## Alternatives considered

- **Cross-machine bit-exact:** rejected. High cost across every math path, constrains library selection permanently, and serves no requirement in a single-player game without shared replays.
- **Tolerance-based everywhere, including locally:** rejected. Produces flaky scenario tests and removes the ability to detect numerical regressions reliably, which is the main reason the verification strategy leans on headless scenarios.
- **`/fp:strict`:** rejected as the default. It disables useful optimization and is stronger than same-machine bit-exactness requires. It remains available for a specific target if one is later shown to need it.
- **`/arch:SSE2` for wider CPU compatibility:** rejected. It would widen support below the declared baseline while costing performance on every declared target.

## Consequences

- `/arch:AVX2` sets a hard floor at Haswell-era and Zen-era CPUs. Pre-2013 Intel and pre-2017 AMD processors are unsupported. This is consistent with the declared baseline and should be stated in support claims rather than discovered by a player.
- Scenario tests may assert exact numerical values when run on the developer machine, and must use documented tolerances in any future CI on different hardware.
- If CI is later added on hardware differing from the development machine, its numerical assertions must be tolerance-based from the start.
- Golden fixtures record the toolset and flags that produced them, per ADR 0008's provenance requirement.
- Save files store authoritative state rather than a random seed plus a replay, since replay reproduction is not guaranteed across builds.

## Validation

- Increment A1 delivers a determinism smoke scenario that runs the same input repeatedly and compares output bit-for-bit.
- Increment A3 asserts bit-identical event ordering and numerical output for its orbital scenarios.
- Increment B2 asserts the same for craft physics and resource-network scenarios.
- A deliberate flag change must be shown to alter golden output, confirming the harness would detect an accidental one.

## Sources

- Microsoft documents `/fp` behavior, contraction, and the interaction with optimization in the [`/fp` (Specify floating-point behavior) reference](https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior).
- Microsoft documents `/arch` targets and their instruction-set implications for x64 in the [`/arch` (x64) reference](https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64).
- Microsoft describes floating-point optimization and reproducibility tradeoffs in [Microsoft Visual C++ floating-point optimization](https://learn.microsoft.com/en-us/cpp/build/reference/fp-specify-floating-point-behavior#remarks).
