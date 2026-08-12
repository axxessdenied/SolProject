# ADR 0002 — Prefer Vulkan for the renderer

**Status:** Proposed

**Date:** 2026-08-12

## Context

SolEngine needs explicit control over large-world rendering, terrain/atmosphere level of detail, GPU memory, synchronization, diagnostics, and scalable graphics settings. Windows x64 is the first platform, but Vulkan is the user's preferred API. The discrete baseline targets 60 FPS at 1080p on an Intel Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, and SSD. Intel UHD 630 and AMD Vega 8-class integrated graphics are investigation targets for 30 FPS at 720p/low; support remains conditional on P1 driver/capability/performance evidence.

## Accepted P1 target and proposed production decision

- Use Vulkan as the primary SolEngine graphics API if the P1 prototype passes its acceptance criteria.
- Keep Vulkan types behind renderer-owned interfaces; game and general engine-domain APIs must not expose them.
- Use Vulkan 1.2 as the candidate minimum API version for P1. Query instance and physical-device versions, features, extensions, formats, queues, limits, and memory capabilities rather than inferring support from the API version alone.
- Treat Vulkan 1.3 and later capabilities as optional during P1. A capability-based path may use them when present, but the prototype must not make them an undeclared baseline requirement.
- Do not select mandatory non-core extensions, shader language/compiler, allocator, rendering framework, or pinned SDK until their owning prototype increment demonstrates the need and completes dependency review.
- Build capability tiers: a conservative required baseline for older hardware and optional visual features for stronger GPUs. Ray tracing and mesh shaders must not be baseline requirements.
- Provide a distinct low graphics tier intended to reach 30 FPS at 720p/low on UHD 630 and Vega 8-class investigation hardware; do not promise support until measured.
- Use validation layers and capture/debug tooling in development; enumerate loader, device version, features, extensions, formats, queues, and memory capabilities at startup with actionable failure diagnostics.
- Treat the Vulkan SDK as a development toolchain input. The shipped game must detect the installed Vulkan loader/driver and fail clearly when the required device capability is unavailable.

## Alternatives considered

- **Direct3D 12:** excellent Windows integration and a credible fallback if Vulkan driver coverage, tooling, or performance fails on the selected older-PC baseline.
- **A third-party rendering abstraction:** may accelerate early development but risks constraining the unusual large-world renderer; evaluate only against concrete P1 requirements.
- **Supporting Vulkan and Direct3D 12 immediately:** rejected for initial scope because two production backends would double validation burden before one renderer proves the game.

## Required P1 evidence

- A p95 total frame time no greater than 16.67 ms at 1080p/low-medium on both discrete baseline GPU classes in the representative surface-to-orbit scene.
- A documented p95 result against 33.3 ms at 720p/low on UHD 630 and Vega 8-class systems, including driver, Vulkan capabilities, frame-time distribution, CPU/GPU memory use, visual compromises, and any unsupported status. Failure of an integrated investigation device does not block P1 if it is reported honestly and removed from the support target.
- Correct camera-relative rendering and depth behavior from launch surface to orbital altitude.
- No more than 0.25 pixels of stationary screen-space jitter in the defined surface and orbital precision views.
- Stable terrain/atmosphere LOD transitions without unbounded memory growth, with peak CPU/GPU memory, allocations, and uploads recorded for later production budgeting.
- Startup capability reporting and graceful rejection on unsupported drivers/devices.
- Clean validation-layer output for the exercised path and usable RenderDoc/capture workflow.
- A documented comparison with a minimal Direct3D 12 spike or evidence-based analysis before this ADR becomes Accepted.

## Consequences

- Vulkan 1.2 is the P1 candidate floor, not yet a production support promise. P1 evidence may retain it, lower it with explicit fallbacks, raise it with an accepted hardware-scope change, or reject Vulkan.
- Optional graphics tiers must never change authoritative simulation results.
- Renderer architecture must budget for pipeline/shader caching, asynchronous uploads, resource lifetime, and device-loss/error reporting from the beginning.
- P1 records peak CPU and GPU memory. Production content budgets are set after representative assets exist rather than invented for the prototype.

## Sources

- Khronos documents that Windows requires a Vulkan loader plus a compatible device driver and recommends checking actual device support: [Checking for Vulkan Support](https://docs.vulkan.org/guide/latest/checking_for_support.html).
- Khronos recommends the Vulkan SDK for headers, validation layers, and development tools: [Vulkan Development Environments](https://docs.vulkan.org/guide/latest/ide.html).
- Vulkan versions are backward compatible, but instance and device version support can differ; required features must be queried: [Vulkan Versions and Porting Guide](https://docs.vulkan.org/guide/latest/versions.html).
- Khronos describes per-device queries for properties, features, extensions, limits, and formats: [Querying Properties, Extensions, Features, Limits, and Formats](https://docs.vulkan.org/guide/latest/querying_extensions_features.html).
