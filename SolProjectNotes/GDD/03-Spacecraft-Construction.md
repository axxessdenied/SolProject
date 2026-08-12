# 03 — Spacecraft Construction

## Design goal

Building a spacecraft is the primary creative and analytical activity. The construction system should let players reason from mission requirements to a physical design, understand validation feedback, and reuse proven work without turning every craft into the same optimal stack.

## Part model

Individual parts are confirmed. Candidate part concerns include structure, attachment, dry mass, materials, resources, power, heat, control, crew capacity, reliability/condition, cost, technology, manufacturer, and data-defined modules.

The first playable uses a curated set of fixed functional parts covering command/control, engines, decoupling, power, communications, and science, combined with procedural tanks and structural pieces. Node attachment and limited surface attachment provide a controlled but expressive topology. Life support, detailed internals, manufacturing quality, and deeper damage remain later decisions.

Early craft are designed around roughly 150–300 parts. This is a prototype and UX/performance target, not permission for algorithms whose cost becomes unbounded beyond it.

## Assemblies and blueprints

Players can select part subgraphs as named reusable assemblies: engine clusters, upper stages, landers, service modules, satellite buses, or standardized docking packages. Assemblies can be versioned and updated deliberately rather than silently changing existing craft.

Blueprint sharing/import must include:

- stable part/content identifiers and required content packs/mods;
- assembly and blueprint versions;
- validation for missing/incompatible definitions;
- previewed mass, cost, resources, performance estimates, and warnings;
- no silent substitution of performance-critical parts.

## Construction workflow

The proposed workflow is mission-aware without forcing one solution:

1. Start empty, from an assembly, or from an earlier blueprint.
2. Place/attach parts with symmetry and alignment tools.
3. Define staging, actions, control authority, and resource routing.
4. Inspect mass, balance, thrust, delta-v estimates, power, communication, thermal/structural warnings, cost, and mission compatibility.
5. Save a blueprint and build a campaign instance.

Estimates should communicate assumptions. Hidden calculations undermine the engineering fantasy.

## Plumbing, fuel, and electrical networks

Resource routing is part of the engineering challenge rather than an automatic craft-wide pool:

- propellants and other fluids move through explicit feed/crossfeed topology, valves, tanks, consumers, and transfer paths;
- electrical sources, storage, buses, switches/control, and loads form an explicit power network;
- staging, damage, disconnection, depletion, and player configuration can change network reachability during flight;
- construction validation identifies unreachable consumers, incompatible resources, insufficient capacity, and likely bottlenecks.

The proposed first implementation edits logical connections over the craft attachment graph and visualizes flow clearly. Requiring freeform 3D placement of every pipe/wire, detailed pressure-wave simulation, and electrical circuit-level behavior is deferred unless prototypes show it adds enough value.

## Design diversity

Parts and environments should make tradeoffs contextual: cheap versus reusable, high thrust versus efficiency, robust versus light, specialized versus modular, automated versus crewed, local manufacturing versus imported supply.

## Decisions

| Decision | Status | Why |
|---|---|---|
| Individual parts | Confirmed | User response |
| Reusable and shareable modular assemblies | Confirmed | User response |
| Blueprints separate from campaign instances/saves | Proposed | Enables sharing and stable persistence boundaries |
| Fixed functional parts plus procedural tanks/structures | Confirmed | User accepted recommendation |
| Node plus limited surface attachment | Confirmed direction | User accepted recommendation |
| Roughly 150–300 parts per early craft | Confirmed target | User accepted recommendation; requires validation |
| Explicit fluid/fuel and electrical systems | Confirmed | User response |
| Logical-network editing before freeform 3D routing | Proposed | Preserves engineering depth within first-playable scope |
| Internal component placement depth | Open | Determines construction and interior complexity |
