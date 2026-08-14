# P1b increment B1 — handoff record

Required at increment closure by the P1b milestone plan and the `AGENTS.md` lightweight-lane
rule. **B1 is not closed**, so this is the mid-increment form of that record: enough state that
the work survives a fresh session without anyone re-deriving it from commit messages.

## Goal and outcome

Build a Vulkan renderer that holds precision and depth from a planetary surface to orbit, with
continuous terrain level of detail and honest capability reporting — the first code in the
production tree rather than under `prototypes/`.

**Outcome: partial, and the boundary is sharp.** Three gating thresholds are met on the one
device available; one is met in part and one cannot close. Several declared deliverables have not
been built. What is measured and what is not is set out in [Index.md](Index.md), which states the
unestablished claims as prominently as the passes.

## Owner, branch, base

- Single writer: **Claude**.
- Branch **`feature/p1b-vulkan-renderer`**, branched from `dev`, at the user's explicit request in
  the same instruction that granted implementation authorization on 2026-08-13.
- Authorization is scoped to **B1 only**. Increment B2 — constructed craft and resource networks —
  is **not authorized** and needs its own explicit instruction.
- Open as **PR #6** against `dev`, not merged. Nothing is tagged and nothing is on `main`.

## Reproducing the evidence

Everything in `raw/` comes from these commands. `raw/` is excluded by `.gitignore`; the numbers
that matter are transcribed into [Index.md](Index.md).

```
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug

cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

Both report **28/28 passed, none disabled**. The individual executables under
`build/windows-msvc-<config>/tests/render/` were run directly to capture `raw/`, because CTest
records pass or fail rather than the printed measurements.

`ctest` runs the memory traverse for 45 seconds, which keeps the instrument under test without
putting half an hour on the daily loop. The 30-minute run the milestone clause names is performed
deliberately, in Release only, and is the one transcribed into [Index.md](Index.md):

```
build/windows-msvc-release/tests/render/SolMemoryTraverse.exe --minutes 30 \
  > evidence/p1b/B1/raw/release-MemoryTraverse-30min.txt
```

It re-measures rather than reproduces: the figures are process memory over wall clock, so a
repeat run gives close but not identical numbers.

Two gates carry their own negative controls and both were re-verified rather than assumed: the
depth gate's conventional projection collapses at 10 km, and the jitter gate's sub-pixel response
control steps the axis carrying the world magnitude. Three renderer contract tests were verified
by reverting the fix and confirming the test fails.

## Validation performed

| Check | Result |
|---|---|
| `ctest`, both configurations | 28/28, none disabled |
| Both builds under `/W4 /WX` | Clean; the only warnings are the documented `/W0` exception on two third-party bodies |
| Debug vs Release raw output | **Byte-identical across all seven deterministic render executables**, apart from the shader provenance line. `SolMemoryTraverse` is excluded and not comparable: it is driven by wall clock and reports process memory, so no two runs of it agree even in one configuration |
| Jitter gate | PASS, with its precision control |
| Depth gate | PASS, with its collapsing negative control |
| LOD gate | PASS under the method ratified 2026-08-14, with its qualification printed |
| Capability check | 23 checks, 0 failed |
| Renderer contracts | 14 checks, 0 failed |
| Memory traverse, 30 minutes | Ran 2026-08-14, Release: 258 939 frames, 0.37 MiB growth after warm-up, 17.9 KiB/min trend. **Reported, not judged** — no threshold exists |
| Validation layers | 3 messages, all third-party, none from this project |

## Remaining risks

- **One device.** Every gate result is from the RTX 4060 Laptop GPU. The Intel UHD is unmeasured,
  which the accepted evidence plan requires and which is therefore outstanding rather than waived.
  No baseline-class or AMD hardware exists on this machine, so nothing here supports a
  baseline-tier claim.
- **The LOD pass is narrower than its plain reading.** The control is below the perceptual limit,
  so the scene does not pop visibly either way. Ratified on that basis; strengthening it needs a
  scene where the abrupt scheme pops visibly at a well-conditioned quality setting, and none has
  been found.
- **The 30-minute memory traverse has run and produced no verdict**, because the clause names no
  statistic and no limit. It measured 0.37 MiB of growth after warm-up at 17.9 KiB/minute, with
  every device-side figure constant. Whether that is "no unbounded growth" is a definition
  nobody has written down, and the shape of the growth is not recorded, so a flattening curve and
  a slow line are not yet distinguishable.
- **The capability profiles are hand-authored** and near-identical across every field the
  requirement consults. They are one assertion, not four, until reconciled against real reports.
- **GPU determinism is assumed.** ADR 0010 governs MSVC and the CPU. The bit-identical-frames and
  exact-depth-inequality results both depend on GPU and driver behaviour that no accepted decision
  covers. The Debug/Release byte-identity above is evidence against the risk, on one driver.
- **No frame-time evidence exists**, and the measurement path must never produce it.

## Next action

Not a code change. **B1's remaining scope needs a decision about what closes it**, because the
gap between "the gates pass" and "the increment is done" is now the largest thing in the
increment:

1. Reconcile the four synthetic capability profiles against real reports, or accept that the
   capability gate closes on the negative control alone and record why.
2. Measure both available devices, or record the Intel UHD as deliberately out of scope.
3. **Define what "no unbounded memory growth" means**, now that the traverse has run and produced
   a number nobody can grade. This is the same gap the popping half had, and it wants the same
   remedy: a documented planning update stating the statistic and the limit. Dumping the sample
   series and repeating the run would let a flattening curve be told apart from a slow line
   first, which is probably worth doing before fixing a limit.
4. Build the atmosphere and the scalable quality settings, or invoke the P1b narrowing option —
   noting that an analytic shell has no LOD transitions, so "atmosphere LOD popping" becomes
   vacuous under it and must be stated rather than quietly satisfied.
5. Produce the Direct3D 12 comparison analysis and ADR 0002's disposition.
6. Add licence notices for the four packages before anything is distributed.

Committing, pushing, merging, tagging and opening a pull request each remain separate explicit
user requests under `AGENTS.md`. PR #6 exists at the user's request and is not merged.
