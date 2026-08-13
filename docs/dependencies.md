# Dependencies

The register ADR 0007 requires. Every third-party package and pinned toolchain input is
recorded here with the need that justified it, the alternatives weighed against it, its
license, and the boundary that keeps it replaceable.

**Policy:** [ADR 0007 — Dependency acquisition and pinning](decisions/0007-dependency-acquisition-and-pinning.md).
**Owning milestone for everything below:** P1b increment B1, authorized 2026-08-13.

Before B1, the project had no dependencies at all: P1a was deliberately dependency-free and no
`vcpkg.json` existed. B1 is the first increment to trigger this workflow.

## The split between vcpkg and the SDK

Two acquisition paths are in use, and the line between them is deliberate.

| Path | Supplies | Pinned by |
|---|---|---|
| vcpkg manifest mode | Everything compiled or linked: headers, loader shim, allocator, window library | `vcpkg.json` `builtin-baseline` |
| Vulkan SDK | Everything used to develop and to produce evidence: `glslc`, validation layer, profiles layer, `vkconfig`, capture tooling | `cmake/SolVulkanSdk.cmake` version check |

The SDK contributes **no headers and no link-time inputs**. That is what keeps a compiled
artifact a function of the checked-in manifest rather than of whichever installer a developer
happened to run. It is still a hard build prerequisite, because P1b's validation-output gate
is measured with the SDK's validation layer and its capability-rejection mitigation depends on
the SDK's profiles layer.

## Pinned toolchain inputs

| Input | Version in use | Why it is not a vcpkg package |
|---|---|---|
| Vulkan SDK (LunarG) | **1.4.357.0** | Layers and developer tooling, not a library. ADR 0007 names this case explicitly: "platform SDKs and development tools that are not vcpkg packages" are pinned toolchain inputs with documented installation and version checks. |
| vcpkg tool | `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8` | It is the package manager. Recorded separately from the registry baseline, as ADR 0007 requires, so the two are not confused. |

`cmake/SolVulkanSdk.cmake` fails configuration with an actionable message when no SDK 1.4.357.0
or newer is found, and warns — without failing — when the version differs from the reviewed
one, because a threshold measured against a different validation layer is not interchangeable
with one measured against the reviewed layer.

Install path: `winget install --id LunarG.VulkanSDK --exact`, or the installer from
`https://vulkan.lunarg.com/sdk/home`.

## vcpkg registry baseline

```
builtin-baseline: 2273a28f34ce5aac8be50b3b6b44da7fc1722e06   (2026-08-13)
triplet:          x64-windows-static-md
```

`x64-windows-static-md` links third-party code statically against the dynamic CRT. Static
libraries mean no third-party DLLs travel with the game, which matches ADR 0005's static-library
model; the dynamic CRT keeps the project on the same runtime as the OS-supplied Vulkan loader
and avoids allocator mismatches across module boundaries.

Updating the baseline is a reviewed dependency change, not maintenance.

## Packages

| Package | Constraint | Resolved | License | Features | Linked to | Boundary |
|---|---|---|---|---|---|---|
| `vulkan-headers` | `version>=1.4.357.0` | 1.4.357.0 | Apache-2.0 OR MIT | defaults only | `SolRender` (private) | Vulkan types never leave `sol::render` |
| `volk` | `version>=1.4.357.0` | 1.4.357.0 | MIT | defaults only | `SolRender` (private) | ditto |
| `vulkan-memory-allocator` | baseline | 3.4.0 | MIT | defaults only | **declared, not linked** | ditto, when linked |
| `glfw3` | baseline | 3.5.1 | Zlib | defaults only | **declared, not linked** | window/surface behind a `sol::platform` interface |

No package enables a non-default vcpkg port feature. `version>=` is a floor rather than a pin;
the checked-in baseline supplies the resolved versions above, and `vulkan-headers` and `volk`
additionally carry floors because volk is code-generated against a specific `vk.xml` revision
and the two must move together.

Everything that **is** linked is linked **PRIVATE**. That is the enforcement mechanism for ADR
0002's rule that Vulkan types stay behind renderer-owned interfaces: a public link would
propagate Vulkan's include directories to every consumer and the rule would hold only by
everyone's continued good intentions. Privately linked, a violation becomes a compile error in
the offending target.

### vulkan-headers

**Need.** The API definition. Pinned at 1.4.357.0 to match the installed SDK's validation
layer, so the headers the renderer compiles against and the layer that validates it come from
the same Vulkan header revision.

**Alternative rejected:** taking headers from the SDK. That would make every compiled artifact
depend on a developer's local installer rather than on the checked-in manifest, which is the
reproducibility problem ADR 0007 exists to prevent.

### volk

**Need.** A meta-loader that resolves Vulkan entry points at runtime, so nothing links
`vulkan-1.lib`.

This is not a convenience. ADR 0002 requires that "the shipped game must detect the installed
Vulkan loader/driver and fail clearly when the required device capability is unavailable." A
binary that imports `vulkan-1.lib` fails at process start with an OS-level "DLL not found"
dialog on a machine with no Vulkan driver — before any of the project's code runs, and with no
opportunity to say anything useful. With volk, the absent loader is a `VkResult` the renderer
inspects and reports. `VulkanInstance::create` returns exactly that diagnostic today.

**Alternative weighed:** linking the loader import library `vulkan-1.lib` directly, from either
the SDK or the `vulkan-loader` port. Simpler, one fewer package, and no implementation
translation unit to compile. Rejected for the failure behaviour above: the import-library form
makes an absent driver an OS-level process-start failure that no project code can intercept,
which is the specific outcome ADR 0002 requires the shipped game to avoid. The cost of volk is
one file and a macro; the cost of the alternative is a support burden on every machine without
a Vulkan driver.

**Boundary.** `VK_NO_PROTOTYPES` is set on `SolRender` only.

**Note on the vcpkg target.** The port's prebuilt `volk::volk` static library is compiled
without platform-specific defines and therefore omits the Win32 entry points. The renderer
needs `vkGetPhysicalDeviceWin32PresentationSupportKHR` to answer whether a queue family can
present *without creating a window*, which is what lets the capability report run headless as a
first-line diagnostic. `volk::volk_headers` is used instead and the implementation body is
compiled in `engine/render/src/VolkImplementation.cpp` with `VK_USE_PLATFORM_WIN32_KHR` set —
the port's own documented alternative. Warnings are relaxed for that one file in CMake, so the
exception is visible in the build description rather than buried in a source file.

**Replacement cost.** Low. Removing volk means linking the loader import library and deleting
one translation unit, at the cost of the clean-failure behaviour above.

### vulkan-memory-allocator

**Status: declared but not yet linked.** B1 creates no `VkDevice` yet and therefore allocates
nothing. An earlier revision of this entry justified VMA in the present tense — "B1 creates
depth images, vertex and index buffers, and staging buffers" — describing work that is not in
the branch, and the package was linked to `SolRender` while no translation unit included it.
Both are corrected: the entry below is explicitly forward-looking, and the link edge is added
by the increment that allocates.

**Anticipated need.** Vulkan requires the application to suballocate device memory itself; the
driver exposes a small number of large allocations and a hard cap on allocation count. The
render path will create depth images, vertex and index buffers, and staging buffers, and P1b
makes "allocation counts and upload volume" mandatory measurements — VMA's budget and
statistics API reports both directly.

**Alternative weighed:** a hand-written suballocator. Realistic in the long run and roughly
two to three weeks to get correct, against a 6-week increment box whose purpose is precision
and capability evidence, not allocator research. Rejected on that basis rather than on
capability.

**ADR 0002 caveat.** That ADR says not to select an allocator "until their owning prototype
increment demonstrates the need". The need above is *anticipated*, not yet demonstrated, which
is exactly why the package is declared and reviewed but not linked. The demonstration is the
first `VkDevice`; the link edge follows it. This will be a B1 selection rather than a
production commitment even then — M2 may revisit it with real asset volumes.

**Replacement cost.** Moderate, and bounded by the private link. VMA handles do not appear in
any `sol::render` public header.

### glfw3

**Need.** Window creation, Win32 surface creation, and keyboard/mouse input for the camera
paths that the depth and LOD gates traverse.

**Status: declared but not yet linked.** The manifest resolves it so the dependency review
lands with the rest of B1's stack, but no target consumes it until the swapchain work begins.

**Alternative weighed:** SDL3 (3.4.14, Zlib). Larger, and covers audio and gamepad input that
the project will eventually want. Rejected for now because those are separate milestone-owned
decisions that should not be pre-empted by a window-library choice, and because GLFW's smaller
surface is easier to keep behind a narrow `sol::platform` boundary. If a later audio milestone
selects SDL, replacing GLFW is a contained change behind that boundary.

**Alternative rejected:** raw Win32. Saves a dependency and costs a week of well-understood
boilerplate that GLFW has already debugged across driver and DPI edge cases.

## Verification

ADR 0007 requires that the first dependency change configure and build from a clean tree using
only the documented toolchain path and checked-in manifests, and that Debug and Release resolve
the same dependency graph. Performed 2026-08-13 on commit-in-progress, branch
`feature/p1b-vulkan-renderer`:

```
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake --preset windows-msvc-debug            # clean tree; vcpkg installed 4 packages in 16 s
cmake --build --preset windows-msvc-debug
cmake --preset windows-msvc-release          # clean tree
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-debug            # 22/22 passed
ctest --preset windows-msvc-release          # 22/22 passed
```

Both presets resolved the same four packages at the same versions from the same baseline. No
configuration-specific feature was needed. The 20 P1a tests are unaffected.

Warnings observed: `cl : Command line warning D9025 : overriding '/W4' with '/W0'`, twice,
from the deliberate warning relaxation on `VolkImplementation.cpp`. No other warnings.

## Decisions

| Decision | Status | Why | Date / source |
|---|---|---|---|
| vcpkg for compiled/linked inputs; Vulkan SDK for tooling only | Confirmed | Keeps a compiled artifact a function of the checked-in manifest rather than of a developer's installer, while P1b's gates still need the SDK's layers | Writer, 2026-08-13, under ADR 0007 |
| `x64-windows-static-md` triplet | Confirmed | No third-party DLLs travel with the game; dynamic CRT matches the OS-supplied loader and avoids cross-module allocator mismatch | Writer, 2026-08-13 |
| Vulkan SDK 1.4.357.0 as the reviewed version | Confirmed | The version the user installed; matched to `vulkan-headers` so headers and validation layer share a revision | User installed 2026-08-13 |
| `volk` over linking `vulkan-1.lib` | Confirmed | Absent-loader failure becomes a diagnostic the renderer writes, which ADR 0002 requires | Writer, 2026-08-13 |
| `glfw3` over SDL3 | Confirmed | Smaller surface, easier to keep behind a narrow boundary; audio/gamepad are separate milestone-owned decisions that a window-library choice should not pre-empt | Writer, 2026-08-13 |
| VMA as the allocator | **Proposed** | Reviewed and declared, but the need is anticipated rather than demonstrated; not linked until the first `VkDevice` | Writer, 2026-08-13 |
| Licence notices file | **Open** | Nothing is distributed yet; must be settled before any build leaves the machine | This document |

These are writer selections made under an accepted policy (ADR 0007), not user rulings, except
where the table says otherwise. Any of them can be reversed on request.

## Open items

- **Licence notices are not yet assembled.** All four packages are permissive (Apache-2.0 OR
  MIT, MIT, MIT, Zlib) and all four require attribution in distributed binaries. No notices
  file exists because nothing is distributed yet. This must be settled before any build leaves
  the machine; it is recorded here rather than deferred silently.
- **`glfw3` is declared but unlinked**, as described above. If the swapchain work selects a
  different path, the declaration is removed rather than left resolving unused.
