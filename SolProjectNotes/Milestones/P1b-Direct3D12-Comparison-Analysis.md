# P1b — Direct3D 12 comparison analysis

**Purpose:** the documented Direct3D 12 comparison [ADR 0002](../../docs/decisions/0002-vulkan-graphics-api.md) requires as gating evidence, covering driver coverage on the baseline GPU classes, tooling maturity, shader toolchain, and the cost of a future backend swap behind the renderer interface.

**Date:** 2026-08-14 · **Status:** Complete, and its limits are stated rather than implied.

**This is an analysis, not a spike.** ADR 0002 withdrew the requirement for a minimal Direct3D 12 implementation on 2026-08-12: a second backend costs weeks and cannot realistically change the decision for a Windows-only single-player title, where the two APIs differ in tooling and driver ergonomics rather than achievable performance. Nothing below is measured on Direct3D 12 by this project, and nothing below should be read as if it were.

## What this analysis can and cannot settle

**It cannot settle driver behaviour on hardware this project does not own.** No GTX 1060, RX 580, UHD 630 or Vega 8 is available, and **no AMD device or driver stack of any kind**. Every statement about those classes rests on vendor documentation and published support matrices, not on observation here. This is the same constraint recorded in the [reference-hardware evidence plan](P1b-Reference-Hardware-Evidence-Plan.md) and in ADR 0002's own hardware caveat, and it bounds this document exactly as it bounds the gate results.

**What it can settle** is the structural question the ADR actually asks: whether choosing Vulkan now forecloses anything, and what reversing it would cost. That is answerable from this project's own code, and the answer is measured rather than asserted — see the swap-cost section.

## 1. Driver coverage on the baseline GPU classes

| Class | Direct3D 12 | Vulkan | Note |
|---|---|---|---|
| GTX 1060 6 GB (Pascal) | Feature Level 12_1 | 1.3+ on current drivers | NVIDIA ships both from one driver package; Pascal remains on the mainline branch |
| RX 580 8 GB (GCN 4) | Feature Level 12_0 | 1.3 on current drivers | **Moved to AMD's legacy/maintenance driver track.** The risk sits on the driver's support lifetime, not on which API is selected — it applies to both |
| UHD 630 (Gen9.5) | Feature Level 12_1 | 1.3 on current drivers | Intel's Windows DCH driver ships both |
| Vega 8 (Raven Ridge) | Feature Level 12_0 | 1.3 on current drivers | Same AMD legacy-track caveat as the RX 580 |

**Both APIs are supported on all four classes**, and the one real coverage risk — AMD's legacy driver track for GCN 4 and Raven Ridge — is API-neutral. It would not be escaped by choosing Direct3D 12.

The asymmetry that does exist runs the other way: Vulkan requires a loader *and* an ICD, so a machine with a Windows-provided display driver but no vendor package can present a working Direct3D 12 stack and no Vulkan device at all. That is a real failure mode. It is also **the specific case B1 already built for** — nothing links the Vulkan import library, so a missing loader is a condition the renderer inspects and reports rather than an OS-level error before the game starts. That path is implemented but **unexercised**, since every available machine has a loader; it is recorded as such in [B1's evidence index](../../evidence/p1b/B1/Index.md) and is not claimed as verified here.

Vulkan 1.2 as the candidate floor is comfortably below what all four classes report, so the floor is not what would exclude a baseline device. Capability queries decide that, per device, which is what the capability requirement set exists to do.

## 2. Tooling maturity

**Direct3D 12 has the stronger vendor-integrated story on Windows** — PIX is excellent, free, and first-party, with GPU captures, timing, and shader debugging in one tool. That is a genuine advantage and should not be talked down.

**Vulkan's tooling is sufficient, and this project has now exercised it end to end.** Two things matter more than the headline comparison:

- **Validation layers are the load-bearing tool for this renderer, and they are Vulkan's strongest.** `VK_LAYER_KHRONOS_validation` catches synchronisation, lifetime and usage errors at the API boundary; B1 captures its output programmatically rather than sending it to a debugger console nobody reads. The Direct3D 12 debug layer plus GPU-based validation is comparable in kind but not obviously better.
- **A capture workflow exists and is proven**, using GFXReconstruct from the already-pinned Vulkan SDK rather than a separate install. Verified 2026-08-14 on `SolRenderLoop`: 240 frames captured, `gfxrecon-info` reporting application, device and resolution, and `gfxrecon-replay` replaying all 240 frames clean. Raw output in `evidence/p1b/B1/raw/release-CaptureWorkflow.txt`. RenderDoc remains the better interactive frame debugger and is not installed here; the ADR clause asks for a usable capture workflow, and GFXReconstruct satisfies it without adding a dependency.

**Net:** tooling favours Direct3D 12 somewhat, and not by enough to outweigh the API preference, given that this project's dominant debugging need is API-correctness at the boundary rather than shader-level stepping.

## 3. Shader toolchain

Both compile HLSL. Direct3D 12 uses DXC to DXIL; Vulkan uses DXC or glslc to SPIR-V. **The toolchains are equivalent in capability** and DXC targets both, so shader source is not locked to either API.

B1 compiles GLSL to SPIR-V with `glslc` at build time against the Vulkan 1.2 floor, embedding the result in the executable — so a shader reaching for a later feature fails the build rather than the one machine that lacks it. Moving to Direct3D 12 would mean either porting GLSL to HLSL or switching to DXC's SPIR-V/DXIL dual targeting. Both are mechanical for a shader set this size; neither is a reason to choose one API.

**SPIR-V is the better intermediate form for this project's purposes** — a stable, inspectable binary that `spirv-opt` and `spirv-val` operate on directly, which is what let B1 discover that Debug and Release produce byte-identical rendered output despite compiling different SPIR-V.

## 4. Cost of a future backend swap

**This is the clause that decides the ADR**, because it bounds the consequence of being wrong. The cost is measured from this project's own module boundary rather than estimated.

`sol::render` is the only module permitted to see Vulkan types, and the boundary is enforced by the build rather than by convention: every Vulkan-facing package is linked **privately**, so a Vulkan type escaping the module is a compile error, not a review comment. `sol::platform` and `sol::render` meet at plain OS handles — GLFW can create a Vulkan surface directly and doing so would have been shorter, but it would put a Vulkan type in the platform module's interface, so it was not done.

Measured on the tree as it stands:

| | |
|---|---|
| Files including any Vulkan header | **4**, all under `engine/render/src/` — `Renderer.cpp`, `VmaImplementation.cpp`, `VolkImplementation.cpp`, `VulkanInstanceImpl.h` |
| Vulkan types in **any** public header, including the renderer's own | **none.** The only occurrences of `Vk*` in `engine/render/include/` are three comments explaining what the interface deliberately does *not* mirror |
| Vulkan types in `sol::platform`'s interface | **none** — plain OS handles |
| Consumers of the renderer's public API | test/tool executables and the game target, none Vulkan-aware |

The confinement is therefore tighter than ADR 0002 requires. The ADR asks that Vulkan types be kept behind renderer-owned interfaces; in practice they do not reach the renderer's own public headers either, only its `src/`. `DeviceCapabilities` mirrors `VkPhysicalDeviceType` in shape while naming none of it, and states in a comment that it deliberately does not mirror `VkPhysicalDeviceLimits` — a limit earns a place there when something consults it.

So a Direct3D 12 backend would be **an additive implementation behind an unchanged interface**, not a refactor of the engine. What would have to be rewritten is the device/swapchain/pipeline/allocator layer inside `sol::render`; what would not is every caller, the terrain LOD scheme, the camera-relative frame arithmetic, the reversed-Z depth model, or the gate harnesses — those consume `Renderer`, `FrameStats` and `CameraState`, none of which name a Vulkan type.

Two costs are real and should be stated:

- **The allocator is Vulkan-specific.** VMA has no Direct3D 12 equivalent in this codebase; D3D12MA is the analogue and would be a separate dependency review. `FrameStats`'s device-memory fields are shaped by VMA's statistics model and would need re-deriving.
- **Reversed-Z with an infinite far plane is expressible in both**, so the depth model — the single most load-bearing rendering decision here — transfers unchanged. This is the main reason the swap cost is bounded.

**The renderer interface boundary is what keeps the option open at bounded cost, and it exists and is enforced today.** ADR 0002 anticipated exactly this and made it a boundary requirement applying from the first commit rather than at a later cleanup; that requirement has been met.

## 5. Conclusion

**Nothing in this analysis argues against Vulkan, and nothing argues that Direct3D 12 would have been a mistake either.** For a Windows-only single-player title the two are close enough that the decision rests on the user's stated API preference and on structural risk, not on a measurable rendering advantage.

- Driver coverage is equivalent on all four baseline classes, and the one genuine coverage risk is API-neutral.
- Tooling favours Direct3D 12 modestly; Vulkan's is sufficient and now demonstrated end to end.
- The shader toolchains are equivalent, and SPIR-V is the more inspectable intermediate.
- The swap cost is bounded by a module boundary that is enforced by the build and verified to hold.

**Recommendation: retain Vulkan.** A Vulkan driver or capability failure on a baseline device with no workaround remains a genuine trigger to reconsider — that is a measured result nobody has yet been able to obtain here, and it is the reason the reference-hardware evidence plan exists rather than a reason to have built a second backend preemptively.

## Decisions

| Decision | Status | Rationale / source |
|---|---|---|
| Direct3D 12 comparison delivered as a documented analysis, not a spike | Confirmed | ADR 0002 revision of 2026-08-12 withdrew the spike requirement; a second backend cannot realistically change a Windows-only decision |
| Retain Vulkan as the SolEngine graphics API | Proposed → feeds ADR 0002's disposition | Coverage equivalent, tooling adequate and demonstrated, swap cost bounded and measured at the module boundary |
| Capture workflow satisfied by GFXReconstruct rather than RenderDoc | Confirmed 2026-08-14 | Ships with the already-pinned Vulkan SDK, so it adds no dependency; verified by a 240-frame capture, inspect and replay round trip |
| No claim in this document rests on AMD hardware | Confirmed | No AMD device or driver stack is available; ADR 0002's hardware caveat governs and is not weakened by this analysis |
