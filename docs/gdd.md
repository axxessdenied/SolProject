# The Stars Don't Wait — Game Design Document

Title: **The Stars Don't Wait** — *Carve out a life in a galaxy already in motion.* (Internal names: **SolProject** / **Sol Engine**, `docs/engine-plan.md`.) Single-player 3D space sandbox for PC (Windows first).

Confidence markers used below — **[core]**: settled, build against it; **[likely]**: direction set, details open; **[open]**: deliberately undecided, see §10.

Sections **§1–§10 keep their numbers permanently**: `docs/engine-plan.md` and `docs/decisions/` cite them by number. New material is appended as §11 onward.

---

## 1. Vision

You are one pilot in a galaxy that runs with or without you. Start with a single underwhelming ship and a handful of credits; end up wealthy, feared, connected, or all three — through trading, fighting, mining, exploring, or any mix you invent. The galaxy is procedurally generated but governed by handcrafted factions and rules, and its economy and politics are genuinely simulated: prices move because NPC freighters actually hauled the goods, and a war front shifts because patrols actually lost.

### Pillars

1. **You are a pilot, not a cursor.** Everything is experienced from a ship you directly fly, in first or third person. The cockpit is home.
   - **Amended 2026-08-28 (the Depth Arc, §11–§14).** As your holdings grow, the map becomes a place you can *act* from as well as read from — issuing orders, working markets, planning routes. It never becomes a place you play *instead of* flying. Two rules hold the pillar: **every order is given by a captain who is himself sitting in a ship**, and **there is no view of the galaxy that is not somebody's instruments**. A player who wants to fly every metre is never made to open the map; a player who wants to run a business from a chair is never made to fly a delivery. Both are the same game.
2. **A universe that doesn't wait for you.** Factions, traders, and pirates pursue their own goals on a real simulation. The player is a participant, not the protagonist of a script.
3. **From nobody to somebody.** Long, earned progression: better ships, better gear, reputation, wealth, and eventually influence. No levels — power comes from what you own, know, and are owed.
4. **Yours to break.** Sandbox foundation: systems over scripts, all content data-driven and Lua-scripted, and the base game built like a mod so mods are first-class. v1 ships an authored **campaign spine** that rides on top of the real systems and paces headline unlocks — but it is ignorable after its opening, and a player who walks away keeps a complete game (✅ Q7, `docs/decisions/008-campaign-spine.md`).

### Influences map

| Game | What we take | What we leave |
|---|---|---|
| Elite Dangerous | Flight feel, cockpit immersion, sense of scale | Scale so vast it's empty; grind pacing |
| X4: Foundations | Living economy, NPC-driven universe, empire *seeds* | UI sprawl; station-walking |
| Starsector | Combat legibility, faction dynamics, outfitting depth | Top-down perspective |
| EVE Online | Market/economy depth, reputation webs, "ships are tools", hull-class vocabulary | Multiplayer, full-loot harshness |
| Stellaris / Civ | Faction personalities, strategic pacing of a galaxy over time | Playing *as* the empire (we fly inside one) |
| Avorion | Procedural galaxy structure, difficulty-by-region | Block-based ship building (not at launch) |

---

## 2. Player Fantasy & Core Loops

**Fantasy**: independent starship captain carving a life out of a frontier galaxy.

- **Minute-to-minute**: fly the ship — maneuver, manage speed/assists, fight, dodge, dock, scan. This loop must feel good in isolation by Phase 4 or nothing above it matters. **[core]**
- **Session (30–90 min)**: run a contract, work a trade route, clear a pirate nest, prospect an asteroid field, scout a new system, run cargo somebody would rather you didn't; return, sell, refit. **[core]**
- **Long-term (tens of hours)**: climb the ship ladder, build faction standing (and enemies), unlock better markets/equipment/regions, accumulate wealth toward big purchases; late-game influence — ships that fly for you, then captains, then fleets (empire *seeds* growing into a small empire, never colony management). **[likely]**

Progression is horizontal as much as vertical: a mining barge, a fast smuggler, and a heavy gunship are different games, not tiers. **[likely]**

---

## 3. World & Universe

- **Galaxy**: procedurally generated from a seed at new-game — 50–150 star systems (target; tune for density of interest, not raw size) connected by jump-gate lanes forming a graph with regions: civilized core, contested frontier, lawless fringe. Difficulty and opportunity rise toward the fringe (Avorion-style gradient). **[core]**
- **System security** **[core, added 2026-08-28]**: that gradient stops being a colour on a map and becomes **a number every system carries**. Security is **signed**. A **positive** rating means a major faction polices the place, and both the strength of its patrols and how fast anything answers a distress scale with it. **Zero** means nobody comes. A **negative** rating means somebody else's law holds — a pirate clan — and it answers intrusion exactly the way a navy does, except that the wing it dispatches is coming for *you*. The rating is a **static baseline** (who owns it, how far from their capital) minus **what is happening there right now** (raids, live front lines), so a quiet core system and the same system three hours into a war do not read the same. This is what makes the fringe pay: nobody polices it, which is the same sentence as nobody protects you. See §15 and `docs/decisions/019-system-security.md`.
- **Systems**: each system is a flat-ish playfield of real 3D space — star, planets (scenery + orbit anchors), stations, asteroid fields, gates, points of interest. Distances in the hundreds of thousands of km; in-system **cruise drive** (superlight throttle mode, interruptible) covers them. **[likely]**
- **Authored systems and constellations** **[core, added 2026-08-28]**: a system may be **written by hand in TOML** and injected into generation rather than rolled — a named station, a fixed layout, a scripted anchor. Each authored system declares a **placement rule** (in a named system; N jumps from a named system; a random system excluding secrets; a random position in the galaxy) and several may be grouped into a **constellation** placed as a unit with its internal topology intact. Authored systems are moddable content like everything else, which is what makes a total-overhaul mod able to replace the map as well as the rules. This is the narrative control the campaign needs and the home every secret has lacked. See `docs/decisions/018-authored-systems.md`.
- **Travel**: jump gates between systems as baseline; player-owned jump drive as a late-game unlock (decided, `decisions/004`). No time compression — the game runs at 1× real time, with cruise speed and system layout tuned so legs stay short (decided, `decisions/005`). **[core]**
- **Handcrafted over procedural**: faction identities, ship/weapon rosters, economy rules, story anchors (unique stations, derelicts, questlines) are authored; the generator *places* them according to rules. Procedural breadth, authored flavor. **[core]**
- **Persistence**: one continuous save-game world per campaign; full state (economy, faction relations, wrecks that matter) persists. Same seed + same content version ⇒ same starting galaxy. Saves live one folder per campaign and the game saves itself (Phase 27). **[core]**

---

## 4. Flight Model **[core]**

Newtonian 6-DoF under the hood; layered assists on top (Elite's proven compromise):

- **Assist on (default)**: velocity/rotation damping makes the ship fly "atmospherically" — approachable, still momentum-aware. Soft speed cap per ship for combat balance and AI fairness.
- **Assist off (toggle)**: raw Newtonian for drift turns and expert play. No cap on velocity except gameplay-pragmatic limits.
- Ships differ meaningfully in mass, thrust envelope (main vs. maneuvering), agility, and power budget — handling is a primary stat, not flavor text.
- Mouse+keyboard first-class; gamepad next; HOTAS eventually. **[likely]**
- **Flying is never the only way to fly**: standing manoeuvres the player can order their own ship to hold — match speed, keep distance, orbit at a range — are part of the flight model, not an autopilot bolted beside it. See §14.

## 5. Combat **[likely]**

- Readable, positional dogfighting and gunline fights — Starsector's clarity in 3D. Encounter sizes: 1–20 ships, not hundreds. Capital hulls (§11) raise the *mass* of an engagement, not its headcount.
- **Defense layers**: shields (regenerating, **directional facings** — ✅ Q2, `docs/decisions/002-shield-facings.md`) → armor (ablative, locational) → hull.
- **Systems damage** **[core, promoted 2026-08-28]**: a ship is a set of **mounts** (§11) and a mount can be knocked out before the hull dies. Engines, turrets, shield generators, sensors and covert suites each carry their own condition, are resolved against **where they physically sit on the hull**, and stop working when destroyed. Disabling a ship is therefore a real tactic distinct from killing it — which is what makes interdiction, electronic warfare, and "leave the freighter alive, kill its drive" into playable ideas rather than flavour text. This has been promised in this document since day one and was previously unbuilt; see `docs/decisions/014-ship-mounts.md`.
- **Weapons**: projectile (travel time, lead indicator), beam (instant, power-hungry), missiles and torpedoes (countermeasures exist). **Mount sizes and classes per ship** (§11); energy weapons draw from the ship power budget — power management (**Elite-style WEP/ENG/SYS pips** — ✅ Q3, `docs/decisions/003-power-management.md`) is the pilot's tactical dial.
- Death: ship destroyed → respawn at last dock in the same ship and fit, cargo lost, insurance deductible charged (fraction of ship + fit value, clamped at zero credits — no debt); opt-in hardcore mode deletes the campaign instead (✅ Q6, `docs/decisions/007-death-penalty.md`). **[core]**

## 6. Economy & Trading **[core]**

The flagship simulation system, and the spine the sandbox hangs on:

- Agent-based: stations **produce and consume** real goods on production chains. NPC freighters physically haul (full sim near player, coarse sim elsewhere — see engine plan §2.7).
- **The material tree** **[core, expanded 2026-08-28]**: goods sit in tiers, and a station's place in the tree is what it is *for*.
  - **T0 raw** — ores by metal class (ferrous, light, precious, rare-earth), ices (water, volatiles), gases (hydrogen/helium, noble), silicates, organics, and **salvage** recovered from wrecks.
  - **T1 refined** — alloys, precious and rare-earth concentrates, water, fuel, industrial gases, polymers, ceramics, and **reclaimed alloy**, which is the recycling leg: salvage re-enters the tree here rather than at the top, so wrecks are an *input* to industry and not just loot.
  - **T2 components** — electronics, power cells, superconductors, optics, hull plate, structural frame, drive assemblies, shield emitters, weapon cores, sensor arrays, computer cores. These are what a **mount fitting** is made of (§11), which is what finally ties outfitting to the economy: the gun you buy was manufactured somewhere by somebody out of things somebody mined.
  - **T3 assemblies** — ship module kits, **station module kits** (§12), hull and drive sections. The construction tier.
  - **Consumer goods** — foodstuffs, medical supplies, textiles, luxuries.
  - **Contraband** — see §13. Contraband is not a tier; it is a *legality*, and the same crate is cargo in one jurisdiction and a crime in the next.
- Prices are local, from actual station stocks. No global price board; information (and stale information) has value.
- The player is small relative to the economy at start — you ride the waves; late-game you can make waves (flood a market, starve a war effort, own freighters).
- Piracy, blockades, and war damage propagate: raided traders mean shortages mean prices mean missions. Systems feeding systems is the whole point.

## 7. Factions & Reputation **[likely]**

- 4–7 major handcrafted factions (distinct ideology, territory, ship aesthetic, economy specialty) + minor factions (pirate clans, corporations, independents) partly generated.
- **Every major faction declares its own legality table** (§13): what it treats as contraband, what it merely licenses, and how hard it looks. A faction's *laws* are as much of its character as its ships — the Hegemony's list is long and its patrols are thorough; the Compact barely has one and cannot afford to enforce it.
- **The black market is a faction template, not a place** **[added 2026-08-28]**: a `kind = "shadow"` faction has **no territory and no stations of its own**. It operates *inside* other factions' stations, wherever a station's module list includes black-market services (§12), and it flies purpose-built covert-ops hulls crewed by pilots who make a living going around the law. Standing with a shadow faction is earned by exactly the acts that cost you standing with the law, which is what makes smuggling a genuine allegiance rather than a tolerated exploit. See `docs/decisions/017-law-and-transponders.md`.
- **A faction's reach is visible before you meet it** **[added 2026-08-28]**: how heavily a faction garrisons a system, and how quickly it answers trouble there, falls off with distance from its capital — which is why a border system flying somebody's flag can still be a bad place to be robbed. Security ratings (§15) are where a faction's *claim* and its actual *grip* are allowed to differ.
- Live relations: wars, truces, and border shifts happen on the coarse sim from real events (Stellaris-flavored personality weights driving Lua-side decision rules).
- Player reputation per faction, moved by actions (contracts, kills, smuggling, rescues). Consequences: market/equipment access, docking rights, patrol hostility, mission tiers. Reputation is a web, not a bar — helping one side is taken personally by their enemies, and being liked by smugglers is noticed by police.

## 8. Activities

- **Missions/contracts** **[likely]**: procedurally generated from real simulation state plus the authored campaign spine and side questlines (✅ Q7, `decisions/008`). Lua-authored. Leads also arrive through people rather than boards — see §12's bar.
- **Mining & salvage** **[likely]**: asteroid prospecting/extraction across the ore classes of §6; gas and ice harvesting; battlefield salvage (wrecks from real battles persist and are lootable) feeding the recycling leg of the material tree.
- **Exploration & scanning** **[likely]**: unvisited systems are unknown; scanning reveals bodies, signals, derelicts, hidden caches. Authored secret systems (§3) are the payoff at the far end of this. The fringe pays explorers.
- **Outfitting** **[core]**: ships are built around **named mounts** under power and mass budgets — see §11. EVE/Starsector-style fitting depth is a primary progression axis. Buy ships outright; own multiple; store at stations.
- **Smuggling and running dark** **[core, added 2026-08-28]**: see §13.
- **Crew** **[likely]**: hired at stations for a one-time cost, flat passive bonuses while aboard, berth count per ship; crew defs share the module stat-modifier vocabulary (✅ Q5, `decisions/006`). **Captains** — crew who can be given a *ship* rather than a bonus — are the v2 escalation of this, §14.

## 9. Scope Guardrails — v1 Non-Goals

Explicitly **out** for the first shipped version (revisit only after v1). **Amended 2026-08-28**; the Depth Arc moved two of these and left the rest standing.

- ❌ Multiplayer of any kind
- ❌ Planetary landings / atmospheric flight (planets are scenery and orbit anchors)
- ❌ Block/modular ship *construction* (Avorion-style building). **Still out, and mounts are not it**: fitting a turret to a mount an author placed is outfitting; welding a hull out of blocks is building.
- ❌ Walking around ships/stations (no first-person on foot). **Still out.** Station interiors — bars, brokers, contacts (§12) — are *screens with people in them*, never places you walk.
- ❌ Colony / population management
- ❌ Hundreds-of-ships battles; VR
- ⏳ **Fleets with orders, owned freighters, captains** — was "yes, late" and unscheduled since this document was written. **Now scheduled as the v2 arc** (§14), specced before v1 ships so v1 does not build anything that forecloses it.
- ⏳ **Player-built stations** — station *modules* (§12) enter v1 as the vocabulary the generator composes NPC stations from. Player **construction** from the same vocabulary is v2. This is the one non-goal the arc genuinely reverses, and it is reversed in halves: describing stations with modules costs nothing and pays immediately; letting the player build one is a different game and waits.

### The v2 arc, committed and deferred

Recorded here so v1 does not paint over it: **multi-system entity simulation** (§14.4), **captains and standing orders**, **fleets and formations**, and **station construction**. Each is specced in `docs/engine-plan.md` §4 before v1 ships.

## 10. Open Questions

| # | Question | Leaning | Decide by |
|---|---|---|---|
| Q1 | Player jump drives (free travel) vs. gates-only | ✅ Decided: gates baseline, drive as late-game unlock (`decisions/004`) | Phase 7 |
| Q2 | Shield facings (directional) vs. single bubble | ✅ Decided: directional (`decisions/002`) | Phase 6 |
| Q3 | Power management UI (pips vs. module toggles) | ✅ Decided: Elite-style pips (`decisions/003`) | Phase 6 |
| Q4 | Time compression out of combat (SETA-like) | ✅ Decided: no — 1× real time, tune cruise/layout instead (`decisions/005`) | Phase 7 |
| Q5 | Crew/officers as passive bonuses | ✅ Decided: trivial version in v1 — hired flat passive bonuses on module machinery (`decisions/006`) | Phase 8 |
| Q6 | Death penalty severity / ironman modes | ✅ Decided: insurance deductible default + opt-in hardcore (`decisions/007`) | Phase 8 |
| Q7 | Story campaign vs. pure sandbox + anchors | ✅ Decided: authored campaign spine, sandbox complete without it (`decisions/008`) | Phase 8 |
| Q8 | Hardpoints: unified named mounts vs. mounts beside slot counts | ✅ Decided: **unified named mounts**; slot counts retire (`decisions/014`) | Phase 31 |
| Q9 | Do owned ships exist while the player is elsewhere? | ✅ Decided: **full entities in every system that holds one**; the frame-of-reference change is its own phase (`decisions/015`) | Phase 38 |
| Q10 | Station modules: composition, construction, or both | ✅ Decided: **generator composes in v1, player constructs in v2** (`decisions/016`) | Phase 34 |
| Q11 | How deep does law enforcement go | ✅ Decided: **full inspection loop** — hail, hold, timed cargo scan, consequence (`decisions/017`) | Phase 36 |
| Q12 | Authored systems: how placed, how modded | ✅ Decided: **TOML systems + constellations with placement rules** (`decisions/018`) | Phase 29 |
| Q13 | System security: static, dynamic, and what "negative" means | ✅ Decided: **static baseline − live danger, on a signed scale whose negative half is pirate-policed**; response diverts before it spawns (`decisions/019`) | Phase 30 |

Decisions get recorded in `docs/decisions/` and reflected here.

---

## 11. Ships: Classes, Roles & Mounts **[core, added 2026-08-28]**

This section is the **vocabulary**, not the roster. It defines the axes every ship in the game is described on, so that a faction roster, a mod's roster, and the generator's spawn tables all speak the same language. What actually gets *built* in the arc's first pass is a spine of roughly eight to ten hulls (see `docs/engine-plan.md` Phase 32); the rest of the grid is named, sized and left for art.

### 11.1 Hull classes

A hull class fixes the scale band, the mount budget, and roughly what it costs to keep alive. It is the ship's *weight*, and nothing else.

| Class | Name | Length | Mount budget | Crew | Notes |
|---|---|---|---|---|---|
| 0 | **Skiff** | 8–20 m | 1–2 S | 1 | Mining skiffs, shuttlepods, escape craft, drones. Barely a ship. |
| 1 | **Light** | 20–45 m | 2–4 S | 1–2 | The starting band. Interceptors, scouts, couriers, covert scouts. |
| 2 | **Medium** | 45–120 m | 4–8 S/M | 2–8 | Frigates, corvettes, blockade runners, small haulers, EW pickets. |
| 3 | **Heavy** | 120–300 m | 8–14 M | 8–40 | Destroyers, freighters, remote-logistics ships, command frigates. |
| 4 | **Cruiser** | 300–600 m | 14–22 M/L | 40–200 | Cruisers, light carriers, industrial platforms, strategic hulls. |
| 5 | **Capital** | 600–1200 m | 22–34 L | 200–1000 | Battlecruisers, battleships, carriers, jump freighters. |
| 6 | **Super-capital** | 1.2–3 km | 34–50 L/XL | 1000+ | Dreadnoughts, supercarriers, capital logistics, construction ships. |
| 7 | **Titan** | 3 km+ | 50+ XL | — | One per major faction at most. Endgame presence; may never be player-owned. |

The band is a **soft** contract: a class-3 hull is expected to look and fly like a class-3 hull, and a def that violates its own class band is a content bug the tools should say so about, not a schema error.

### 11.2 Role families

A role says what the hull is *for*. Class × family is the grid; a named ship type sits in one cell.

| Family | What it does |
|---|---|
| **Line** | Shoots things and is shot at. Interceptor → frigate → destroyer → cruiser → battlecruiser → battleship → dreadnought. |
| **Carrier** | Projects force it does not itself carry: hangars, drones, fighters. Light carrier → carrier → supercarrier. |
| **Logistics (economic)** | Moves matter. Hauler → freighter → deep-space transport → jump freighter; personnel transports; blockade runners; mining barges, gas harvesters, salvagers, refinery tenders. |
| **Support** | Makes other ships better or worse. Remote logistics (repair/shield projection), electronic warfare, command hulls, interdiction and tackle. |
| **Covert** | Operates where it should not be. Covert-ops scouts, stealth bombers, smuggler hulls, recon. Defined by what it can carry (§13) as much as by its stats. |
| **Industrial** | Builds and services. Construction ships, station tenders, module haulers, mobile refineries, mining platforms. The family v2's station construction (§12) is built on. |

### 11.3 The named types, placed on the grid

Not every cell is filled, and that is deliberate — an empty cell is a design statement.

| | Line | Carrier | Logistics | Support | Covert | Industrial |
|---|---|---|---|---|---|---|
| **1 Light** | Interceptor | — | Courier, Shuttle | Tackle | Covert Scout | Mining Skiff |
| **2 Medium** | Frigate, Corvette | — | Hauler, Personnel Transport | EW Picket, Interdictor | Blockade Runner, Smuggler | Prospector |
| **3 Heavy** | Destroyer | — | Freighter | Remote Logistics, Command Frigate | Stealth Bomber | Salvager, Gas Harvester |
| **4 Cruiser** | Cruiser | Light Carrier | Bulk Freighter | EW Cruiser, Strategic Cruiser | Recon Cruiser | Mining Barge, Refinery Tender |
| **5 Capital** | Battlecruiser, Battleship | Carrier | Jump Freighter, Deep-Space Transport | Fleet Command Ship | — | Industrial Platform |
| **6 Super-cap** | Dreadnought | Supercarrier | Capital Logistics | Capital Command | — | Construction Ship |
| **7 Titan** | Titan | — | — | — | — | — |

### 11.4 Faction specialisation

The grid above is the *genus*. A faction roster names its own hull for a cell, with its own stats, silhouette and mount layout — and **a faction is characterised as much by the cells it leaves empty as by the ones it fills**. The Freight Guild builds no battleships. The Hegemony builds no covert hulls and would not admit it if it did. A pirate clan has no logistics tier above medium because it steals what it needs. Rosters must be able to say "we do not build that", which the def format supports by simple absence.

### 11.5 Mounts

**A ship is its mounts.** A mount is a named, typed, sized place on a hull where exactly one fitting goes. This replaces the old model of one weapon plus four integer slot counts entirely; see `docs/decisions/014-ship-mounts.md`.

A mount declares:

- **`id`** — unique within the hull, and stable: a save refers to a fitting by the mount it sits in.
- **`kind`** — what may be fitted: `turret`, `fixed`, `launcher`, `bay`, `engine`, `thruster`, `shield`, `armor`, `utility`, `subsystem`, `hangar`, `dock`.
- **`size`** — `small` | `medium` | `large` | `xlarge`. A mount accepts its own size or smaller; fitting small kit to a big mount wastes the mount, which is the trade the player is making.
- **`at` / `aim` / `arc`** — position in metres in the hull frame, facing, and traverse. **Present means external**: the fitting is drawn on the hull and can be shot at where it sits. **Absent means internal**: it exists, it can be destroyed by damage that reaches it, and it is never drawn. **`arc` is the full cone angle centred on `aim`**, so a `270` ring reaches 135° either side and is blind only through the hull it is bolted to; `0` is a gun bolted down, aimed by flying the ship.

A gun with an arc is **laid by a gunner**: it swings onto the ship the pilot has selected, leads it with its own projectile speed, and follows the nose when there is nothing selected, when the selection is out of that gun's reach, or when the selection is **not hostile**. A gun that cannot reach round far enough **holds its fire** rather than shooting into its own stop. A gunner does not open on a neutral, so **you open with the nose and the rings join once it is a fight** — which is what lets a hauler leave its turrets armed while it hails a patrol or cuts a rock. That is what makes a turreted hauler a different thing to fly from a fighter with a nose gun, and it is why mount layout is a hull's character rather than a stat block.

**Subsystems** are internal mounts, and they are where a ship's *character* beyond its guns lives: sensors and data gathering, mining rigs, fire control, drive tuning, automated flight control (which is what makes §14's standing orders and later a captain possible), science, comms, and covert suites (§13). A hull that cannot take a covert suite cannot be a smuggler, however it is flown.

Fittings are manufactured goods (§6 T2), which is what connects the fitting screen to the economy.

---

## 12. Stations & Modules **[core, added 2026-08-28]**

A station is **a list of modules**, and everything about it follows from that list: what it produces and consumes, which screens it offers when you dock, what its silhouette looks like, and who you can meet inside it. In v1 the **galaxy generator composes** every NPC station from this vocabulary. In v2 the **player builds** from the same vocabulary (§9, `decisions/016`).

> **Naming.** `[[module]]` is currently ship outfitting. Phase 31 renames ship fittings to `[[component]]` — a thing that occupies a mount — which frees `[[module]]` for its natural meaning here. See `docs/decisions/014-ship-mounts.md`.

### Module families

| Family | Modules | What it gives the station |
|---|---|---|
| **Power** | Solar array, fission plant, fusion plant, capacitor bank | The budget every other module draws against. A station is power-limited exactly as a ship is. |
| **Habitat** | Habitat ring, barracks, medical bay | Population, which gates crew hiring and consumer demand. |
| **Storage** | Bulk hold, cryo hold, hazardous hold | Stock capacity, per goods class. A station cannot hold what it has no hold for — including contraband. |
| **Industry** | Refinery, smelter, fabricator, component works, shipyard, drydock | The production chains of §6. Which tier a station sits at *is* its industry modules. |
| **Commerce** | Market floor, brokerage, outfitter, shipyard sales | The Trade, Outfitting and Shipyard screens. |
| **Recreation** | Bar, restaurant, concourse, casino, resort | Where people are, which is where rumours, contacts and leads are (below). Also consumer-goods demand. |
| **Services** | Docking control, refinery service, insurance office, crew hall, mission board | The remaining dock screens. |
| **Shadow** | Fence, unlicensed clinic, ghost dock, data haven | Black-market services (§7, §13) — present on stations of *any* owner, which is exactly how a shadow faction operates without territory. |

**A station's screens are derived from its modules, not hardcoded.** The eight tabs a station currently always shows become a *consequence*: a mining outpost with no market floor has no Trade tab, and finding one that does is worth flying to. This is the single biggest gameplay dividend of the module system and it costs nothing extra once the list exists.

### People, rumours and leads **[likely]**

Recreation modules put **people** on a station — as a screen, never as a place you walk (§9).

- **Generated ambient**: what the barkeep and the regulars talk about is read from live simulation state — a real shortage two jumps out, a real war front, a real raid last night, a real bounty. Always true, endless, never memorable.
- **Authored characters**: a small cast of named persons with persistent state, placed by the generator into stations whose modules suit them, with Lua hooks for questlines, black-market introductions and campaign beats. Memorable, finite.
- The two coexist: ambient chatter is the texture, a named character is the hook. A lead heard in a bar is a real mission posted by real state, which is what stops this being a dialogue minigame bolted to a space sim.

---

## 13. Law, Contraband & the Transponder **[core, added 2026-08-28]**

### The transponder

Every ship broadcasts an identity. In **policed space** — systems held by a faction with the will to enforce — running with your transponder **off** is itself an offence, and it is also the only way to do a number of profitable things. The switch is the whole mechanic: it is always available, it is never free.

**“Policed space” is a number, not a mood** (§15): how likely a patrol is to notice you, how many turn up, and how fast, all read off the system's security rating. The same run is routine at 0.9 and suicidal at 0.2 — and below zero the ships that stop you are not police at all.

### The inspection loop

Law is played out, not rolled (`docs/decisions/017-law-and-transponders.md`):

1. A patrol notices you — dark transponder, a bad reputation, a random check, or a tip.
2. It **hails** and orders you to hold. Running is an answer, and a legible one.
3. If you hold, it runs a **cargo scan over real seconds** at real range. You can break off mid-scan; that is also an answer.
4. What it finds is judged against **that faction's** legality table: legal, licensed, or contraband.
5. Consequence: waved on, fined, cargo impounded, a bounty posted, or shot at — scaling with what was found, your standing, and how the stop went.

### Countermeasures

Covert **subsystem** mounts (§11.5) are the counterplay, and they are why covert hulls exist: signature dampeners that shrink scan range, transponder spoofers that broadcast somebody else's identity, shielded holds that a scan reads as empty, and sensor packages that see the patrol before it sees you. Each is a mount that is not carrying a gun — that is the cost.

### Legality is per faction

The same crate of stims is ordinary medicine in the Compact, a licensed pharmaceutical in the Guild, and ten years in a Hegemony labour camp. Each major faction declares its own **contraband** and **restricted** lists, and a smuggler's real skill is knowing whose space they are in. Cargo is never intrinsically illegal; jurisdictions are.

---

## 14. Ship Commands, Captains & Fleets

### 14.1 Commanding your own ship **[core, v1]** — ✅ shipped as engine-plan Phase 28, 2026-08-28

Standing manoeuvres the player orders and the ship holds: **match speed**, **maintain distance**, **orbit at a range**, **hold station**, **follow**, **align to**, plus the interactions that already exist as keys — hail, request docking, scan, autopilot to.

**⚑ SIX OF THOSE SEVEN SHIPPED, AND `align to` IS THE ONE THAT DID NOT.** `CommandMode` is `Autopilot`, `Orbit`, `MatchSpeed`, `KeepDistance`, `Hold`, `Follow`. Align-to was never carried into Phase 28's spec — it was lost between this section and that one rather than refused — so it is **still owed**, and it is a small one: a mode that aims the nose and commands nothing else. Everything else in this section is live, by key and by menu alike.

Two ways to give an order, and **both are first-class**:

- **Right-click an object** — in space, on the radar, or on the map — and get a context menu of everything you can do to that thing. This is the discoverable path and the one that makes the map a place you can act from (Pillar 1, as amended).
- **A key binding.** Every command is bindable. The common ones ship with defaults; the rest are unbound and available. A player who never opens a menu can fly the whole game.

### 14.2 Captains and standing orders **[v2]**

A **captain** is crew you give a *ship* to instead of a bonus. Hired at a crew hall, they take a ship out of your storage and fly it. A captain accepts the same command vocabulary as your own ship plus **standing orders** that outlive the session: mine here, haul between there and there, patrol this, escort that, sell when the price clears X.

### 14.3 Fleets and formations **[v2]**

Ships can be grouped into a **fleet** with a commander, a **formation** (customisable), and a **fleet order** that the fleet resolves *as a unit according to what it is made of*. A mining fleet ordered to work a field does not need to be told that the miners stay at the rock, the haulers shuttle to the nearest refinery, and the escorts split to cover both — that is what "mining fleet" *means*. The fleet's composition is its plan.

### 14.4 The prerequisite nobody asks for

All of §14.2 and §14.3 rest on one engine change: **a ship you own that is not in your system has to actually exist**. Today exactly one system is instantiated and every position in the sim is expressed in that system's barycentre frame. Multi-system entity simulation is therefore its own phase, ahead of captains and fleets, and it is the largest single item in the v2 arc. See `docs/decisions/015-multi-system-simulation.md`.

---

## 15. System Security **[core, added 2026-08-28]**

The GDD has promised a civilized core, a contested frontier and a lawless fringe
since day one. §15 is where that promise stops being a colour on the galaxy map
and becomes **a number every system carries, that the player can read before
they fly there**.

### The scale is signed, and the sign is the whole idea

| Band | Who polices it | What that means for you |
|---|---|---|
| **High positive** | A major faction, heavily | Thick patrols. Fire on someone and a wing is on you in seconds. The safest place to be law-abiding and the worst place to be anything else. |
| **Low positive** | A major faction, thinly | A patrol exists. It is probably somewhere else, and it will take a while. |
| **Zero** | Nobody | Nobody comes. Not for you, and not for the people robbing you. |
| **Negative** | A pirate clan | Somebody else's law. It answers intrusion exactly the way a navy does — and the wing it dispatches is coming for *you*. |

**Negative security is not the absence of security.** It is security belonging to
someone whose interests you are on the wrong side of. A clan-held system is
patrolled, watched, and responded to; the difference is who the response is for.
This is what makes the deep fringe a *place* rather than an empty region — and it
is the home the black market (§7) has always needed.

### What the rating is made of

Two numbers, deliberately, because they answer two different questions:

- **The baseline** — a property of the *place*. Who owns it, how far it sits from
  their capital, and what kind of owner they are. Written when the galaxy is
  generated; an authored system (§3) may declare its own.
- **The live rating** — the baseline minus **what is happening there now**: raids
  in progress, live front lines. A quiet core system and the same system three
  hours into a war do not read the same.

**Patrol strength reads the baseline. Danger, attrition and response *time* read
the live rating.** A navy's garrison does not evaporate because pirates turned
up — if anything it digs in — and wiring it the other way makes a spiral where
one raid thins the patrols and buys the next one. What live pressure *does* cost
you is speed: busy patrols are slower to arrive, and that is the honest penalty.

### Response is a journey, not a timer

When something happens that the local authority cares about, the nearest patrol
that is not already fighting is **sent**. If there is none in range, one comes
**from a station or a gate**, and it flies there.

So response time is a real transit across real distance. A provocation over a
station pad and the same provocation at a gate 600,000 km out are different
events, with no special-casing and no script — and in a zero-security system the
difference stops mattering, because nothing is coming either way.

### What it changes about how the game is played

- **Routes become decisions.** The safe road and the short road stop being the
  same road, and the map is where you find that out.
- **The fringe pays because nobody protects you.** Difficulty and opportunity
  rising toward the fringe (§3) finally has a mechanism instead of a tuning
  table.
- **It is the dial the law runs on.** Whether a patrol bothers to stop and scan
  you (§13) is a question about where you are, not just about what you are
  carrying.
- **It gives the shadow faction an address after all.** §7 says the black market
  has no territory — and that stays true. But a negative-security system is
  where its people are already at home.

See `docs/decisions/019-system-security.md`.
