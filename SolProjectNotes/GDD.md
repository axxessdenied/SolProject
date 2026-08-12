# Game Design Document

**Game title:** Frontiers of Sol

**Engine:** SolEngine

**Genre:** 3D spacecraft construction and flight simulation growing into a Solar System economic/strategy sandbox

**Mode/platform:** Single-player, Windows x64, 1080p and higher

**Status:** Living pre-production design

This is the executive summary and index. Focused design notes live under `GDD/`. Current development state belongs only in `docs/project_status.md`.

## High concept

Begin as a small private spaceflight company in a present-day technological era. Design individual-part spacecraft, pilot them seamlessly across a real-scale Solar System from a planetary surface into space, perform scientific exploration, and turn discoveries into new technology and business opportunity. Astronomical names/data are real; companies and politics are fictional.

Over a long sandbox campaign, the company grows from contracts and launches into orbital infrastructure, mining, logistics, manufacturing, habitats, shipyards, and off-world colonies. Other corporations and Earth nations compete, cooperate, regulate, and eventually struggle to keep pace as space-based corporations become major powers.

## Product priority

1. Designing spacecraft.
2. Flying spacecraft.
3. Scientific exploration.
4. Advancing technology.
5. Running an economy.
6. Diplomacy and politics.
7. Controlling fleets.
8. Managing colonies.

This order resolves scope conflicts. Later systems should deepen or reward the earlier ones rather than replacing them with menus and automation.

## Design pillars

1. **Every craft is an engineering argument.** Parts, assemblies, resources, mass, environment, mission, and technology create meaningful tradeoffs.
2. **Flight makes the design real.** The player directly experiences the consequences of construction and planning from surface to orbit and beyond.
3. **Knowledge unlocks reach.** Exploration and valid scientific data are required inputs to research, not passive map collectibles.
4. **Infrastructure compounds capability.** Depots, mines, shipyards, habitats, people, and logistics turn difficult expeditions into routine operations.
5. **The Solar System changes around success.** Companies, nations, colonies, markets, and later security threats respond to the player's expansion.
6. **Realism is legible and adjustable.** Genuine constraints create decisions; assists and simplified modes reduce execution burden without making outcomes arbitrary.

## Core loop

```text
Choose opportunity → research/plan → design and validate craft → build/launch
        ↑                                                    ↓
Reinvest in tech and infrastructure ← return data/value ← fly and explore
```

Early play emphasizes direct construction and flight. Strategic command and mission planning grow as the company owns multiple vehicles and cannot personally fly everything. Automation should feel earned through technology, staff, procedures, and infrastructure.

## First playable

The proposed first playable is intentionally narrow:

- one private company at the 2026-01-01 00:00:00 UTC epoch, operating from a fictional commercial launch facility near 28 degrees north on Florida's Atlantic coast;
- one real-scale primary body, one bounded high-detail launch region, and enough orbital space for a meaningful mission;
- a curated chemical-rocket part set, procedural tanks/structures, reusable assemblies, and explicit logical fuel/fluid and electrical networks;
- direct construction, keyboard/mouse external third-person launch/ascent, instrumentation, stability/rate/throttle assists, maneuver/staging guidance, orbital map, stable orbit, and orbital science transmission;
- remappable flight defaults: W/S pitch, A/D yaw, Q/E roll, Shift/Ctrl throttle, Z/X full/cut throttle, Space stage, T stability assist, M orbital map, right-mouse drag camera orbit, and wheel zoom;
- an uncrewed **Orbital Environmental Survey** requiring an approximately 200 km by 200 km stable orbit for one revolution, radiation/magnetic-field/upper-atmosphere observations, and valid data transmission;
- contracts, funds, and a small research tree;
- save/resume and adjustable assists;
- no colonies, open market, politics, fleet combat, or general editor.
- no required reentry/recovery, cockpit/IVA, or walkable interiors.

Success means the loop is enjoyable and supports multiple valid designs—not that every long-term system has a placeholder screen.

## Campaign structure

The primary mode is an open-ended sandbox. Optional finite objectives or victory conditions may be added, with continued play after completion. Campaign length is player-directed and may be very long.

Adjustable starting years are a long-term goal. Development begins with a modern era, then may expand backward to early rocketry and forward through fusion, antimatter, and eventually more speculative technology.

## Scale targets

Initial design targets are up to 32 people on ships, 128 on stations, and 256 in colonies. These describe intended game entities, not a promise that all people receive full real-time individual simulation. Population behavior and level of detail remain to be designed.

## Inspiration map

- **Kerbal Space Program:** vehicle construction, orbital mechanics, flight, experimentation, and the consequences of design.
- **X4: Foundations:** a persistent economy, logistics, fleets, stations, and progression from direct action into command.
- **Civilization:** technological eras, focused progression, changing capabilities, and readable long-term goals.
- **Stellaris:** organizations/factions, events, research choices, politics, and grand-strategy context.

These are influence categories, not feature checklists. SolProject remains spacecraft-first and constrained to the Solar System for the planned game.

## Index

| Note | Scope | Status |
|---|---|---|
| [01 — Vision and Player Experience](GDD/01-Vision-and-Player-Experience.md) | Fantasy, pillars, priorities, boundaries | Initial draft |
| [02 — Core Loop and Progression](GDD/02-Core-Loop-and-Progression.md) | Company start, loop, eras, victory | Initial draft |
| [03 — Spacecraft Construction](GDD/03-Spacecraft-Construction.md) | Parts, assemblies, blueprints, validation | Initial draft |
| [04 — Flight and Space Simulation](GDD/04-Flight-and-Space-Simulation.md) | Direct control, seamless scale, realism, warp | Initial draft |
| [05 — Science and Technology](GDD/05-Science-and-Technology.md) | Exploration, data, research | Initial draft |
| [06 — Company, Economy, and Logistics](GDD/06-Company-Economy-and-Logistics.md) | Contracts through system economy | Initial draft |
| [07 — People and Habitats](GDD/07-People-and-Habitats.md) | Individuals, ships, stations, colonies | Initial draft |
| [08 — Factions and Conflict](GDD/08-Factions-and-Conflict.md) | Corporations, nations, colonies, piracy/combat | Initial draft |
| [09 — UX, Modding, and Persistence](GDD/09-UX-Modding-and-Persistence.md) | Controls, accessibility, content, saves | Initial draft |
| [10 — Roadmap and First Playable](GDD/10-Roadmap-and-First-Playable.md) | Release layers and scope gates | Initial draft |

## Decisions

| Decision | Status | Why |
|---|---|---|
| Spacecraft construction and flight outrank strategy systems | Confirmed | User priority order |
| Start as a small private spaceflight company | Confirmed | Creates a personal, comprehensible progression base |
| Sandbox first; optional endings with continued play | Confirmed | Supports long self-directed campaigns |
| Single-player only | Confirmed | Allows time acceleration and simulation architecture to remain tractable |
| Modern start first; wider historical/future starts later | Confirmed | Reduces initial content and simulation range |
| Solar System remains the intended geographical scope | Confirmed | User concept and progression endpoint |
| Use real Solar System scale/data with fictional companies/politics | Confirmed | User accepted recommendation |
| First playable is an orbit-and-transmit science-contract-to-research loop | Confirmed | User accepted the boundary |
| Frontiers of Sol is the game title | Confirmed | User selected it |
| Initial epoch 2026-01-01 00:00:00 UTC | Confirmed | User accepted recommendation |
| Initial mission is uncrewed | Confirmed | User response |
| Initial facility is near 28 degrees north on Florida's Atlantic coast and distinct from real launch complexes | Confirmed direction | User accepted recommendation; exact longitude and terrain placement remain open |
| Company owns a small hangar, mission control, one pad, and limited testing while leasing major services | Confirmed initial scope | User accepted recommendation |
| Initial default flight/camera bindings | Confirmed | User accepted recommendation; controls remain remappable |
| Orbital Environmental Survey is the first contract | Confirmed | User accepted the approximate orbit, one-revolution dwell, instrument set, and transmission objective |
