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
4. **Yours to break.** Sandbox first: no forced story, systems over scripts, all content data-driven and Lua-scripted. The base game is built like a mod so mods are first-class.

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
- **Travel**: jump gates between systems as baseline; player-owned jump drives as a late-game unlock is **[open]** (Q1, §10).
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
- **Defense layers**: shields (regenerating, directional facings **[open]** Q2) → armor (ablative, locational) → hull (systems damage: engines, weapons can be knocked out before destruction).
- **Weapons**: projectile (travel time, lead indicator), beam (instant, power-hungry), missiles (countermeasures exist). Hardpoint sizes/classes per ship; energy weapons draw from the ship power budget — power management (weapons/engines/shields pips or similar **[open]** Q3) is the pilot's tactical dial.
- Death: ship destroyed → respawn at last dock with insurance cost; harsher modes later. **[likely]**

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

- **Missions/contracts** **[likely]**: procedurally generated from real simulation state (a station short on meds generates a haul contract; a raided sector generates patrol bounties) plus authored questlines at story anchors. Lua-authored generators.
- **Mining & salvage** **[likely]**: asteroid prospecting/extraction; battlefield salvage (wrecks from real battles persist and are lootable).
- **Exploration & scanning** **[likely]**: unvisited systems are unknown; scanning reveals bodies, signals, derelicts, hidden caches. The fringe pays explorers.
- **Outfitting** **[core]**: ships have hardpoints + module slots (weapons, shields, engines, cargo, utility) under power/mass budgets — EVE/Starsector-style fitting depth is a primary progression axis. Buy ships outright; own multiple; store at stations.

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
| Q1 | Player jump drives (free travel) vs. gates-only | Gates baseline; drive as late-game unlock | Phase 7 |
| Q2 | Shield facings (directional) vs. single bubble | Directional (rewards positioning) | Phase 6 |
| Q3 | Power management UI (pips vs. module toggles) | Pips-style triage | Phase 6 |
| Q4 | Time compression out of combat (SETA-like) | Yes, needed at these distances | Phase 7 |
| Q5 | Crew/officers as passive bonuses | Post-v1 unless trivial | Phase 8 |
| Q6 | Death penalty severity / ironman modes | Insurance default + optional hardcore | Phase 8 |
| Q7 | Story campaign vs. pure sandbox + anchors | Sandbox + authored anchor questlines | Phase 8 |

Decisions get recorded in `docs/decisions/` and reflected here.
