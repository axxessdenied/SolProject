# SolProject

SolProject is the planning and development repository for **SolEngine** and
**Frontiers of Sol**, a single-player 3D spaceflight and Solar System expansion game.

The game begins with a small private spaceflight company. Players design and
fly spacecraft across a real-scale Solar System, conduct scientific exploration,
unlock technology, and grow into a system-spanning industrial and political power.

## Current status

Pre-production planning is complete, the planning gate is approved, and implementation has
started under milestone-scoped authorization. **P1a — precision and orbit prototypes — is
complete**, delivering headless measured evidence for reference-frame conversion, the
astronomical-scale precision budget, and hybrid orbital propagation under time warp. See
[project status](docs/project_status.md), the [engine plan](SolProjectNotes/Engine-Plan.md),
and the [game design document](SolProjectNotes/GDD.md). The stage that produced the current
code is the
[P1a precision and orbit plan](SolProjectNotes/Milestones/P1a-Precision-and-Orbit.md), with
evidence indexed at [`evidence/p1a/`](evidence/p1a/Index.md); the next stage is
[P1b renderer and craft](SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md).

**P1b increment B1 — the Vulkan renderer — is in progress**, and is the first work written in
the production tree: `engine/platform/` holds `sol::platform` and `engine/render/` holds
`sol::render`, the only module permitted to see Vulkan types. It enumerates and reports device
capabilities, rejects unsupported devices with actionable diagnostics, and presents a reference
scene through a reversed-Z depth buffer spanning half a metre to planetary distance. None of
P1b's gating thresholds has been measured yet.

Nothing in the repository is a game yet. P1a's executables are **disposable measurement
prototypes** under `prototypes/p1a/`, deliberately kept out of `engine/` and `game/`; they
exist to produce evidence, and promoting any of them requires an explicit review.

## Project boundaries

- Engine: SolEngine, purpose-built for this game while retaining clean module boundaries.
- Platform: Windows x64 first, targeting 60 FPS at 1080p on the baseline discrete-GPU PC and scalable higher-quality options. Selected integrated graphics are a 30 FPS/720p-low investigation tier, not yet a support promise. **Both targets are unverified on hardware:** no GTX 1060, RX 580, UHD 630, or Vega 8 — and no AMD device of any kind — is available to this project, so those claims rest on the [reference-hardware evidence plan](SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) rather than on measurement.
- Mode: single-player.
- Language/toolchain: C++23 in namespace `sol`, MSVC, CMake, and Ninja.
- Graphics: Vulkan 1.2 is the P1 candidate floor; production adoption remains pending prototype evidence. UHD 630/Vega 8-class graphics form a 30 FPS/720p-low investigation tier, unmeasured for the reason above.
- Orbital model: patched conics with spheres of influence. No perturbations, drag, or orbital decay in the propagation; aerodynamic forces still act on craft inside the atmosphere.
- Determinism: bit-exact on the same build and machine, tolerance-based across machines. `/arch:AVX2` sets a hard CPU floor at Haswell-era Intel and Zen-era AMD.
- Assets: authored in Blender, interchanged as glTF 2.0, generated procedurally where parametric, baked at build time. Binary sources will use Git LFS.
- Reference project: FactoryProject supplies workflow ideas only. SolProject does not copy its code or assumptions.

## Building

Requires Windows x64, MSVC 19.51 or later, CMake 3.28 or later, and Ninja, plus two
prerequisites introduced by P1b increment B1:

- **vcpkg**, which supplies every compiled and linked dependency from the pinned baseline in
  `vcpkg.json`. Clone it to `C:\vcpkg` (or set `VCPKG_ROOT`); no admin rights are needed.

  ```powershell
  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
  ```

- **The Vulkan SDK**, 1.4.357.0 or newer: `winget install --id LunarG.VulkanSDK --exact`. It
  supplies `glslc` and the validation and profiles layers. It contributes no headers and no
  link inputs, so a compiled artifact depends on the checked-in manifest rather than on which
  installer you ran — but the build still requires it, because P1b's gates are measured with
  those tools.

Both are reviewed in [dependencies](docs/dependencies.md). Configuration fails with an
actionable message if either is missing.

From a shell with the MSVC environment loaded (`Launch-VsDevShell.ps1 -Arch amd64` if
`INCLUDE` and `LIB` are not already set):

```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

Substitute `windows-msvc-debug` for a Debug tree; the two cannot overwrite one another. Only
the Release preset may produce performance evidence. A third preset,
`windows-msvc-release-negcontrol-contract`, deliberately fails as an ADR 0010 negative
control and must not be used for evidence.

Git LFS must be configured before the first binary asset is committed.
