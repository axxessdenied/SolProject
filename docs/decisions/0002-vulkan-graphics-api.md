# ADR 0002 — Prefer Vulkan for the renderer

**Status:** Accepted

**Date:** 2026-08-12

**Revised:** 2026-08-12 — required evidence rebalanced toward precision and capability; frame-time confirmation moved to P2/M2; Direct3D 12 spike replaced with a documented analysis. See [P1b — Renderer and Craft Prototypes](../../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md).

**Accepted 2026-08-14** by the user, on the P1b increment B1 evidence indexed at [`evidence/p1b/B1/`](../../evidence/p1b/B1/Index.md). Every gating clause below is met; the disposition and its limits are recorded in [Gating evidence disposition](#gating-evidence-disposition). **Acceptance is on precision, capability, tooling and the documented Direct3D 12 analysis, and on one device.** It does not assert baseline-class or AMD driver behaviour, which the hardware caveat below continues to exclude, and it does not close the deferred P2/M2 frame-time gates.

> **Hardware caveat, added 2026-08-13.** Every claim in this ADR about the GTX 1060 6 GB, RX 580 8 GB, UHD 630, and Vega 8 classes is **unverified on hardware**. None of those devices is available to this project, and no AMD device or driver stack of any kind is — so no clause here may be read as asserting AMD driver behaviour. What can be obtained is set out in the accepted [reference-hardware evidence plan](../../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md); this ADR may close on precision, capability, tooling, and the documented Direct3D 12 analysis, but not on baseline-class driver evidence.

## Context

SolEngine needs explicit control over large-world rendering, terrain/atmosphere level of detail, GPU memory, synchronization, diagnostics, and scalable graphics settings. Windows x64 is the first platform, but Vulkan is the user's preferred API. The discrete baseline targets 60 FPS at 1080p on an Intel Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, and SSD. Intel UHD 630 and AMD Vega 8-class integrated graphics are investigation targets for 30 FPS at 720p/low; support remains conditional on P1 driver/capability/performance evidence.

## Accepted P1 target and proposed production decision

- Use Vulkan as the primary SolEngine graphics API if the P1 prototype passes its acceptance criteria.
- Keep Vulkan types behind renderer-owned interfaces; game and general engine-domain APIs must not expose them.
- Use Vulkan 1.2 as the candidate minimum API version for P1. Query instance and physical-device versions, features, extensions, formats, queues, limits, and memory capabilities rather than inferring support from the API version alone.
- Treat Vulkan 1.3 and later capabilities as optional during P1. A capability-based path may use them when present, but the prototype must not make them an undeclared baseline requirement.
- Do not select mandatory non-core extensions, shader language/compiler, allocator, rendering framework, or pinned SDK until their owning prototype increment demonstrates the need and completes dependency review.
- Build capability tiers: a conservative required baseline for older hardware and optional visual features for stronger GPUs. Ray tracing and mesh shaders must not be baseline requirements.
- Provide a distinct low graphics tier intended to reach 30 FPS at 720p/low on UHD 630 and Vega 8-class investigation hardware; do not promise support until measured.
- Use validation layers and capture/debug tooling in development; enumerate loader, device version, features, extensions, formats, queues, and memory capabilities at startup with actionable failure diagnostics.
- Treat the Vulkan SDK as a development toolchain input. The shipped game must detect the installed Vulkan loader/driver and fail clearly when the required device capability is unavailable.

## Alternatives considered

- **Direct3D 12:** excellent Windows integration and a credible fallback if Vulkan driver coverage, tooling, or performance fails on the selected older-PC baseline.
- **A third-party rendering abstraction:** may accelerate early development but risks constraining the unusual large-world renderer; evaluate only against concrete P1 requirements.
- **Supporting Vulkan and Direct3D 12 immediately:** rejected for initial scope because two production backends would double validation burden before one renderer proves the game.

## Required P1b evidence

This ADR closes on evidence that a prototype can uniquely provide: precision, structural viability, and device capability. It deliberately does not close on prototype frame rate.

Frame time measured in a scene with placeholder geometry, no production part meshes, and no authored terrain is weakly predictive of the shipped renderer, and gating an API decision on it would reject or accept Vulkan for reasons unrelated to Vulkan. Frame time is therefore **measured and recorded** in P1b as the M2 baseline, and **gated** in P2/M2 where the scene is representative.

### Gating evidence

- Correct camera-relative rendering and depth behavior from launch surface to orbital altitude, with no depth collapse, z-fighting, or near/far discontinuity along the full path.
- No more than 0.25 pixels of stationary screen-space jitter at the surface anchor and a 200 km orbital vantage, measured by the method defined in the P1b milestone plan.
- Stable terrain/atmosphere LOD transitions with no detectable popping at the recorded quality setting and no unbounded memory growth over a 30-minute traverse.
- Startup capability reporting — loader, device version, features, extensions, formats, queues, limits, memory — and graceful rejection of unsupported drivers and devices with actionable diagnostics.
- Clean validation-layer output for the exercised path, or an explanation and acceptance of every remaining message, plus a usable RenderDoc/capture workflow.
- Vulkan types confined to renderer-owned interfaces, demonstrated by inspection of the module boundary.
- A documented Direct3D 12 comparison analysis covering driver coverage on the baseline GPU classes, tooling maturity, shader toolchain, and the cost of a future backend swap behind the renderer interface.

### Gating evidence disposition

Recorded 2026-08-14 at acceptance. Every figure is from the RTX 4060 Laptop GPU unless stated, and the hardware caveat above governs what that permits anyone to claim.

| Gating clause | Disposition |
|---|---|
| Camera-relative rendering and depth, surface to orbit | **Met.** No collapse; resolution scales linearly from 1 m to 10 000 km, matching the analytic prediction to between 3e-9 and 1e-7. A conventional finite-far projection through the identical harness collapses past 10 km, which is what makes the pass evidence rather than an assertion |
| ≤ 0.25 px stationary jitter at both reference views | **Met.** 0.000000 px, frames bit-identical, with a sub-pixel response control confirming precision rather than only stability |
| Stable LOD transitions, no detectable popping, no unbounded memory growth over a 30-minute traverse | **Met**, under two measurement methods that did not exist when this clause was written — neither named a statistic or a limit. Both were defined and ratified by the user on 2026-08-14 as separate decisions. Popping: 0.000101 of the frame at the worst step against a 0.0020 limit, control separating 4.4×. Memory: second-half trend 15.9 KiB/min against a 64 limit and 0.98 MiB growth against a 2 MiB backstop, with an 8-byte-per-frame leak demonstrated to fail both. **The popping pass is a margin and a response, not a rescue** — the control is itself below the perceptual limit. **The memory pass bounds a rate; it does not prove an asymptote** |
| Startup capability reporting and graceful rejection with actionable diagnostics | **Met.** The renderer enumerates loader, device version, features, extensions, formats, queues, limits and memory, and rejects unsupported devices by named reason with a negative control proving the rejection path fires. **Narrower than it looks:** the four synthetic device profiles are hand-authored and near-identical across every field the requirement consults, so they are one assertion rather than four until reconciled against real reports. That reconciliation bears on baseline-class support claims, which this ADR does not make, so it does not block acceptance — it is tracked in `docs/project_status.md` |
| Clean validation output, or explanation and acceptance of every message, plus a usable capture workflow | **Met.** Zero messages originate from this project; three `LLP_LAYER_3` loader warnings come from a third-party overlay layer installed on the machine and fall in this clause's "explained and accepted" category. The capture workflow is **GFXReconstruct from the pinned Vulkan SDK**, not RenderDoc — it adds no dependency, and was verified end to end on 2026-08-14 by a 240-frame capture, `gfxrecon-info` inspection, and a clean 240-frame replay. RenderDoc remains the better interactive frame debugger and is not installed here |
| Vulkan types confined to renderer-owned interfaces, by inspection | **Met, and tighter than required.** Four files include a Vulkan header, all under `engine/render/src/`. **No public header contains a Vulkan type at all**, including the renderer's own; the only `Vk*` occurrences in `engine/render/include/` are three comments explaining what the interface deliberately does not mirror. Enforced by the build rather than convention — every Vulkan-facing package is linked privately, so an escaping type is a compile error |
| Documented Direct3D 12 comparison analysis | **Met.** [P1b — Direct3D 12 comparison analysis](../../SolProjectNotes/Milestones/P1b-Direct3D12-Comparison-Analysis.md), 2026-08-14. Driver coverage is equivalent across all four baseline classes and the one genuine coverage risk — AMD's legacy driver track for GCN 4 and Raven Ridge — is API-neutral. Tooling favours Direct3D 12 modestly. Shader toolchains are equivalent. The backend-swap cost is bounded and measured at the module boundary. Recommendation: retain Vulkan |

**What acceptance does not settle**, restated so a later reader cannot infer more than was measured: nothing is verified on any baseline-class device or on any AMD hardware; the Intel UHD is unmeasured; frame time is neither gated nor claimed here; and the deferred P2/M2 gates below are untouched.

### Recorded, not gating

- Full frame-time distribution — p50, p95, p99, maximum, CPU and GPU separately — at 1920×1080 low/medium on GTX 1060 6 GB and RX 580 8 GB classes. **Neither device is available; unmeasured is recorded as a state, not as a pass.**
- The same distribution at 1280×720 low on UHD 630 and Vega 8-class systems where accessible, including driver, Vulkan capabilities, visual compromises, and any unsupported status. **Neither is accessible.**
- What is measurable instead, recorded under its own name and never as a proxy for a baseline class: an RTX 4060 Laptop GPU and an Alder Lake-P Intel UHD, both on a single thermally-constrained laptop with variable GPU power.
- Peak CPU and GPU memory, allocation counts, and upload volume.

### Deferred to P2/M2

- The p95 ≤ 16.67 ms discrete-baseline gate at 1080p low/medium, in a scene with representative assets.
- A frame-time spike criterion alongside it: p99 ≤ 25 ms and no frame exceeding 33 ms. Mean frame time is not a sufficient description of perceived smoothness, and the original single-p95 gate would have accepted a visibly stuttering renderer.
- The integrated investigation tier's disposition. Failure there narrows the support target rather than reopening this ADR.

### Withdrawn

The original requirement for a minimal Direct3D 12 spike is withdrawn. A second backend costs weeks and cannot realistically change the decision for a Windows-only single-player title, where the two APIs differ in tooling and driver ergonomics rather than achievable performance. A Vulkan driver or capability failure on a baseline device with no workaround remains a genuine trigger to reconsider — that is a measured result, not a reason to have built the spike preemptively.

## Consequences

- Vulkan 1.2 is the P1 candidate floor, not yet a production support promise. P1b evidence may retain it, lower it with explicit fallbacks, raise it with an accepted hardware-scope change, or reject Vulkan.
- Optional graphics tiers must never change authoritative simulation results.
- Renderer architecture must budget for pipeline/shader caching, asynchronous uploads, resource lifetime, and device-loss/error reporting from the beginning.
- P1b records peak CPU and GPU memory. Production content budgets are set after representative assets exist rather than invented for the prototype.
- Accepting this ADR on precision evidence means M2 inherits a real performance risk. If M2's representative scene misses the 16.67 ms gate, the response is a renderer optimization or content-budget milestone — not an API reversal, because by then the backend swap cost is the renderer interface's problem rather than this decision's. The renderer interface boundary required above is what keeps that option open at bounded cost.
- The P1b renderer is built in the production tree rather than as a disposable prototype. This ADR's boundary requirements therefore apply from the first commit rather than at a later cleanup.

## Sources

- Khronos documents that Windows requires a Vulkan loader plus a compatible device driver and recommends checking actual device support: [Checking for Vulkan Support](https://docs.vulkan.org/guide/latest/checking_for_support.html).
- Khronos recommends the Vulkan SDK for headers, validation layers, and development tools: [Vulkan Development Environments](https://docs.vulkan.org/guide/latest/ide.html).
- Vulkan versions are backward compatible, but instance and device version support can differ; required features must be queried: [Vulkan Versions and Porting Guide](https://docs.vulkan.org/guide/latest/versions.html).
- Khronos describes per-device queries for properties, features, extensions, limits, and formats: [Querying Properties, Extensions, Features, Limits, and Formats](https://docs.vulkan.org/guide/latest/querying_extensions_features.html).
