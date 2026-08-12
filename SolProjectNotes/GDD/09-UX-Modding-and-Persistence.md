# 09 — UX, Modding, and Persistence

## Information design

The game spans hands-on construction, real-time flight, orbital planning, and strategy. Each workspace needs a clear job while sharing consistent object identity, time controls, alerts, and navigation.

Proposed primary workspaces are:

- construction/engineering;
- external third-person flight initially, with cockpit/IVA later;
- orbital map and mission planning;
- science/research;
- company/operations;
- later system economy and political views.

Seamless travel does not forbid useful map or management views. The player’s craft and universe continue to exist while presentation changes.

## Difficulty and accessibility

Difficulty should be assembled from coherent presets plus advanced options. Candidate axes include piloting assists, planning information, aerodynamic/thermal/structural fidelity, communication and life support, failures, economy pressure, and save/revert rules.

Baseline accessibility planning should include remappable controls, scalable UI for 1080p+, color-safe information, subtitle/caption support, motion/camera options, pause, and alternatives to time-critical fine motor input where consistent with mode.

The discrete baseline is an Intel Core i5-8400 or Ryzen 5 2600, GTX 1060 6 GB or RX 580 8 GB, 16 GB RAM, and SSD, targeting 60 FPS at 1080p on low/medium settings. Graphics options should scale terrain/atmosphere quality, shadows, effects, texture/mesh budgets, anti-aliasing, and other presentation features upward for newer hardware without altering authoritative simulation results.

Intel UHD 630 and AMD Vega 8-class integrated graphics are investigation targets for 30 FPS at 720p/low. P1 must record driver/API support, frame-time distribution, memory use, and visual compromises. “Investigation target” is not a support promise until both classes pass or their exclusions are documented.

## Modding

Modding is a confirmed architectural goal. Likely data-driven domains include parts, resources, materials, experiments, research, contracts, organizations, and tuning. Code/plugin mods, scripting, load order, dependency resolution, security, distribution, and multiplayer concerns are not yet decided.

Validation must explain missing identifiers, incompatible versions, cycles, invalid ranges, and schema errors before a campaign starts.

## Saves and sharing

Keep campaign saves, craft/assembly blueprints, settings, and mods/content packs separate. Saves should record schema version, required content and versions, persistent IDs, campaign time, random seeds where used, and enough diagnostics to explain incompatibility.

All persistent formats carry explicit versions from first use. Internal pre-alpha saves and blueprints may be invalidated intentionally with clear diagnostics. Beginning with the first public alpha, supported releases guarantee explicit cross-release migrations for saves and blueprints. Migration must preserve the original or a recoverable backup on failure.

Settings, data-defined content, and manifests use schema-validated UTF-8 JSON. Blueprints are versioned packages with readable JSON manifests; campaign saves are versioned chunked binary containers with small readable JSON manifests. Concrete archive/compression/binary libraries remain milestone choices. Version 1.0 must migrate every supported public-alpha save and blueprint. Beginning with 1.0, releases support their current major series plus the final supported save/blueprint schema of the immediately preceding major series. ADRs 0004 and 0009 own the technical contract.

## Editor policy

Do not build a general editor upfront. Begin with validated data, debug visualizations, inspectors, and in-game developer panels. Build focused authoring tools when repeated content work proves their value.

## Decisions

| Decision | Status | Why |
|---|---|---|
| Modding is an architectural goal | Confirmed | User response |
| Save compatibility is desired but scheduled later | Confirmed | User response |
| Version persistent formats from their first use | Confirmed | User response and ADR 0004 |
| Guarantee cross-release save/blueprint migrations from first public alpha | Confirmed | User response and ADR 0004 |
| Permit explicit pre-alpha invalidation | Confirmed | Preserves rapid iteration before the public promise |
| Delay general editor until needed | Confirmed | User response |
| 60 FPS at 1080p on the named discrete reference PC | Confirmed goal | User accepted recommended baseline |
| UHD 630/Vega 8-class investigation tier targets 30 FPS at 720p/low | Confirmed target | User accepted recommendation; support remains validation-dependent |
| Scalable higher-quality graphics options | Confirmed direction | User response |
| JSON settings/content and JSON-manifest blueprint packages | Confirmed | User accepted recommendation and ADR 0009 |
| JSON-manifest chunked binary campaign saves | Confirmed direction | User accepted artifact boundary; concrete encoding remains milestone-owned |
| Public-alpha-to-1.0 and one-major migration windows | Confirmed | User accepted recommendation and ADR 0009 |
| Concrete encoding/archive libraries and mod packaging | Open | Requires owning milestone and dependency review |
