# Sol — Game Design Document

Working title: **Sol** (placeholder). Single-player 3D space sandbox for PC (Windows first). Built on the from-scratch Sol Engine (`docs/engine-plan.md`).

Confidence markers used below — **[core]**: settled, build against it; **[likely]**: direction set, details open; **[open]**: deliberately undecided, see §10.

---

## 1. Vision

You are one pilot in a galaxy that runs with or without you. Start with a single underwhelming ship and a handful of credits; end up wealthy, feared, connected, or all three — through trading, fighting, mining, exploring, or any mix you invent. The galaxy is procedurally generated but governed by handcrafted factions and rules, and its economy and politics are genuinely simulated: prices move because NPC freighters actually hauled the goods, and a war front shifts because patrols actually lost.

### Pillars

1. **You are a pilot, not a cursor.** Everything is experienced from a ship you directly fly, in first or third person. No omniscient strategy view as the primary mode — the map is a tool, the cockpit is home.
2. **A universe that doesn't wait for you.** Factions, traders, and pirates pursue their own goals on a real simulation. The player is a participant, not the protagonist of a script.
3. **From nobody to somebody.** Long, earned progression: better ships, better gear, reputation, wealth, and eventually influence. No levels — power comes from what you own, know, and are owed.
4. **Yours to break.** Sandbox foundation: systems over scripts, all content data-driven and Lua-scripted, and the base game built like a mod so mods are first-class. v1 ships an authored **campaign spine** that rides on top of the real systems and paces headline unlocks — but it is ignorable after its opening, and a player who walks away keeps a complete game (✅ Q7, `docs/decisions/008-campaign-spine.md`).

### Influences map

| Game | What we take | What we leave |
|---|---|---|
| Elite Dangerous | Flight feel, cockpit immersion, sense of scale | Scale so vast it's empty; grind pacing |
| X4: Foundations | Living economy, NPC-driven universe, empire *seeds* | UI sprawl; station-walking |
| Starsector | Combat legibility, faction dynamics, outfitting depth | Top-down perspective |
| EVE Online | Market/economy depth, reputation webs, "ships are tools" | Multiplayer, full-loot harshness |
| Stellaris / Civ | Faction personalities, strategic pacing of a galaxy over time | Playing *as* the empire (we fly inside one) |
| Avorion | Procedural galaxy structure, difficulty-by-region | Block-based ship building (not at launch) |

---

## 2. Player Fantasy & Core Loops

**Fantasy**: independent starship captain carving a life out of a frontier galaxy.

- **Minute-to-minute**: fly the ship — maneuver, manage speed/assists, fight, dodge, dock, scan. This loop must feel good in isolation by Phase 4 or nothing above it matters. **[core]**
- **Session (30–90 min)**: run a contract, work a trade route, clear a pirate nest, prospect an asteroid field, scout a new system; return, sell, refit. **[core]**
- **Long-term (tens of hours)**: climb the ship ladder, build faction standing (and enemies), unlock better markets/equipment/regions, accumulate wealth toward big purchases; late-game influence (own freighters flying routes for you — empire *seeds*, not empire management). **[likely]**

Progression is horizontal as much as vertical: a mining barge, a fast smuggler, and a heavy gunship are different games, not tiers. **[likely]**

---

## 3. World & Universe

- **Galaxy**: procedurally generated from a seed at new-game — 50–150 star systems (target; tune for density of interest, not raw size) connected by jump-gate lanes forming a graph with regions: civilized core, contested frontier, lawless fringe. Difficulty and opportunity rise toward the fringe (Avorion-style gradient). **[core]**
- **Systems**: each system is a flat-ish playfield of real 3D space — star, planets (scenery + orbit anchors), stations, asteroid fields, gates, points of interest. Distances in the hundreds of thousands of km; in-system **cruise drive** (superlight throttle mode, interruptible) covers them. **[likely]**
- **Travel**: jump gates between systems as baseline; player-owned jump drive as a late-game unlock (decided, `decisions/004`). No time compression — the game runs at 1× real time, with cruise speed and system layout tuned so legs stay short (decided, `decisions/005`). **[core]**
- **Handcrafted over procedural**: faction identities, ship/weapon rosters, economy rules, story anchors (unique stations, derelicts, questlines) are authored; the generator *places* them according to rules. Procedural breadth, authored flavor. **[core]**
- **Persistence**: one continuous save-game world; full state (economy, faction relations, wrecks that matter) persists. Same seed + same content version ⇒ same starting galaxy. **[core]**

---

## 4. Flight Model **[core]**

Newtonian 6-DoF under the hood; layered assists on top (Elite's proven compromise):

- **Assist on (default)**: velocity/rotation damping makes the ship fly "atmospherically" — approachable, still momentum-aware. Soft speed cap per ship for combat balance and AI fairness.
- **Assist off (toggle)**: raw Newtonian for drift turns and expert play. No cap on velocity except gameplay-pragmatic limits.
- Ships differ meaningfully in mass, thrust envelope (main vs. maneuvering), agility, and power budget — handling is a primary stat, not flavor text.
- Mouse+keyboard first-class; gamepad next; HOTAS eventually. **[likely]**

## 5. Combat **[likely]**

- Readable, positional dogfighting and gunline fights — Starsector's clarity in 3D. Encounter sizes: 1–20 ships, not hundreds.
- **Defense layers**: shields (regenerating, **directional facings** — ✅ Q2, `docs/decisions/002-shield-facings.md`) → armor (ablative, locational) → hull (systems damage: engines, weapons can be knocked out before destruction).
- **Weapons**: projectile (travel time, lead indicator), beam (instant, power-hungry), missiles (countermeasures exist). Hardpoint sizes/classes per ship; energy weapons draw from the ship power budget — power management (**Elite-style WEP/ENG/SYS pips** — ✅ Q3, `docs/decisions/003-power-management.md`) is the pilot's tactical dial.
- Death: ship destroyed → respawn at last dock in the same ship and fit, cargo lost, insurance deductible charged (fraction of ship + fit value, clamped at zero credits — no debt); opt-in hardcore mode deletes the save instead (✅ Q6, `docs/decisions/007-death-penalty.md`). **[core]**

## 6. Economy & Trading **[core]**

The flagship simulation system, and the spine the sandbox hangs on:

- Agent-based: stations **produce and consume** real goods on production chains (ore → refined → components → ships/equipment). NPC freighters physically haul (full sim near player, coarse sim elsewhere — see engine plan §2.7).
- Prices are local, from actual station stocks. No global price board; information (and stale information) has value.
- The player is small relative to the economy at start — you ride the waves; late-game you can make waves (flood a market, starve a war effort, own freighters).
- Piracy, blockades, and war damage propagate: raided traders mean shortages mean prices mean missions. Systems feeding systems is the whole point.

## 7. Factions & Reputation **[likely]**

- 4–7 major handcrafted factions (distinct ideology, territory, ship aesthetic, economy specialty) + minor factions (pirate clans, corporations, independents) partly generated.
- Live relations: wars, truces, and border shifts happen on the coarse sim from real events (Stellaris-flavored personality weights driving Lua-side decision rules).
- Player reputation per faction, moved by actions (contracts, kills, smuggling, rescues). Consequences: market/equipment access, docking rights, patrol hostility, mission tiers. Reputation is a web, not a bar — helping one side is taken personally by their enemies.

## 8. Activities

- **Missions/contracts** **[likely]**: procedurally generated from real simulation state (a station short on meds generates a haul contract; a raided sector generates patrol bounties) plus the authored campaign spine and side questlines (✅ Q7, `decisions/008` — the mission system must treat authored, stateful, multi-step questlines as first-class, not only generators). Lua-authored.
- **Mining & salvage** **[likely]**: asteroid prospecting/extraction; battlefield salvage (wrecks from real battles persist and are lootable).
- **Exploration & scanning** **[likely]**: unvisited systems are unknown; scanning reveals bodies, signals, derelicts, hidden caches. The fringe pays explorers.
- **Outfitting** **[core]**: ships have hardpoints + module slots (weapons, shields, engines, cargo, utility) under power/mass budgets — EVE/Starsector-style fitting depth is a primary progression axis. Buy ships outright; own multiple; store at stations.
- **Crew** **[likely]**: trivial v1 version — hired at stations for a one-time cost, flat passive bonuses while aboard, berth count per ship; crew defs share the module stat-modifier vocabulary (✅ Q5, `decisions/006`). Richer officers post-v1.

## 9. Scope Guardrails — v1 Non-Goals

Explicitly **out** for the first shipped version (revisit only after v1):

- ❌ Multiplayer of any kind
- ❌ Planetary landings / atmospheric flight (planets are scenery and orbit anchors)
- ❌ Block/modular ship *construction* (Avorion-style building; outfitting ≠ building)
- ❌ Walking around ships/stations (no first-person on foot)
- ❌ Full empire management (fleets with orders and owned freighters: yes, late; colony/pop management: no)
- ❌ Hundreds-of-ships battles; VR; base building

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

Decisions get recorded in `docs/decisions/` and reflected here.
