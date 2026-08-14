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
  - **Addendum, 2026-08-14:** B2 has since been authorized, on its own branch
    `feature/p1b-craft-and-resources` from `dev`. This changes nothing about B1's scope or state:
    the user directed that **B1 be finished first**, and B2 is authorized and queued rather than
    started. No B1 gate is waived by it. See
    [`docs/project_status.md`](../../../docs/project_status.md#p1b-increment-b2-implementation-authorization-2026-08-14).
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

Both report **29/29 passed, none disabled**. The individual executables under
`build/windows-msvc-<config>/tests/render/` were run directly to capture `raw/`, because CTest
records pass or fail rather than the printed measurements.

`ctest` runs the memory traverse for 45 seconds, plus its negative control, which keeps the
instrument under test without putting half an hour on the daily loop. The 30-minute runs the
milestone clause names are performed deliberately, in Release only, and are the ones transcribed
into [Index.md](Index.md):

```
build/windows-msvc-release/tests/render/SolMemoryTraverse.exe --minutes 30 \
  > evidence/p1b/B1/raw/release-MemoryTraverse-30min.txt

build/windows-msvc-release/tests/render/SolMemoryTraverse.exe --minutes 30 --leak 8 \
  > evidence/p1b/B1/raw/release-MemoryTraverse-30min-leak8.txt
```

They re-measure rather than reproduce: the figures are process memory over wall clock, so a repeat
run gives close but not identical numbers. The whole-window trend in particular has fitted 17.9,
40.7, 56.8 and 25.7 KiB/minute across four clean runs, which is why the gated statistic is the
second-half trend and not that one.

Two gates carry their own negative controls and both were re-verified rather than assumed: the
depth gate's conventional projection collapses at 10 km, and the jitter gate's sub-pixel response
control steps the axis carrying the world magnitude. Three renderer contract tests were verified
by reverting the fix and confirming the test fails.

## Validation performed

| Check | Result |
|---|---|
| `ctest`, both configurations | 29/29, none disabled |
| Both builds under `/W4 /WX` | Clean; the only warnings are the documented `/W0` exception on two third-party bodies |
| Debug vs Release raw output | **Byte-identical across all seven deterministic render executables**, apart from the shader provenance line. `SolMemoryTraverse` is excluded and not comparable: it is driven by wall clock and reports process memory, so no two runs of it agree even in one configuration |
| Jitter gate | PASS, with its precision control |
| Depth gate | PASS, with its collapsing negative control |
| LOD gate | PASS under the method ratified 2026-08-14, with its qualification printed |
| Capability check | 23 checks, 0 failed |
| Renderer contracts | 14 checks, 0 failed |
| Memory traverse, 30 minutes | **PASS** under the method ratified 2026-08-14, Release: 256 954 frames, second-half trend 15.9 KiB/min (limit 64), growth 0.98 MiB (limit 2) |
| Memory traverse negative control | 8 B/frame over 30 min fails both criteria (271.7 KiB/min, 6.47 MiB). Suite runs a 4 KiB/frame control at 45 s continuously |
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
- **The memory pass bounds a rate rather than proving an asymptote.** No finite window can prove
  one, the gate cannot see growth below its noise floor at 30 minutes, and the gated second-half
  statistic has only three clean observations so far (12.2, 2.1 and 15.9 KiB/minute). The method states
  these limits and the program prints them with the verdict.
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
3. ~~Define what "no unbounded memory growth" means.~~ **Done 2026-08-14**: the method is defined,
   ratified, and passing, with its controls and stated limits in the milestone plan. What remains
   of this clause is the same thing that remains of every other gate — the second device.
4. Build the atmosphere and the scalable quality settings, or invoke the P1b narrowing option —
   noting that an analytic shell has no LOD transitions, so "atmosphere LOD popping" becomes
   vacuous under it and must be stated rather than quietly satisfied.
5. Produce the Direct3D 12 comparison analysis and ADR 0002's disposition.
6. Add licence notices for the four packages before anything is distributed.

Committing, pushing, merging, tagging and opening a pull request each remain separate explicit
user requests under `AGENTS.md`. PR #6 exists at the user's request and is not merged.
