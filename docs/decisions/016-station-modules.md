# 016 — Stations are lists of modules: the generator composes in v1, the player builds in v2

- **Date**: 2026-08-28
- **Status**: accepted

## Context

The Depth Arc asks for stations *"built out of modules based on what their purpose
is for"* — power, habitat, storage, recreation, industry, commerce, leisure,
gambling, black-market services.

Priced against the code on 2026-08-28:

- **A station today is an archetype id, four rate lists and one mesh name.**
  `StationDef` has `produces`, `consumes`, `feedstock`, `producesFrom`,
  `stockCapacity`, `refineInput`/`refineOutput`, three region weights and
  `model = "station"`. Four archetypes ship: agri, mine, refinery, factory.
- **Every station in the galaxy is drawn with the same mesh.** There is exactly
  one `station.forge` in `assets/meshes/`, and `StationDef::model` defaults to it.
- **Every docked station shows the same eight tabs** — `station_screen.cpp:42`
  hardcodes `{"Trade", "Outfitting", "Shipyard", "Crew", "Factions", "Missions",
  "Survey", "Refinery"}` for all of them. A mining outpost in the fringe offers a
  shipyard because the tab list is a literal.
- **`[[module]]` is taken.** It is ship outfitting (`ModuleDef`, `ModuleSlot`),
  and has been since Phase 8a.
- **GDD §9 lists base building as a v1 non-goal.**

The ask is therefore two features wearing one word, and they have very different
prices: *describing* a station as a module list is a data change that pays
immediately, while *letting the player build one* is a construction game.

## Decision

**One module vocabulary, two consumers, sequenced.**

- **v1 — the generator composes.** Every NPC station is generated as a list of
  modules. The module list *derives* what the station produces and consumes, how
  much it can hold, which screens it offers when you dock, and eventually what it
  looks like. `StationDef`'s rate lists stop being authored per archetype and
  become the sum of what its modules do; an archetype becomes a *recipe* — a
  weighted module list — rather than a bag of numbers.
- **v2 — the player constructs.** The same vocabulary, bought as T3 station module
  kits (GDD §6), delivered and assembled. GDD §9's non-goal is amended in halves
  and says so explicitly.
- **Ship outfitting `[[module]]` is renamed `[[component]]`** in the mount phase
  (`decisions/014`), which frees the noun before this phase needs it. Doing the
  rename in the phase that is already rewriting every ship def costs nothing;
  doing it later would be a second breaking change to the same files.

**The dividend that justifies v1 on its own**: a station's tabs become a
consequence of its modules. No market floor, no Trade tab. This turns eight
identical stations into stations worth choosing between, and it is *free* once
the list exists — the tab array becomes a filter over the module list.

## Alternatives considered

- **Composition only, ever.** Keeps GDD §9 completely intact and still delivers
  station variety, the shadow-faction footprint, and the bar. Rejected only
  because the user wants player construction eventually; the design is otherwise
  identical, so this remains the fallback if v2 is never reached — nothing in v1
  depends on construction happening.
- **Go straight at player construction.** Rejected on sequencing, not on merit: it
  needs the module vocabulary, the T3 material tier, industrial hulls and
  multi-system simulation to all exist first, and building it first would mean
  authoring the vocabulary against one consumer that does not exist yet instead
  of against 124 stations that do.
- **A second word for station modules** (`[[facility]]`, `[[station_module]]`)
  keeping ship modules as-is. Rejected: it preserves a name that is wrong on both
  sides of the analogy — the ship thing is a *component in a mount*, and calling
  it a module was the original imprecision.

## Consequences

- **The economy's authored rates move from archetypes to modules.** Four station
  defs become N module defs plus four recipes. Balance work: the existing rates
  were tuned against what the trader fleet can move (`stations.toml` documents
  this at length, including why every producer runs a quarter ahead of its
  customers), and that tuning has to survive the decomposition. **This is the
  main risk of the phase**, and it is a numbers risk, not a code risk.
- **Station screens become data-driven.** `station_screen.cpp`'s tab array is
  replaced by a derivation, and each tab gains an "absent" case that must read as
  deliberate rather than broken.
- **`stock_capacity` becomes per goods class**, since holds are modules — which is
  what lets a station be unable to store contraband (GDD §13).
- **The shadow faction gets a footprint with no territory** (`decisions/017`):
  shadow modules can appear on any owner's station, which is the mechanism that
  makes a stationless faction operable.
- **Station meshes become composable eventually.** Not in v1 — one mesh still
  draws every station — but the module list is the data a later art phase needs,
  and recording it now costs nothing.
- **Save format**: stations gain a module list. Bumps `kSaveVersion`. Since the
  mount phase (`decisions/014`) also bumps it and both land in the same arc,
  the arc should expect to invalidate saves more than once and say so rather
  than pretending otherwise.
