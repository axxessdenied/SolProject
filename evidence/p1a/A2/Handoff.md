# P1a increment A2 — handoff record

Required at increment closure by the P1a milestone plan and the `AGENTS.md` lightweight-lane
rule.

## Goal and outcome

Establish explicit-unit and explicit-frame types, compare at least two candidate frame-graph
models on conversion cost, precision, and ergonomics, build the ADR 0008 fixtures, measure
per-boundary conversion error along a surface-to-200-km trajectory, and state a precision budget
as measured numbers.

**Outcome: achieved.** Every A2 done criterion has a reproducible result. One frame model is
selected. Four findings are recorded in [Index.md](Index.md); the two that required a user
decision were resolved on 2026-08-12 and both ADRs amended. No open decision remains from A2.

## Owner, branch, base

- Single writer: Claude.
- **Working tree of `dev`, uncommitted.** `feature/p1a-precision-and-orbit` was merged and
  deleted when A1 landed as PR #1, and `AGENTS.md` forbids creating a branch without an explicit
  request. Every report accordingly records `gitDirty: true` against parent commit `185f6e8`.
  Re-running the evidence commands after a commit produces reports with `gitDirty: false`; the
  numbers do not change.
- **Scope note.** `docs/project_status.md` recorded A2 as blocked on user review of A1's
  evidence, with authorization scoped to A1 only. The user's direct instruction to implement A2
  is taken as the authorization, and the status document is updated to say so. Commit, push,
  merge, tag, and pull request remain unrequested and were not performed.

## Changed files

New:

```
fixtures/p1a/Provenance.md
fixtures/p1a/kernels/{naif0012.tls,pck00011.tpc,gm_de440.tpc}
fixtures/p1a/horizons/{ssb-sun,ssb-emb,ssb-earth,ssb-moon}.txt
prototypes/p1a/Frames/CMakeLists.txt
prototypes/p1a/Frames/include/Sol/Proto/Frames/{AscentProfile,Ellipsoid,FlatFrameModel,
  FrameGraph,FrameId,FrameTransform,HierarchicalFrameModel,Mat3,ReferenceData,Sha256,
  TextKernel,TimeScales,Units,Vec3}.h
prototypes/p1a/Frames/src/{AscentProfile,Ellipsoid,FlatFrameModel,FrameGraph,FrameTransform,
  HierarchicalFrameModel,ReferenceData,Sha256,TextKernel,TimeScales,Units}.cpp
prototypes/p1a/{FramesSelfCheck,ReferenceFixtures,FrameRoundTrip,AscentSampling,
  PrecisionBudget,FrameModelCost}/Main.cpp
evidence/p1a/A2/{Index.md,Handoff.md}
```

Modified: `prototypes/p1a/CMakeLists.txt`, `docs/project_status.md`, `docs/architecture.md`,
`docs/changelog.md`, `docs/decisions/0008-astronomical-reference-data-and-time-boundary.md`,
`docs/decisions/0010-determinism-and-floating-point.md`,
`SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md`, `evidence/p1a/A1/Index.md`.

Deleted: `prototypes/p1a/TimingScenario/Main.cpp`.

**Dependency changes: none.** P1a remains dependency-free and no `vcpkg.json` exists, so ADR
0007's workflow was not triggered. Two things were implemented rather than taken from a library,
each with a stated reason in its header: SHA-256 (about a hundred lines of well-specified
arithmetic, verified against published vectors, versus a dependency review) and a SPICE
text-kernel reader for the subset the three pinned kernels use.

## Literal commands

Environment as recorded in A1's handoff: `INCLUDE` and `LIB` were already present in the shell.
On a shell without them, run `Launch-VsDevShell.ps1 -Arch amd64` first.

```powershell
# Reference-data acquisition. Run once; the fixtures are frozen and checksum-verified
# thereafter. Requires network access.
#   NAIF kernels
curl.exe -sS -f -o fixtures/p1a/kernels/naif0012.tls https://naif.jpl.nasa.gov/pub/naif/generic_kernels/lsk/naif0012.tls
curl.exe -sS -f -o fixtures/p1a/kernels/gm_de440.tpc https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/gm_de440.tpc
curl.exe -sS -f -o fixtures/p1a/kernels/pck00011.tpc https://naif.jpl.nasa.gov/pub/naif/generic_kernels/pck/pck00011.tpc
#   Horizons state vectors: see fixtures/p1a/Provenance.md for the full parameter table.

# Clean tree
Remove-Item -Recurse -Force build

# Debug
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug

# Release
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release

# Evidence capture
$rel = "build\windows-msvc-release\prototypes\p1a"
foreach ($s in @("ReferenceFixtures","FrameRoundTrip","AscentSampling","PrecisionBudget","FrameModelCost")) {
  & "$rel\$s.exe" --out "evidence\p1a\A2\raw\release-$s.json"
}
```

Each scenario also accepts `--fixtures <dir>` to override the fixture root baked in at configure
time.

## Results

```
Debug   : 100% tests passed out of 12
Release : 100% tests passed out of 12
```

Five A1 tests — six, less `TimingScenario`, removed below — plus seven added by A2:
`FramesSelfCheck`, `ReferenceFixtures`, `FrameRoundTrip`, `AscentSampling`, `PrecisionBudget`,
`FrameModelCost`, and `FrameRoundTripAcrossRuns`.

`FramesSelfCheck` alone runs 95 checks. The A1 caveat still applies: do **not** run
`ctest --preset windows-msvc-release-negcontrol-contract`, which fails `ToolchainReport` by
design.

Hardware, toolchain, thresholds, and summary metrics are tabulated in [Index.md](Index.md).

## Failed or waived criteria

None. No threshold was relaxed and no criterion was waived.

Three reporting defects were found in A2's own output and fixed before the numbers were treated
as evidence. All three were caught by reading the first captured reports rather than by a test,
which is worth noting as a limitation of the test suite:

1. **Rebuild cost was reported as `0.0 ns`.** The performance counter's resolution is 100 ns and
   a hierarchical rebuild is 11.7 ns, so the measurement was absent rather than fast. Rebuilds
   are now batched 512 per sample and a check asserts the reported minimum is above zero. This is
   the same failure mode as A1's contraction probe: an instrument that silently measured nothing.
2. **The drift result was surprising and unexplained.** Iterated and stateless drift came out
   bit-identical, and the plausible prediction had been √N growth. Rather than report the
   coincidence, the scenario now measures iterations-to-bitwise-fixed-point directly and finds
   it is 1. The bound is a property of the arithmetic, not of the run length.
3. **The precision budget summed a term the implementation avoids.** The unreduced
   rotation-angle cost was included in the total, overstating it 6.7× and mislabelling headroom
   as 31× when the implemented path has 208×. It is now reported separately as the price of
   dropping the reduction.
4. **Git would have corrupted the fixtures on every fresh clone.** The repository sets
   `core.autocrlf=true` with `* text=auto`, so the checked-in kernels and Horizons tables — which
   are text, and whose SHA-256 digests are compiled into `ReferenceData.cpp` — would have been
   rewritten to CRLF on checkout. Every byte after the first line changes, every digest fails,
   and nothing in the increment runs. Caught from `git add`'s line-ending warnings before the
   first commit. `.gitattributes` now marks `fixtures/p1a/kernels/**` and
   `fixtures/p1a/horizons/**` as `-text`, and the stored blobs were hashed to confirm they match
   the recorded constants exactly.

   Worth noting for the design: the checksum verification turned this from silent corruption into
   a loud refusal to start. That is the behaviour it was built for, but it would still have
   broken the build for every clone, and no test in the suite would have caught it — the fixtures
   are correct on the machine that downloaded them.

Two expectations in `FramesSelfCheck` were also wrong when first written — a tolerance that
ignored cancellation in a 5 m difference of two 6.37 × 10⁶ m vectors, and an off-by-one
transform-application count. Both were corrections to the checks, not to the library.

## Remaining risks

- **Origin motion is linear, not an ephemeris.** A2 has one fixture epoch and extrapolates
  celestial origins linearly across the 500 s ascent window. This is exactly self-consistent,
  which is all a frame round trip can be sensitive to, and it is *not* astronomically accurate:
  neglected curvature displaces Earth's barycentric position by several hundred metres over the
  window. Nothing A2 concludes depends on it and nothing A2 concludes may be quoted as an
  ephemeris result. **A3 must replace this**, since propagation is its subject under ADR 0011.
- **Earth orientation is IAU_EARTH, not ITRF.** No nutation, polar motion, or UT1−UTC; can differ
  from true Earth attitude by tens of metres at the surface. Adequate for measuring the numerics
  of a rotating-frame boundary, useless for navigation. The production model remains open in
  `docs/architecture.md`.
- **The geoid gap is unclosed.** Finding 3: "5 m above mean sea level" is interpreted as height
  above the ellipsoid, and the geoid is roughly −30 m from it in this region. Four orders of
  magnitude past the position budget.
- **Single-machine evidence.** One i7-12650H. ADR 0010 only promises same-machine bit-exactness,
  so this is in scope, but no cross-machine tolerance data exists yet — and A2 now *has* pinned
  cross-run goldens, which will need tolerance-based equivalents the first time this runs on
  other hardware.
- **The selection is reopenable by P1b.** B1's screen-space jitter gate is evaluated against the
  model A2 selected, and the P1a plan makes a B1 failure a reason to revisit A2 rather than a
  P1a failure.
- **Fixture root is an absolute path baked in at configure time.** Fine for a disposable
  prototype whose evidence is produced on the machine that built it; a shipping target would
  resolve data relative to the executable. Every scenario accepts `--fixtures` to override.

## Disposable code

`TimingScenario` was marked disposable in A1 and described there as "replaced in A2".
`FrameModelCost` now does its job against real frame conversions, and **`TimingScenario` was
removed on 2026-08-12 with user approval.** The P1a plan permits removing rejected prototype code
once its evidence and decision record are retained and the removal scope is reviewed: its
evidence stays in [A1's index](../A1/Index.md), which carries a dated addendum noting the
deletion, and the scope was one directory and one CTest registration.

`prototypes/p1a/Frames/` is the substantial new code. Unlike the harness it is **not** free of
domain concepts — it knows about ellipsoids, ephemeris fixtures, and Earth orientation — so
promoting it is a larger decision than promoting the harness, not a smaller one. The parts most
likely to survive review are `Units`, `Vec3`, `Mat3`, `FrameTransform`, and the hierarchical
model; the parts most likely to be replaced are `TextKernel` and `Sha256`, which exist only
because P1a is dependency-free.

## Smallest next action

A3 — hybrid orbit and time warp — is the next increment, and it is **not** authorized. A2's
authorization came from a direct instruction covering A2.

One thing carries into A3: **the linear origin-motion model must be replaced** with real
propagation under ADR 0011. A3 owns this; A2 deliberately did not.

**No open decision remains.** All four raised by A1 and A2 were resolved on 2026-08-12:

| Decision | Resolution |
|---|---|
| DE441 versus DE440 | ADR 0008 amended to name the DE440/DE441 family; each fixture records its product |
| "5 m above mean sea level" | ADR 0008 amended to define the anchor against the reference ellipsoid, with the datum required to travel with the coordinate |
| ADR 0010 contraction wording | Amended in place: prefer an explicit flag, permit the default only when a negative control proves it |
| `TimingScenario` | Removed; evidence retained in A1's index |

A3 remains unauthorized.
