# P1a increment A1 — evidence index

**Increment:** A1, measurement and build harness
**Owner:** Claude (single writer)
**Branch / base:** `feature/p1a-precision-and-orbit` from `dev`
**Date:** 2026-08-12
**Result:** All A1 done criteria met. Two findings against ADR assumptions, recorded below.

Raw measurement output lives in `raw/`, which `.gitignore` excludes. It is reproducible from
the commands in [Handoff.md](Handoff.md); this index and the handoff are the durable record.

## Toolchain and host

| Item | Recorded value |
|---|---|
| Compiler | MSVC 19.51.36252.0 (`_MSC_VER` 1951, `_MSC_FULL_VER` 195136252), toolset 14.51.36231 |
| Visual Studio | 18 Community, x64 host / x64 target |
| CMake | 4.4.2 |
| Ninja | 1.12.1 (resolved from `PATH`; the local copy ships with Meson, and is deliberately not pinned) |
| OS | Windows 10.0.26200 |
| CPU | 12th Gen Intel Core i7-12650H, 16 logical processors, AVX2 and FMA3 present and OS-enabled |
| Shell requirement | `INCLUDE`/`LIB` were already set in the environment; no `vcvars64.bat` step was needed. See the handoff for the caveat. |

## Preset names (project truth)

| Kind | Debug | Release | ADR 0010 negative control |
|---|---|---|---|
| configure | `windows-msvc-debug` | `windows-msvc-release` | `windows-msvc-release-negcontrol-contract` |
| build | `windows-msvc-debug` | `windows-msvc-release` | `windows-msvc-release-negcontrol-contract` |
| test | `windows-msvc-debug` | `windows-msvc-release` | `windows-msvc-release-negcontrol-contract` |

Binary directories are `build/<preset name>/`. Separate single-configuration Ninja trees per
ADR 0001.

## Recorded compiler flags

Project-owned targets receive, via `sol_apply_project_options()` in
`cmake/SolProjectOptions.cmake`:

```
-std:c++latest /fp:precise /arch:AVX2 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W4 /WX
```

Full Release command line as recorded in the reports:

```
-std:c++latest /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG /fp:precise /arch:AVX2
/permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W4 /WX
```

## Findings

### Finding 1 — MSVC 19.51 rejects `/std:c++23`

Only `/std:c++latest` is accepted. CMake's `cxx_std_23` correctly emits `-std:c++latest`, and
`__cplusplus` reports `202400` under `/Zc:__cplusplus`. No action needed beyond recording the
literal flag, which ADR 0001 anticipated. The project never hand-writes the flag.

### Finding 2 — no contraction-disabling flag exists; ADR 0010's prose does not match the toolset

ADR 0010 requires FMA contraction to be "disabled explicitly rather than relying on the
default." **This toolset offers no way to do that.** Configure-time probes of both plausible
spellings fail:

```
-- Performing Test SOL_HAVE_FLAG__fp_contract_   - Failed    (/fp:contract-)
-- Performing Test SOL_HAVE_FLAG__fp_no_contract - Failed    (/fp:no-contract)
```

Only the *enabling* `/fp:contract` exists. The requirement is therefore satisfied by
`/fp:precise`'s default rather than by an explicit flag, and the build records this as
`fpContractionMode = "implicit-off-under-fp-precise"` in every report.

Because that is a default rather than a guarantee, A1 **verifies** it empirically rather than
asserting it. See the negative control below. ADR 0010 states that values recorded during A1
are authoritative over its prose, so its "Recorded implementation values (A1)" section now
carries this.

### Finding 3 — MSVC implements `auto(x)` but defines no `__cpp_auto_cast`

The decay-copy use site compiles and behaves correctly; the feature-test macro is absent. The
conformance target guards the `static_assert` behind `#if defined(...)` and relies on the use
site. Recorded rather than worked around, and re-checkable when the toolset changes. This is
exactly the case ADR 0001 warns about.

All twelve other required C++23 facilities report present. See `raw/release-ToolchainReport.json`.

## Threshold results

| A1 done criterion | Result |
|---|---|
| Clean configure/build/test through checked-in presets, both configurations | **Pass.** 6/6 tests in Debug and 6/6 in Release from a removed `build/` tree. |
| Conformance target compiles each required C++23 facility | **Pass**, with Finding 3 recorded. |
| One headless timing scenario emits complete metadata and machine-readable measurements | **Pass.** `TimingScenario` emits full provenance, a distribution, peak memory, and allocation counts. |
| Debug and Release artifacts cannot overwrite one another | **Pass.** Separate trees; both `DeterminismSmoke.exe` binaries coexist (Debug 1,218,560 B, Release 212,992 B). |
| Determinism smoke reproduces bit-identical output across repeated runs | **Pass.** `DeterminismAcrossRuns` compares the `results` sections of two separate process launches. |
| Negative control changes the digest (ADR 0010 validation) | **Pass.** See below. |

### ADR 0010 negative control

Baseline and control differ **only** by `/fp:contract`.

| Kernel | `/fp:precise` | `+ /fp:contract` | Verdict |
|---|---|---|---|
| `accumulationChain` | `4023cc23ec8bab52` | `4023cc23ec8bab42` | changed |
| `contractionProbe` | `0000000000000000` | `bcf5554000000000` | changed |
| `transcendentalChain` | `3ff2057bc24c2021` | `3ff2057bc24c2021` | unchanged |
| `orderedReduction.forward` | `423750fed217e8ee` | `423750fed217e8ee` | unchanged |

`contractionProbe` is the decisive one. It computes `a*b - 1.0` for `a = 1+e`, `b = 1-e`,
`e = 2^-27`: without contraction the product rounds to exactly `1.0` and the result is
exactly `0.0`; with a fused multiply-add it is exactly `-e^2`. The measured values are
`0.0` and `-4.73687929158917e-15`.

**This establishes Finding 2's conclusion as measured fact: MSVC 19.51 under `/fp:precise`
does not contract, and the harness detects it if that ever changes.**

An earlier version of this kernel produced identical output in both builds — it was named for
a job it did not do. It was replaced rather than reinterpreted; the first design's failure is
recorded here because a probe that silently fails to probe is the most dangerous artifact A1
could have shipped.

## Measurements

`timing.transformAndReduce`, 16384 doubles, 16 warm-up iterations, 128 samples.

Values below are from the `raw/` files currently on disk.

| Metric | Release | Debug |
|---|---|---|
| iterationTime min | 7,400 ns | 32,100 ns |
| iterationTime median | 8,000 ns | 32,300 ns |
| iterationTime p95 | 8,700 ns | 33,100 ns |
| iterationTime p99 | 9,100 ns | 43,700 ns |
| iterationTime max | 26,300 ns | 78,500 ns |
| perElementTime median | 0.488 ns | 1.971 ns |
| Allocations in measured region | 0 | 0 |
| Peak working set | 4,583,424 B | 5,206,016 B |

Only the Release column is P1a performance evidence; the reports carry an
`evidenceEligibility` field stating so, so a Debug number cannot be quoted by accident.

**These timings do not reproduce exactly, and are not expected to.** An earlier run of the
same Release binary gave a median of 7,700 ns with a p99 of 24,500 ns and a max of 80,900 ns.
The median moves by a few percent and the upper tail by several times, because the tail is
scheduler noise on a loaded interactive machine rather than a property of a fixed-count loop.

This is the distinction A1 exists to make legible: **numerical** results are bit-exact and are
gated as such, while **timing** results are a distribution that must be reported with its
spread and re-measured rather than quoted as a constant. Any later increment quoting a single
timing number without its distribution is misreporting.

### Observation, not a guarantee

Debug and Release produced **identical** bit patterns for all four determinism kernels. ADR
0010 promises nothing across builds, and this must not be relied on or asserted in a test.
It is recorded only because the opposite would have warranted investigation.

## Deliverables

| Path | Role |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | Build graph and preset definitions |
| `cmake/SolProjectOptions.cmake` | The single place compiler policy is applied; probes flags, rejects `/fp:fast` |
| `cmake/SolToolchainFacts.cmake`, `.h.in` | Configure-time provenance baked into every binary |
| `cmake/RunDeterminismCheck.cmake` | Cross-run bit-exactness gate |
| `prototypes/p1a/Harness/` | **Durable.** Measurement, provenance, and reporting library (`sol::proto`) |
| `prototypes/p1a/Cpp23Conformance/` | ADR 0001 facility gate |
| `prototypes/p1a/HarnessSelfCheck/` | Guards the harness itself |
| `prototypes/p1a/ToolchainReport/` | Host and toolchain capability report |
| `prototypes/p1a/DeterminismSmoke/` | ADR 0010 determinism kernels |
| `prototypes/p1a/TimingScenario/` | Timing pipeline demonstration; **disposable**, replaced in A2 |
