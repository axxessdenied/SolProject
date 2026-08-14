# P1b — Reference Hardware Evidence Plan

**Status:** Accepted by the user on 2026-08-13, as the documented evidence plan required by the [P1b milestone plan](P1b-Renderer-and-Craft.md)'s authorization prerequisites. **Extended on 2026-08-14** with the [CPU section for increment B2](#cpu-evidence-for-increment-b2), which was left open by design in the original and settled when B2 was authorized.

**Purpose:** P1b's authorization prerequisites require that the named baseline GPU hardware be available, **or** that the user accept a documented evidence plan for unavailable devices. None of the four named device classes is present. This document is that plan. It defines what will be measured, on what, what those measurements may and may not be used to claim, and how the unverified claims are discharged later.

The 2026-08-14 extension applies the same treatment to the **baseline CPU classes** that increment B2's gates are specified against, which are equally absent.

This plan does not change any accepted threshold. Thresholds are owned by the [P1b milestone plan](P1b-Renderer-and-Craft.md) and may only be changed by a user-approved planning update.

## Available measurement hardware

One machine is available. It is a laptop, which has consequences recorded under [Measurement hazards](#measurement-hazards).

| Component | Detail |
|---|---|
| CPU | 12th Gen Intel Core i7-12650H, 10 cores / 16 threads |
| Discrete GPU | NVIDIA GeForce RTX 4060 Laptop GPU, `PCI\VEN_10DE&DEV_28A0`, driver 32.0.15.8115 (2025-08-20) |
| Integrated GPU | Intel UHD Graphics (Alder Lake-P GT1), `PCI\VEN_8086&DEV_46A3`, 16 EU Xe-LP, driver 32.0.101.7082 (2025-11-30) |
| OS | Windows 11 Home, build 10.0.26200 |

Exact driver versions, OS build, and preset/toolchain are re-recorded per report as the shared measurement rules require; the table above is the inventory, not a substitute for per-report metadata.

## Named baseline classes and their availability

| Plan class | Tier | Available? | Nearest present device | Usable as a substitute? |
|---|---|---|---|---|
| GTX 1060 6 GB | Discrete baseline | **No** | RTX 4060 Laptop | **No.** Substantially faster, newer architecture, newer feature level. |
| RX 580 8 GB | Discrete baseline | **No** | none — no AMD GPU is present | **No.** No AMD driver stack can be exercised at all. |
| Intel UHD 630 | Integrated investigation | **No** | Intel UHD Graphics (Alder Lake-P, 16 EU Xe-LP) | **No.** Different architecture generation (Xe-LP vs Gen9.5) and a different EU count. It is a separate integrated data point, not a proxy. |
| AMD Vega 8 | Integrated investigation | **No** | none | **No.** |

The P1b plan's risk register states the rule directly: *do not substitute stronger hardware and extrapolate.* This plan does not extrapolate. Every measurement is reported against the device that produced it, under that device's own name.

The absence of any AMD device is the sharpest gap. It removes both AMD baseline classes and, more importantly, removes the entire AMD Vulkan driver stack from B1's evidence — the single most likely source of a driver-specific capability or validation failure that the NVIDIA path would not reveal.

## Threshold disposition under this plan

The gating thresholds separate cleanly into those that a correct implementation satisfies on any conformant device, and those that are a property of the device.

### Gating thresholds that remain fully gating

These are properties of the frame model, projection and depth setup, LOD scheme, and code correctness, not of GPU throughput. They gate P1b as written, measured on both available devices.

| Threshold | Why it is device-portable | How the unavailable devices affect it |
|---|---|---|
| Render precision (≤ 0.25 px jitter at both reference views) | A function of the A2 frame model, camera-relative rendering, and shader precision. A faster GPU does not hide jitter; it produces the same centroid. | Low residual risk. A driver-specific shader optimisation could alter precision. Recorded as a residual risk, not a waived gate. |
| Depth behavior (no collapse, z-fighting, or near/far discontinuity) | A function of the depth range, reversed-Z choice, and format selection. | Depth format and precision **availability** is device-dependent; the chosen format's support on unavailable devices is a capability question, handled below. |
| LOD continuity (no detectable popping; no unbounded memory growth over 30 min) | Popping is algorithmic. Unbounded growth is a leak, not a throughput limit. | None for popping. Absolute memory figures are device-specific and reported as such. |
| Validation output (clean, or every message explained and accepted) | Validation layers are the Khronos implementation, not the vendor's. | Vendor-specific *best-practice* layer output is not obtainable for AMD. Core validation is. |

### Gating thresholds that are partly device-bound

**Capability reporting** gates P1b, and is the one gating threshold that this hardware genuinely cannot fully exercise. Two problems:

1. Both present GPUs exceed the Vulkan 1.2 candidate floor. The **rejection path** — "unsupported devices are rejected with actionable diagnostics" — has no real device that triggers it.
2. The GTX 1060 and RX 580 classes are exactly the devices most likely to sit at or near the 1.2 floor and to lack an optional capability the renderer wants.

Mitigation, which is required rather than optional for B1 closure:

- The capability query must be structured so that the **required set is declared as data**, separately from the query and rejection logic.
- The rejection path is verified by a **negative control**: an injected capability profile that denies a required feature, extension, format, or limit, asserting the actionable diagnostic. This is a test-visible seam, not a shipping code path that can be triggered accidentally.
- Synthetic profiles representing the named baseline classes are constructed from published device data and run against the required-set declaration, so a required capability those devices lack is caught at the profile level even though no such device is present. A profile check is **not** a driver test and must never be reported as one.
- The Vulkan Configurator / profiles layer is used to constrain the reported API version and feature set to the 1.2 floor where it can, so the renderer is exercised against the floor it claims rather than against a 1.4-era device's full capability set.

This mitigation makes the *implementation* verifiable. It does not verify the *drivers*. That distinction is preserved in every report.

### Non-gating, device-bound measurements

Frame time was already non-gating in P1b by user decision; it is recorded as the M2 baseline. Under this plan it is recorded on the devices that exist:

- **Discrete:** RTX 4060 Laptop at 1920×1080, low and medium. Recorded under its own name. It is **not** a GTX 1060 or RX 580 result and must never be presented as a floor, a proxy, or a bound for either.
- **Integrated:** Intel UHD Graphics (Alder Lake-P) at 1280×720, low. Recorded under its own name, as a third integrated data point alongside the still-unmeasured UHD 630 and Vega 8.

Peak process memory, committed GPU memory, allocation counts, and upload volume remain mandatory and are recorded per device.

## CPU evidence for increment B2

**Added 2026-08-14, when the user authorized increment B2.** The original plan left this open deliberately: the craft-physics (4 ms p95 at 300 parts) and resource-network (1 ms p95) gates are specified against "the accepted baseline CPU classes" — i5-8400 and Ryzen 5 2600 — the available i7-12650H is materially faster than both, and the same no-substitution rule applies on the CPU side. Deciding it before B2 was authorized would have pre-empted that authorization.

The user settled it by **extending this plan rather than amending the baseline CPU classes**, consistent with the same choice declined on the GPU side on 2026-08-13. The thresholds are unchanged.

### The gap, stated precisely

| Plan class | Cores / threads | Base / boost | Available? | Usable as a substitute? |
|---|---|---|---|---|
| Intel Core i5-8400 | 6C / 6T, homogeneous Coffee Lake | 2.8 / 4.0 GHz | **No** | **No.** Four microarchitecture generations older, no SMT, DDR4. |
| AMD Ryzen 5 2600 | 6C / 12T, Zen+ | 3.4 / 3.9 GHz | **No** | **No.** No AMD CPU is present, as no AMD device of any kind is. |
| Intel Core i7-12650H | 6 P-cores / 12 threads + 4 E-cores, hybrid Alder Lake | 2.3 / 4.7 GHz P-core | Present | **No.** Faster per clock and per core, hybrid rather than homogeneous, DDR5, larger caches. |

The hybrid topology is the part that makes this harder than the GPU case rather than merely analogous to it. An i5-8400 has six identical cores; the i7-12650H has two core types with different IPC, different clock ceilings, and a scheduler that moves work between them. A thread that migrates from a P-core to an E-core mid-measurement produces a p99 that reflects Windows' scheduling policy, not the craft solver.

### What is measured

1. **Unconstrained run, under its own name.** Full distribution — p50, p95, p99, maximum, at least 10 000 post-warm-up samples per the shared measurement rules — on the i7-12650H, reported as an i7-12650H result. This is the primary datum and it is **not** a baseline-class result.
2. **Constrained-CPU control.** The same scenarios with the process pinned to **6 P-cores, one thread per core** (SMT siblings and all E-cores excluded by affinity mask) and the processor's maximum performance state capped so the sustained clock approximates the i5-8400's envelope. This is the analogue of B1's synthetic capability profiles: it makes a gate testable that the available hardware would otherwise pass trivially.
3. **Both are reported for both craft representations**, at both 150 and 300 parts. A comparison run on a differently-constrained machine compares nothing.

Every report records the affinity mask actually applied, the achieved sustained clock read back rather than requested, the power plan, AC/battery state, and whether the run thermally throttled — the laptop hazards below bite far harder on a sustained CPU benchmark than on a GPU one.

### What a constrained result may and may not claim

**A constrained run is not an i5-8400.** Core count and clock are the two variables this method can control; they are not the two that dominate the difference. Golden Cove's IPC advantage over Coffee Lake, DDR5 against DDR4 bandwidth and latency, and a materially larger cache hierarchy all survive the constraint, and all favour the present machine. A physics solver at 300 parts is cache- and memory-sensitive, so this residual is not small.

The constrained figure is therefore a **bound from one direction with named residuals**, not a proxy. Concretely:

- **Constrained run passes:** the baseline-class claim is supported by a constrained bound, recorded with its residuals, and remains **unverified on hardware** until discharged as below. It is not written as a baseline-class pass.
- **Constrained run fails while the unconstrained run passes:** that is a real result and the strongest signal this hardware can produce. It triggers the P1b plan's narrow-or-reject conversation for B2 rather than being averaged away or reported as a pass.
- **Unconstrained run fails:** the representation is rejected outright. No constraint can rescue it, since the available machine is the faster one.

### How the unverified CPU claim is discharged

The same three paths as the GPU classes, with the same preference order: borrowed or remote measurement on a real i5-8400 or Ryzen 5 2600; a user-approved change to the baseline CPU classes; or a recorded deliberate deferral. An i7-12650H result, constrained or not, is never a discharge for any of them.

### Determinism interaction

ADR 0010's bit-exactness guarantee is per-build and per-machine, and affinity pinning must not disturb it. If B2's solvers are single-threaded, this is trivially satisfied and should be stated rather than assumed. If any solver is threaded, the determinism runs required by B2's done criteria are performed under both the constrained and unconstrained configurations, since a work-distribution scheme that is deterministic on twelve threads and not on six is a defect this increment should surface rather than inherit into P2.

## Measurement hazards

The single available machine is a laptop with hybrid graphics. These are not incidental:

- **Thermal and power throttling.** Sustained runs of 10,000+ samples will throttle where a desktop would not. Every performance report records the power plan, AC/battery state, and whether the run was thermally limited; a run that throttled is reported as throttled rather than averaged into a clean figure.
- **NVIDIA Dynamic Boost and variable TGP.** The RTX 4060 Laptop's power budget shifts with CPU load, so its frame time is not a fixed-hardware quantity. This alone disqualifies it as a comparison baseline, independently of it being the wrong class.
- **Adapter selection.** Which GPU actually serves a surface on a hybrid system is a function of the driver's application profile and the presentation path. Every report records the physical device actually selected, read back from the Vulkan device properties rather than assumed.
- **Display topology.** The integrated GPU drives the internal panel; presenting from the discrete GPU may involve a cross-adapter copy. Where this affects a measurement it is recorded.

Jitter, depth, LOD, and capability results are unaffected by throttling. Only the frame-time and memory figures are exposed to it.

## What this plan does not permit

- Claiming support, or a performance floor, for the GTX 1060 6 GB, RX 580 8 GB, UHD 630, or Vega 8 classes on the strength of these measurements.
- Reporting an RTX 4060 result as a baseline-class result, or a "conservative" one, in any document.
- Closing ADR 0002 on a claim that requires AMD driver evidence. ADR 0002's disposition may close on precision, capability, tooling, and the documented Direct3D 12 analysis — all of which are obtainable here — but any clause asserting AMD driver behavior must be stated as unverified or omitted.
- Treating the synthetic capability profiles as device testing.
- Claiming support, or a performance floor, for the i5-8400 or Ryzen 5 2600 classes on the strength of an i7-12650H measurement, constrained or unconstrained.
- Reporting a constrained-CPU run as a baseline-class result, or treating the constraint as having removed the IPC, memory, and cache differences it cannot touch.

## How the unverified claims are discharged

Until real evidence exists on those device classes, `docs/project_status.md`, the README, and ADR 0002 must describe the baseline-tier support claims as **unverified on hardware**. The claims are discharged by one of:

1. **Borrowed or remote hardware.** A measurement run on a real 1060- or 580-class device, and on a real UHD 630 or Vega 8, following the same scenarios and reporting rules. This is the preferred discharge and is a P1b-closure follow-up, not a P1b blocker.
2. **A user-approved change to the baseline classes.** If the project's support claim moves to hardware that is actually available, the P1b plan and ADR 0002 are amended by a planning update and this plan is superseded.
3. **A recorded, deliberate deferral to M2**, where the frame-time gate lives anyway, with the support claim withheld from any public statement until then.

An RTX 4060 result is never a discharge for any of them.

## Effect on P1b closure

P1b may close under this plan when its gating thresholds have reproducible results on the available devices and the capability-rejection mitigation above is verified. The exit criterion "integrated-graphics status is honest and does not block closure" is read to include the discrete baseline classes under this plan: **unmeasured is a recorded state, not a pass**.

The P1b evidence index must carry an explicit, prominent statement of which device classes were never measured.

## Decisions

| Decision | Status | Why | Date / source |
|---|---|---|---|
| Proceed with a documented evidence plan rather than acquiring baseline hardware first | Confirmed | User selected this option when authorizing P1b; the gating thresholds are largely device-portable and the frame-time rows were already non-gating | User, 2026-08-13 |
| RTX 4060 Laptop and Alder Lake-P UHD are measured under their own names, never as proxies | Confirmed | The P1b plan forbids substituting stronger hardware and extrapolating | [P1b plan](P1b-Renderer-and-Craft.md), risks section |
| Capability rejection verified by negative control and synthetic baseline profiles | Confirmed | No present device triggers the rejection path; the gate would otherwise be untestable | This plan |
| Baseline-tier support claims marked unverified on hardware until borrowed/remote evidence exists | Confirmed | Prevents an availability gap from silently becoming a support claim | This plan |
| No AMD driver evidence is obtainable in P1b | Confirmed | No AMD device is present | Hardware inventory above |
| B2's CPU-class substitution question | **Confirmed** — settled by extending this plan with a [CPU section](#cpu-evidence-for-increment-b2), not by amending the baseline CPU classes | Mirrors the GPU side: measure under the device's own name, and make the gate testable by a constrained-CPU control as the analogue of the synthetic capability profiles. Amending the classes was declined on the GPU side and is declined here for the same reason | User, 2026-08-14, at B2's authorization |
| A constrained-CPU run is a bound with named residuals, never a proxy | Confirmed | Affinity and clock are controllable; IPC, DDR5 bandwidth, and cache size are not, and all favour the present machine. A constrained pass leaves the baseline claim unverified on hardware | User, 2026-08-14 |
| Baseline CPU classes themselves | Unchanged | Same position as the GPU classes: amending them is a planning change the user declined | User, 2026-08-14 |
| Baseline GPU classes themselves | Unchanged | Amending them is a planning change the user declined to make at authorization time | User, 2026-08-13 |
