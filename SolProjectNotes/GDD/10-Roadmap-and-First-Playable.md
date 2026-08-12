# 10 — Roadmap and First Playable

## Roadmap shape

The project grows in concentric playable layers. A later layer cannot pull unfinished scope into an earlier milestone simply because its data model may eventually need it.

```text
Planning
  → risk prototypes
  → construct + fly
  → science + research + contracts
  → orbital company + people + infrastructure
  → mining + logistics + manufacturing + markets
  → colonies + politics
  → piracy + combat + fleets
  → far-future Solar System
```

Detailed technical sequencing is authoritative in `../Engine-Plan.md`.

## First-playable promise

“At 2026-01-01 00:00:00 UTC, start a small private spaceflight company at a fictional commercial launch facility near 28 degrees north on Florida's Atlantic coast. Accept the Orbital Environmental Survey contract, build an uncrewed chemical rocket from individual parts and reusable assemblies, pilot it seamlessly from the real-scale surface into an approximately 200 km by 200 km orbit, remain stable for one revolution, collect radiation, magnetic-field, and upper-atmosphere observations, transmit the data, and use the rewards to unlock a genuinely different design option.”

## Acceptance criteria

The first playable is complete when:

- at least two materially different valid craft designs can complete the target mission;
- the mission uses an uncrewed vehicle and keyboard/mouse flight with the accepted initial guidance set;
- the remappable default flight/camera bindings work as specified in [04 — Flight and Space Simulation](04-Flight-and-Space-Simulation.md);
- construction feedback explains major invalid states and performance assumptions;
- surface-to-orbit flight has no loading transition and meets accepted precision/performance budgets;
- orbital coast/time warp does not introduce unacceptable discontinuity;
- the Orbital Environmental Survey requires the correct radiation, magnetic-field, and upper-atmosphere instruments, an approximately 200 km by 200 km stable orbit, one complete revolution of observation, and valid transmission;
- the target mission completes through orbital data transmission and does not require reentry or physical recovery;
- funds/contracts make design tradeoffs meaningful without a dead-end economy;
- a campaign saves, reloads, and preserves craft, mission, company, science, and time state;
- a new player can finish the loop with onboarding and selected normal assists;
- headless scenarios cover the load-bearing flight/resource/progression rules;
- packaging and controls work on the accepted Windows reference hardware;
- the representative scene sustains 60 FPS at 1080p low/medium on the discrete baseline; integrated-graphics results are reported against their separately accepted target.

Technical-risk thresholds are accepted in the [P1a](../Milestones/P1a-Precision-and-Orbit.md) and [P1b](../Milestones/P1b-Renderer-and-Craft.md) milestone plans. The first-playable orbit acceptance band, final launch-site terrain placement, detailed science-data quality values, and production performance budgets remain open until their owning P2 milestones are planned.

Orbital mechanics use patched conics with spheres of influence (ADR 0011). Orbits do not decay, and Lagrange points do not exist. The first playable is unaffected; a player familiar with real spaceflight is most likely to notice this from P3 onward.

## Excluded from first playable

- multiple simulated corporations and open markets;
- mining and manufacturing;
- stations, shipyards, depots, or colonies;
- detailed people/workforce simulation;
- strategic fleets, politics, piracy, or combat;
- historical start years, fusion, antimatter, and speculative technologies;
- a general-purpose editor;
- cockpit/IVA and walkable interiors;
- production mod distribution and the public-alpha compatibility obligation unless this build is explicitly designated the first public alpha.

## Scope gates

- Do not start strategic/company expansion until the first-playable loop has been playtested.
- Do not start colonies before orbital logistics, habitats, people, and the economy are independently playable.
- Do not start warfare before detection, traffic, valuable infrastructure, factions, and peaceful logistics create a meaningful context.
- Do not extend to historical starts or speculative technology until the modern progression spine is stable.

## Decisions

| Decision | Status | Why |
|---|---|---|
| Concentric playable layers | Confirmed | User accepted the recommended roadmap direction |
| First playable promise above | Confirmed | User selected stable orbit and transmission rather than recovery |
| Strategy, industry, colonies, and combat excluded initially | Confirmed | Accepted first-playable boundary |
| Cockpit/IVA later and walking deferred | Confirmed | User response |
| Keyboard/mouse and the named initial guidance set first | Confirmed | User accepted recommendation |
| Initial mission is uncrewed | Confirmed | User response |
| Fixed 2026-01-01 00:00:00 UTC epoch and fictional launch region near 28 degrees north on Florida's Atlantic coast | Confirmed direction | User accepted recommendation; exact terrain placement remains open |
| Small owned launch/assembly/control/test footprint; lease major manufacturing/tracking | Confirmed initial scope | User accepted recommendation |
| Remappable default flight/camera bindings | Confirmed | User accepted recommendation |
| Orbital Environmental Survey objective | Confirmed | User accepted the approximate orbit, one-revolution observation, instrument set, and data transmission |
| P1 technical-risk thresholds | Confirmed | User accepted recommendation; P1 milestone plan is authoritative |
| First-playable orbit tolerance, final terrain placement, and content/reward values | Open | Belong to P2 milestone planning and prototype evidence |
