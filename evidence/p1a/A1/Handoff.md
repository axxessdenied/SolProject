# P1a increment A1 — handoff record

Required at increment closure by the P1a milestone plan and the `AGENTS.md` lightweight-lane
rule.

## Goal and outcome

Stand up the smallest C++23/MSVC/CMake/Ninja build graph capable of producing trustworthy
measurements, apply and verify the ADR 0010 floating-point policy, and deliver a
metrics/metadata format that makes A2's precision numbers and A3's orbital numbers
reproducible.

**Outcome: achieved.** Every A1 done criterion has a reproducible result. Three findings are
recorded in [Index.md](Index.md); one of them changes what ADR 0010 can claim.

## Owner, branch, base

- Single writer: Claude.
- Branch `feature/p1a-precision-and-orbit`, based on `dev`.
- Implementation authorization granted by the user on 2026-08-12, scoped to A1.
- **Nothing has been committed.** Branch creation was authorized; commit, push, merge, tag,
  and PR were not. The working tree is dirty, and every evidence report accordingly records
  `gitDirty: true` against parent commit `9df0c47`. Re-running the evidence commands after a
  commit will produce reports with `gitDirty: false`; the numbers will not change.

## Changed files

New:

```
CMakeLists.txt
CMakePresets.json
cmake/SolProjectOptions.cmake
cmake/SolToolchainFacts.cmake
cmake/SolToolchainFacts.h.in
cmake/RunDeterminismCheck.cmake
prototypes/p1a/CMakeLists.txt
prototypes/p1a/Harness/CMakeLists.txt
prototypes/p1a/Harness/include/Sol/Proto/Harness/{AllocationCounter,Check,HostInfo,JsonWriter,MetricSeries,ProcessMetrics,ScenarioReport}.h
prototypes/p1a/Harness/src/{AllocationCounter,Check,HostInfo,JsonWriter,MetricSeries,ProcessMetrics,ScenarioReport}.cpp
prototypes/p1a/{Cpp23Conformance,HarnessSelfCheck,ToolchainReport,DeterminismSmoke,TimingScenario}/Main.cpp
evidence/p1a/A1/{Index.md,Handoff.md}
```

Modified: `docs/project_status.md`, `docs/architecture.md`, `docs/changelog.md`,
`docs/decisions/0010-determinism-and-floating-point.md`.

**Dependency changes: none.** P1a is deliberately dependency-free, so ADR 0007's workflow was
not triggered and no `vcpkg.json` exists. The only external link inputs are the Windows SDK
libraries `psapi` and `ntdll` (the latter via runtime `GetProcAddress`), which are platform
SDK inputs rather than packages.

## Literal commands

Environment: `INCLUDE` and `LIB` were already present in the shell, so no developer-prompt
activation step was required. **This is a property of this machine's environment, not of the
project.** On a shell without them, run
`& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64`
first. `VSCMD_VER` is empty here, which means the variables were set globally rather than by
`vcvars64.bat`; a future clean machine should not assume this.

```powershell
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
build\windows-msvc-release\prototypes\p1a\ToolchainReport.exe   --out evidence\p1a\A1\raw\release-ToolchainReport.json
build\windows-msvc-release\prototypes\p1a\DeterminismSmoke.exe  --out evidence\p1a\A1\raw\release-DeterminismSmoke.json
build\windows-msvc-release\prototypes\p1a\TimingScenario.exe    --out evidence\p1a\A1\raw\release-TimingScenario.json

# ADR 0010 negative control
cmake --preset windows-msvc-release-negcontrol-contract
cmake --build --preset windows-msvc-release-negcontrol-contract
build\windows-msvc-release-negcontrol-contract\prototypes\p1a\DeterminismSmoke.exe --out evidence\p1a\A1\raw\negcontrol-DeterminismSmoke.json
```

## Results

```
Debug   : 100% tests passed out of 6   (0.36 s)
Release : 100% tests passed out of 6   (0.33 s)
```

Tests: `Cpp23Conformance`, `HarnessSelfCheck`, `ToolchainReport`, `DeterminismSmoke`,
`TimingScenario`, `DeterminismAcrossRuns`.

Do **not** run `ctest --preset windows-msvc-release-negcontrol-contract`. That build fails
`ToolchainReport` on purpose — the report asserts an evidence build is not a negative control.
Run its `DeterminismSmoke.exe` directly, as above.

Hardware, toolchain, thresholds, and summary metrics are tabulated in [Index.md](Index.md).

## Failed or waived criteria

None. No threshold was relaxed and no criterion was waived.

Two defects were found and fixed during the increment rather than carried:

1. **`JsonWriter` wrote every C-string as `true`.** `const char*` bound to the `bool`
   overload in preference to `string_view`, so the entire provenance block of every report
   was corrupted while the build and all tests passed. Fixed with an exact-match `const
   char*` overload, and `HarnessSelfCheck` was added specifically so this class of silent
   corruption cannot recur.
2. **The original `contractionProbe` did not detect contraction.** It produced identical
   output with and without `/fp:contract`. Replaced with the standard FMA detector, which now
   yields exactly `0.0` versus exactly `-e^2`. See [Index.md](Index.md).

Both are worth carrying forward as a pattern: A1's own instruments needed testing before
their output could be trusted.

## Remaining risks

- **Provenance staleness.** Toolchain facts are captured at *configure* time. A commit made
  between configuring and running a scenario is misreported. Mitigated by always configuring
  from clean before an evidence run; a build-time regeneration is deferred as not worth the
  complexity at A1's scale.
- **Contraction policy rests on a compiler default.** `/fp:precise` not contracting is a
  measured fact about MSVC 19.51, not a contract. A toolset upgrade must re-run the negative
  control. The `DeterminismAcrossRuns` gate would catch a change only if golden values were
  also pinned, which A1 does not yet do.
- **No golden values are pinned.** `DeterminismSmoke` proves run-to-run stability, not
  stability against a recorded reference. Pinning goldens becomes worthwhile in A2/A3, where
  the numbers mean something physical; doing it now would pin an arbitrary workload.
- **Single-machine evidence.** All measurements come from one i7-12650H. ADR 0010 only
  promises same-machine bit-exactness, so this is in scope, but no cross-machine tolerance
  data exists yet.
- **`/WHOLEARCHIVE` on the harness.** Needed so the replacement `operator new`/`delete` are
  always linked in. It also forces every harness object into every consumer. Harmless at this
  size; revisit if the harness grows.

## Disposable code

`TimingScenario` is disposable and is expected to be replaced by A2's frame-conversion
scenarios. `Cpp23Conformance`, `HarnessSelfCheck`, `ToolchainReport`, and `DeterminismSmoke`
are expected to survive the increment. The `Harness` library is the durable deliverable and is
deliberately free of any simulation or domain concept, so promoting it later remains a real
option rather than a sunk cost.

## Smallest next action

Increment A2 is **not** authorized. A1's scope was A1 only; A2 begins after the user reviews
this evidence.

Two things are worth deciding before A2 starts, both already agreed in principle:

1. Reference-data acquisition — fetch the pinned `naif0012.tls` and `gm_de440.tpc` kernels
   and JPL Horizons state vectors once, and check them in as provenance-stamped fixtures with
   SHA-256. This is the first A2 task and needs network access.
2. Whether ADR 0010's contraction wording should be amended in place rather than only carrying
   an A1 "recorded values" section. The section is sufficient under the ADR's own rules; a
   full amendment is a judgement call for the user.
