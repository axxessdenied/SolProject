# P1b increment B1 — evidence index

**Increment:** B1, Vulkan large-world renderer
**Owner:** Claude (single writer)
**Branch / base:** `feature/p1b-vulkan-renderer`, branched from `dev` — see [Handoff.md](Handoff.md)
**Date:** 2026-08-14
**Result:** **B1 is not closed.** Three of the four gating thresholds it can reach are met and one
is met in part; several declared deliverables have not been built. This index is the evidence
record as it stands, written so the increment survives a fresh session, and it is explicit
throughout about which claims are measured and which are not.

Raw measurement output lives in `raw/`, which `.gitignore` excludes. It is reproducible from the
commands in [Handoff.md](Handoff.md); this index and the handoff are the durable record.

## Headline

**Four results, and the two that matter most are the ones that constrain what may be claimed.**

1. **Screen-space jitter is 0.000000 px at both required reference views**, frames bit-identical,
   against a 0.25 px gate. The number is worth something only because of the control beside it:
   the camera is stepped 10 mm at a world magnitude where one `float` ULP is 0.5 m, and the
   marker's centroid tracks the geometric prediction. A pipeline that had lost the precision
   would also have scored zero, by being stably wrong.
2. **Depth holds across the full surface-to-orbit path**, matching the analytic prediction
   `near / distance` to between 3e-9 and 1e-7 relative error from 1 m to 10 000 km, with
   resolvable separation scaling linearly rather than degrading. The conventional projection run
   through the identical harness **collapses at 10 km** and fails three of the checks, which is
   what makes the pass evidence rather than an assertion.
3. **Debug and Release produce byte-identical output across all seven deterministic render
   executables** — every one whose output is a function of its inputs; `SolMemoryTraverse` is
   driven by wall clock and reports process memory, so it is excluded rather than failing. This
   was not expected and it retires a recorded risk: the two configurations compile different
   SPIR-V (`-g -O0` against `-O`), `spirv-opt` may reassociate floating-point arithmetic, and ADR
   0010 governs MSVC while saying nothing about the GPU. Measured, the divergence changes nothing.
4. **The LOD gate passes, and certifies less than its plain reading suggests.** The production
   path moves 0.000101 of the frame at the worst step of an isolated transition against a 0.0020
   perceptual limit, with the control separating 4.4×. But the control is *also* below the limit,
   so this scene does not pop visibly either way: the pass is a margin and a response, not a
   rescue. Ratified on that basis by the user on 2026-08-14.

## Toolchain and host

| Item | Recorded value |
|---|---|
| Compiler | MSVC, `/fp:precise /arch:AVX2`, `/W4 /WX` on project targets |
| Vulkan SDK | 1.4.357.0, pinned; `glslc --target-env=vulkan1.2` |
| Shader flags | `-g -O0` (Debug), `-O` (Release) — printed by every gate |
| Loader | instance API 1.4.357, 19 layers, validation installed and active |
| OS | Windows 10.0.26200 |
| CPU | 12th Gen Intel Core i7-12650H |
| GPUs present | NVIDIA RTX 4060 Laptop (discrete, API 1.4.312, driver 581.15.0.0) and Intel UHD Graphics (Alder Lake-P) |
| Device used for every gate | **RTX 4060 Laptop only** |
| Resolution | 1280×720, except the contract tests at 640×480 |

**Every gate result below is from one device.** The accepted [reference-hardware evidence
plan](../../../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) requires the
gating thresholds on *both* available devices, and the Intel UHD is unmeasured. That is
outstanding, not waived. No baseline-class or AMD hardware exists on this machine at all, so no
result here is a baseline-tier claim.

## Gate results

| Gate | Verdict | Evidence |
|---|---|---|
| Screen-space jitter, 0.25 px | **PASS**, RTX 4060 only | `raw/{debug,release}-JitterHarness.txt` |
| Depth behaviour | **PASS**, RTX 4060 only | `raw/{debug,release}-DepthGate.txt` |
| LOD continuity — popping | **PASS** under the ratified method, RTX 4060 only | `raw/{debug,release}-LodGate.txt` |
| LOD continuity — memory | **PASS** under the method ratified 2026-08-14, RTX 4060 only | `raw/release-MemoryTraverse-30min*.txt`, `raw/release-MemoryTraverse-control-45s.txt` |
| Capability reporting | **Cannot close** until synthetic profiles are reconciled | `raw/*-RenderCapabilityTests.txt`, `raw/*-RenderCapabilityReport.txt` |
| Validation output | **PASS with an explained exception** | every raw file |

### Screen-space jitter

0.000000 px maximum and p99 on both axes, at the surface anchor and the 200 km orbital vantage,
frames bit-for-bit identical. Gate is 0.25 px.

The **sub-pixel response control** is what carries the precision claim, and it did not always.
The control originally stepped the camera along an axis whose magnitude is at most 0.11 m, where
a `float` ULP is ~1e-8 m — a fully `float` pipeline passed it to within 4e-9 px. It now steps the
axis carrying the world magnitude, 6 378 141.6 m, where one `float` ULP is **0.5 m against a
10 mm step**, so a `float` pipeline would produce exactly zero screen motion for fifty
consecutive steps. Only then does a smooth response discriminate.

### Depth behaviour

| Distance | Relative error vs `near / distance` | Resolvable separation |
|---|---|---|
| 1 m | 7.8e-08 | 5.96e-08 m |
| 1 km | 5.1e-08 | 3.05e-05 m |
| 1 000 km | 3.1e-08 | 0.094 m |
| 6 378 km | 1.0e-07 | 0.150 m |
| 10 000 km | 2.8e-09 | 0.500 m |

`sep/dist` stays between 2.4e-08 and 9.4e-08 across seven orders of magnitude — linear scaling,
which is the property the threshold names. The **negative control**, a conventional finite-far
projection through the same harness, degrades from 5.96e-08 m at 1 m to 0.18 m at 1 km and
**collapses entirely at 10 km**; the harness records three collapses and fails the run.

Note that these separations are the *guaranteed* figures, one full ULP. An earlier revision
published best-case draws from a rounding boundary — 6 cm at 1 000 km and 15 cm at Earth's radius
— and those were withdrawn on 2026-08-13.

### LOD continuity

Measured by the method [ratified into the milestone
plan](../../../SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md) on 2026-08-14.

| | production | control | |
|---|---|---|---|
| Worst fraction of frame changing perceptibly in one step | 0.000101 | 0.000447 | limit 0.0020 |
| Mean maximum difference | 0.0150 | 0.0484 | |

Verdict PASS at a 20× margin, control separating 4.4× against a 3× requirement.

**The qualification is part of the result.** The control is itself below the perceptual limit, so
the scene does not pop visibly with or without morphing. What is certified is that the production
path sits far under the limit and that the metric responds strongly when the morph is switched
off. The gate prints this in its own output rather than leaving it to be found here.

Descent statistics, retained as diagnostics: 600 steps, 0 pops in both configurations, median
frame difference 0.0832 production against 0.0885 control, maxima 0.2014 against 0.3222. The
descent **cannot** isolate a transition and is not the verdict — established by measurement, not
argued: re-running at a 20° field of view tripled the screen-space error, grew every other
statistic's separation by roughly 3×, and still produced exactly zero pops in both configurations.

**Memory has now been measured over the stated 30 minutes** — by a separate program, because the
gate's reading was withdrawn for two reasons and only one of them was duration. The other was
that the figure it read *could not vary*: every device allocation happens once at creation and
capacity is fixed, so `max == min` is arithmetic. Running that same reading for thirty minutes
would have produced thirty minutes of the same tautology, so `render.memory-traverse` measures
**process commit charge**, which can move, and which is also the only instrument that can see a
host-side leak at all — VMA cannot, because VMA does not own host memory.

Release build, 2026-08-14, RTX 4060, wall-clock driven rather than step-driven:

Graded against the method **ratified by the user on 2026-08-14**:

| | production | 8 B/frame control |
|---|---|---|
| Duration | 30.0 min graded, 256 954 frames | 30.0 min graded, 256 628 frames |
| Process private bytes, start → end | 363.84 → 382.03 MiB | — |
| **Second-half trend** (gated, ≤ 64 KiB/min) | **15.9** — PASS | **271.7** — FAIL |
| **Growth after 120 s warm-up** (gated, ≤ 2 MiB) | **0.98 MiB** — PASS | **6.47 MiB** — FAIL |
| Whole-window trend (reported, not gated) | 25.7 KiB/min | 247.9 KiB/min |
| Device allocated / block bytes | 43.03 / 131.75 MiB, **constant** | constant |
| Live allocations / blocks | 9 / 4, **constant** | constant |

Camera path 3 km ↔ 300 km altitude every 90 s while orbiting at 0.004 rad/s, terrain reselected
throughout. Raw output in `raw/release-MemoryTraverse-30min.txt` and
`raw/release-MemoryTraverse-30min-leak8.txt`; the suite's 45-second control is
`raw/release-MemoryTraverse-control-45s.txt`.

**The gate passes and the control fails**, which is what makes the pass a measurement rather than
an instrument that is deaf.

**Why the whole-window trend is reported and not gated**, since it is the figure a reader would
otherwise take as the headline. It is contaminated by settling that continues past the 120 s cut,
and it is unstable: across four clean runs of the same build it fitted **17.9, 40.7, 56.8 and
25.7** KiB/minute, a 3.2× spread. The gated second-half statistic over the same runs sits at
**12.2, 2.1 and 15.9**, comfortably inside a 64 limit each time.

On one graded run the whole-window figure also came out *larger than either half it spans* — 56.8
against a first half of 15.3 and a second half of 2.1. That is the signature of a discrete step,
not an arithmetic error: a line fitted across a jump sits above lines fitted either side. Growth
after warm-up on that run was 1.00 MiB, so essentially all of it was one step — what a
fixed-capacity design settling looks like, and what a whole-window slope would have reported as a
steady 56.8 KiB/minute leak.

The four device-side figures were exactly constant for the whole run, expected under the
fixed-capacity design and recorded as falsification rather than confirmation.

**What this does not settle, stated because the numbers alone read stronger than they are.**

- **A pass does not prove an asymptote.** No finite window can, and the gate cannot see growth
  below its noise floor at 30 minutes. Both are properties of bounding a rate over a finite run,
  and the program prints them with the verdict rather than leaving them to be inferred.
- **One device, as everywhere else in B1.** The Intel UHD is unmeasured for this clause too.
- **The clean sample is small.** The gated second-half statistic has three observations — 12.2,
  2.1 and 15.9 KiB/minute — all inside a 64 limit, worst case a 4× margin. Firmer with more runs.
- **Duration is load-bearing, and the two controls are not interchangeable.** The 8-byte leak fits
  271.7 KiB/minute at 30 minutes and **−60.2** at 3 minutes, where noise swamps it entirely. The
  suite's continuously-run control leaks a gross 4 KiB/frame (35 451 KiB/minute) to clear the
  short-run noise floor; it proves the gate *can* fail, while how *small* a leak it catches rests
  on the 8-byte run at graded duration.

### Capability reporting

23 checks pass in the pure requirement test, which runs with no loader, no device and no window,
and is the negative control the evidence plan requires — it exercises device classes this project
does not own. The real machine enumerates both GPUs with loader, API, driver, features, limits,
formats, queues and memory.

**It cannot close.** The four synthetic baseline profiles are hand-authored and near-identical
across every field the requirement consults; they must be reconciled against real capability
reports from the named device classes before they support a support claim. The test says this in
its own output.

### Validation output

Three messages, identical in every run, all `LLP_LAYER_3` loader warnings naming
`GalaxyOverlayVkLayer` — a third-party overlay layer installed on this machine, not this
project's Vulkan usage. This is ADR 0002's "explained and accepted" category. **Zero messages
originate from SolRender.**

**A capture workflow is established** as of 2026-08-14, satisfying ADR 0002's capture clause. It
uses **GFXReconstruct**, which ships with the already-pinned Vulkan SDK 1.4.357.0, rather than
RenderDoc — so it adds no dependency. Verified end to end on `SolRenderLoop`, Release: 240 frames
captured, `gfxrecon-info` reporting application, device and resolution from the file, and
`gfxrecon-replay` replaying all 240 frames clean at 104.6 fps. The only replay warnings are the
same third-party overlay messages explained above. Raw output in `raw/release-CaptureWorkflow.txt`.

RenderDoc remains the better *interactive* frame debugger and is not installed on this machine.
The clause asks for a usable capture workflow, not specifically for RenderDoc.

### Renderer contract tests

14 checks, 0 failed. Not a threshold — these cover behaviours the gates depend on and none of them
measures, all of which were broken while the whole suite was green:

- A root terrain patch has no parent, so enabling the morph must change no pixel. Verified against
  the defect by reverting the shader guard: it moved 58 720 of 66 062 terrain pixels, 89% of the
  planet.
- A frame that drew nothing must say so and must not return the previous frame's pixels.
- A camera whose forward is parallel to up is refused rather than rendered at an arbitrary roll.
- Terrain outside the view frustum is not built: 108 patches looking at the planet, 0 looking away.

## Performance, recorded as data and not gated

Frame time is non-gating in P1b by user decision, and these are laptop figures subject to Dynamic
Boost, variable TGP and thermal limits — not fixed-hardware quantities.

| | Before | After |
|---|---|---|
| LOD gate wall time | 8 m 29.6 s | **1 m 59.2 s** |
| Peak terrain patches | 1 008 | **240** |
| Peak terrain vertices | 81 648 | **19 440** |
| Device allocation | 43.03 MiB | 43.03 MiB |

Two changes, both verified to leave the rendered image untouched. Coarse terrain samples are read
from the fine grid rather than recomputed — the same expression on the same operands, so
bit-identical — halving `surfacePoint` calls per patch from 162 to 81. And terrain is culled
against the view frustum, not only the horizon; because a conservative cull removes only what was
never visible, every image-derived statistic across the 600-step descent is identical either side
of it.

The depth attachment uses `STORE` rather than `DONT_CARE` so the depth gate can read it back. That
write bandwidth is included in any frame-time figure this renderer produces and is not netted out.

## What is not established

Recorded here so that a reader of the passes above does not infer more than was measured.

- **Nothing is measured on the Intel UHD**, and no baseline-class or AMD hardware exists here.
- **The memory pass bounds a rate, it does not prove an asymptote**, and cannot see growth below
  its noise floor at 30 minutes.
- **The capability profiles are unreconciled** against real reports.
- **A2's frame model is not confirmed by this increment.** `WorldVec3` is a bare `double` triple;
  nothing walks a frame graph to a lowest common ancestor or checks an epoch. What is exercised —
  subtract in `double`, then narrow — is common to A2's hierarchical model and to the global-root
  candidate it rejected. The P1b done-criterion is not met.
- **The GPU does receive large magnitudes** on the reference-object path, whose vertex positions
  are formed on the GPU from an origin and a radius. The claim that it never sees a world
  coordinate holds for terrain only.
- **No frame-time evidence exists**, and the capture path must never produce it: it waits for the
  GPU before reading, removing the pipelining that makes frame time meaningful.
- **Deliverables not built:** the atmosphere, representative scalable quality settings including a
  conservative low tier, the documented Direct3D 12 comparison analysis and ADR 0002's
  disposition, and licence notices for the four packages.

## Findings

Ten findings from the second review round, all applied — the round reviewed the whole open PR
including the commit that applied the *first* round's findings, which none of those reviewers saw.
Detail in [`docs/project_status.md`](../../../docs/project_status.md). The three that changed
what may be claimed:

1. **Root terrain patches were pinned to full coarse morph.** A level-0 node has no parent and the
   CPU signals that with a zero-width morph band; the shader widened it to an epsilon and drove
   the factor to 1. Out of reach of every harness, in reach of the renderer from geostationary
   altitude.
2. **A minimised window returned the previous frame's pixels** from the capture path as a
   well-formed frame. In the jitter gate a duplicate contributes zero centroid deviation and
   *strengthens* the bit-identical reading.
3. **The terrain quality default was below the scheme's own validity floor** — 2.5 against a
   documented ~2.8 — while the gate measured at 3.0 and a comment claimed 3.0 was the default. A
   `static_assert` now ties the two together.
