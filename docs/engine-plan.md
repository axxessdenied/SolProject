# Sol Engine — Development Plan

Working name: **Sol Engine** (placeholder). A from-scratch C++20 engine purpose-built for one game: **Sol**, a single-player 3D space sandbox (see `docs/gdd.md`). This is not a general-purpose engine; every system exists to serve that game.

Companion documents: `AGENTS.md` (working rules), `docs/gdd.md` (game design). When this plan and reality diverge, update this plan in the same change set.

---

## 1. Goals & Constraints

- **From scratch.** Platform layer, math, renderer, ECS, asset pipeline, UI — written in-repo. Approved exceptions (see `AGENTS.md` §5): Vulkan SDK, glslang (build-time), Lua 5.4, Dear ImGui (dev tools only).
- **Windows first, portable by design.** Win32 + Vulkan initially; no Win32 lock-in outside the platform layer so a Linux backend can be added later without surgery.
- **Single-player only.** No networking layer. The simulation can be tightly integrated with the client; we still keep sim/render decoupled for correctness, not for netcode.
- **Vulkan only.** One graphics backend, wrapped in a thin RHI so renderer code stays clean — not to support other APIs.
- **Data-driven and scriptable from early on.** Content in data files, game logic in Lua. Modding falls out of this for free.
- **C++20, exceptions off, RTTI off, warnings-as-errors.**

### The space-scale constraint (day one, non-negotiable)

Space games die on floating-point precision. `float` runs out of useful precision a few kilometers from origin; star systems are millions of kilometers across.

- **Simulation positions are 64-bit** (`double` in a `DVec3`; revisit fixed-point only if evidence demands it).
- **Rendering is camera-relative**: every frame, world positions are rebased against the camera (or a floating origin) before conversion to `float` for the GPU. The GPU never sees large coordinates.
- **Hierarchical coordinates**: galaxy (system id + light-year offsets) → system (double meters from system barycenter) → local/attached frames (e.g., docked at a station). Only the active system is simulated at full fidelity.

Every system that touches transforms must respect this from its first commit.

---

## 2. Architecture

Strict layering; a module depends only on layers below it.

```
┌─────────────────────────────────────────────────────┐
│  game/          C++ glue, game states, Lua scripts   │
├─────────────────────────────────────────────────────┤
│  engine/                                             │
│   ui         game UI (HUD, map, menus) [later phase] │
│   scripting  Lua VM, bindings, hot-reload            │
│   sim        flight, collision, economy, AI host     │
│   ecs        entities, components, systems, queries  │
│   assets     runtime asset loading, hot-reload       │
│   renderer   scene rendering, materials, passes      │
│   rhi        thin Vulkan wrapper                     │
├─────────────────────────────────────────────────────┤
│   platform   window, input, filesystem, time, dylib  │
│              (win32/ backend now, linux/ later)      │
├─────────────────────────────────────────────────────┤
│   core       math, memory, containers, jobs,         │
│              log/assert, events, serialization       │
└─────────────────────────────────────────────────────┘
tools/cooker/   offline asset compiler (own executable)
```

### 2.1 Platform layer (`sol::platform`)

Window creation, input (keyboard/mouse first; gamepad/HOTAS later), filesystem paths + file IO + change watching (for hot-reload), high-resolution clock, dynamic library loading. Portable API in public headers; `win32/` implementation selected at compile time (no virtual dispatch needed — one backend per build). Sits directly above `core` (uses its math/log/assert). Sanctioned platform-specific touch points outside this module: Vulkan surface creation (`engine/rhi/src/win32/`) and the ImGui platform backend bridge (`engine/ui/src/win32/`).

### 2.2 Core layer (`sol::core`)

- **Math**: `Vec2/3/4`, `Mat3/4`, `Quat`, plus `DVec3`/`DQuat` for sim-space; transforms, intersection primitives (ray/sphere/OBB/frustum), SIMD-friendly layout (SSE where it pays, scalar fallback). Column-major, right-handed, -Z forward (decide once, document in the header, never revisit). Fully unit-tested — this is the foundation everything trusts.
- **Memory**: linear/arena allocator (per-frame scratch), pool allocator, tracking wrapper for leak/usage reporting in dev builds. `std` containers with custom allocators where std suffices; bespoke containers only when profiling justifies them.
- **Job system**: work-stealing thread pool, job handles with dependencies, parallel-for. Sized to (hardware threads − 1); main thread can pump jobs while waiting.
- **Logging/assert**: leveled logging with compile-time stripping, `SOL_ASSERT` (dev) / `SOL_VERIFY` (always), crash-time log flush.
- **Events**: small synchronous event bus for engine signals (window resized, device lost, asset reloaded). Gameplay events go through sim/Lua, not this.
- **Serialization**: binary reader/writer with versioning for cooked assets and save games; a small TOML parser for human-authored data (written in-repo — it's a weekend-sized, well-specified format).
- **Hashing/ids**: FNV-1a string hashing, `StringId` for interned names, stable asset ids.
- **PRNG**: splitmix64/xoshiro-family generators with explicit streams — procgen determinism depends on never sharing generator state between systems.

### 2.3 RHI (`sol::rhi`)

A thin, Vulkan-only abstraction — its job is to make renderer code short and safe, not to hide Vulkan:

- Instance/device/queues bootstrap, validation layers in dev builds.
- Swapchain with resize/recreate handling.
- Resources: buffers, images, samplers with VMA-style sub-allocation (written in-repo: a simple block allocator over `vkAllocateMemory` first; grow sophistication as needed).
- Pipelines and shader modules (SPIR-V in), pipeline cache.
- Bindless-leaning descriptor management (large descriptor arrays + push constants) to keep material binding simple.
- Command recording per frame with N-buffered frame contexts, timeline-semaphore based sync.
- Deletion queue for safe resource destruction.

Later: a render-graph layer for pass scheduling/barriers once pass count justifies it (Phase 4+, not before).

**Shader toolchain**: GLSL → SPIR-V via glslang at build time (CMake custom commands); dev builds can also compile at runtime for shader hot-reload.

### 2.4 Renderer (`sol::renderer`)

Built on the RHI, consumes ECS render components:

- Forward+ (clustered forward) lighting — space scenes are mostly one dominant star light + point lights (engines, weapons); clustered forward keeps transparency (shields, glass, nebulae) simple. HDR pipeline with physically-plausible exposure + tonemap.
- PBR materials (metal/rough), normal mapping; emissive is a first-class citizen (engine glow, windows, weapon fire).
- Sky: procedural starfield + milky-way band rendered to a cubemap per system seed; distant planets/stars as impostors.
- Planet/star rendering: scaled-space impostors first (planets are scenery, not landable — see GDD non-goals); atmosphere shader later.
- Particles for thrusters, impacts, explosions (GPU-sim in a later phase; CPU first).
- Debug draw (lines, shapes, text) from day one — space is empty and dark; you cannot debug what you cannot see.
- Camera-relative transform flow: sim `DVec3` → subtract camera origin → `float` model matrices → GPU.

### 2.5 ECS (`sol::ecs`)

Custom ECS with **sparse-set storage** — chosen via the Phase 3 spike benchmark of our actual access patterns (thousands of ships/projectiles iterated linearly per sim tick, sparse component churn on projectiles); see `docs/decisions/001-ecs-storage-model.md` for the numbers and the archetype comparison.

- Entities are generational handles. Components are plain structs (POD-leaning, serializable).
- Systems are free functions scheduled explicitly (fixed order first; job-graph parallelism later, using the core job system).
- Queries/views for iteration; deferred structural changes via command buffers so systems can run in parallel safely.
- The ECS is the save-game backbone: world state serialization walks component storage.

### 2.6 Asset pipeline (`tools/cooker` + `sol::assets`)

- **Cooker** (separate executable, links `core` only): imports source assets → engine-native binary formats with stable asset ids + a pack manifest. Importers written in-repo: glTF (JSON + buffers) for meshes, PNG for textures (→ BCn-compressed at cook time; BCn encoder in-repo, quality over speed initially), WAV for audio (later), TOML/JSON data defs → validated binary tables. *(Phase 5 update: data defs load as strictly-validated TOML at runtime (`sol::assets::DefDatabase`) — validation happens at load either way and runtime parsing is what makes def hot-reload cheap; cooking defs to binary tables joins the pack/manifest work in Phase 7.)*
- **Runtime** (`sol::assets`): async loading via the job system, handle-based registry, reference counting, **hot-reload in dev builds** (file watcher → recook → swap) for textures, meshes, shaders, data defs, and Lua scripts. Fast iteration is the single biggest force multiplier for a solo/small team — treat hot-reload as a feature, not a luxury.

### 2.7 Simulation (`sol::sim`)

- **Fixed timestep** (start at 60 Hz sim tick), accumulator loop, render interpolation between sim states. Sim never reads wall clock.
- **Flight model**: Newtonian 6-DoF (force/torque integration) with layered flight assists (velocity damping, rotation damping, speed limiter) — "Newtonian under the hood, flyable by default," Elite-style. Assist-off mode is free.
- **Collision**: custom broadphase (uniform grid or sweep-and-prune per local bubble) + narrowphase (sphere, capsule, OBB, and convex-hull vs. ray for hit-scan). Ships are rigid bodies with simplified collision response — this is a space game: no stacking, no contact manifolds resting on floors. A full physics library is explicitly out of scope.
- **Simulation LOD (the living universe)**: the player's current system runs the full sim ("bubble"). Everything else runs a coarse agent simulation — ships as schedule entries moving between markets, battles resolved statistically — promoted to full fidelity when the player arrives. This is the EVE/X4 trick and it is an architectural commitment: every gameplay system (economy, AI, missions) must define both its full-fidelity and coarse representations.
- **Economy sim**: agent-based — stations produce/consume goods, NPC traders haul, prices from local supply/demand. Runs on the coarse layer galaxy-wide; ticks slower than the physics sim (e.g., 1 Hz coarse, minutes-scale for economy).

### 2.8 Scripting (`sol::scripting`)

- **Lua 5.4** vendored, exceptions-off friendly (compiled as C, longjmp error handling contained at the boundary).
- Hand-rolled binding layer (no sol2): a small template-based registration API for exposing C++ functions/types + a typed handle scheme for entities/assets crossing the boundary. Bindings are explicit and audited — the Lua↔C++ surface is the engine's public API and the mod API.
- **What lives in Lua**: mission logic, faction AI decision-making (strategy layer), economy tuning/rules, event scripting, UI flow, ship/weapon/faction stat definitions (via data files Lua can also generate).
- **What stays in C++**: per-tick hot loops (physics integration, collision, individual ship steering execution), rendering, ECS internals.
- Script hot-reload with state-preserving reload where feasible.
- **Mod loading**: mods are directories (data + scripts) layered over base content by load order. Same mechanism the base game uses — the base game is "mod zero."

### 2.9 UI

- **Dev/debug**: Dear ImGui from Phase 2 — perf HUD, entity inspector, sim controls, economy dashboards, console. Never ships as player-facing UI.
- **Game UI** (Phase 8d): custom immediate-mode framework over a batched screen-space quad renderer — HUD (targeting reticles, velocity indicators, shield/hull), menus, station screens; map screens (system + galaxy) ride along with the exploration item. Text comes from an in-repo TrueType rasterizer, baked to atlases by the cooker. **Revision**: screens are authored in C++ for v1; the original "driven from Lua for layout/flow" intent is deferred to a later modding pass, once the widget vocabulary has stopped moving (a Lua binding over a churning API is wasted work). C++ was always the drawing side; this only defers the layout/flow half.

### 2.10 Audio (later)

WASAPI backend behind a platform interface, mixing in-engine, WAV/ogg (stb_vorbis-class decoder written or approved later — flagged as a future dependency decision). Scheduled Phase 8; stubbed interface earlier so gameplay code can post sound events into a void.

---

## 3. Repository Layout

```
SolProject/
├── AGENTS.md
├── CMakeLists.txt
├── CMakePresets.json
├── docs/
│   ├── engine-plan.md        ← this file
│   ├── gdd.md
│   └── decisions/            ← numbered decision records
├── engine/
│   ├── platform/  (public headers + win32/)
│   ├── core/
│   ├── rhi/
│   ├── renderer/
│   ├── ecs/
│   ├── assets/
│   ├── sim/
│   ├── scripting/
│   ├── ui/
│   └── test/                 ← in-repo test harness + unit tests
├── game/
│   ├── src/                  ← C++ game glue
│   └── data/                 ← base content: TOML defs + Lua scripts ("mod zero")
├── tools/
│   └── cooker/
├── assets/                   ← source assets (glTF, PNG, ...)
├── shaders/                  ← GLSL source
└── third_party/              ← lua/, imgui/, (glslang via SDK or vendored)
```

Each engine module is a CMake static library target (`sol_core`, `sol_rhi`, ...) with layering enforced by target link dependencies — you physically cannot include upward if the include paths aren't linked.

---

## 4. Roadmap

Each phase ends with a runnable milestone and explicit exit criteria. Phases are sequential but late-phase design questions (marked ⚑) should be kept in mind early.

### Phase 0 — Scaffolding ✅ (completed 2026-08-15)
Repo layout above; root CMake + presets (dev/release, MSVC+Ninja); `.clang-format`, `.editorconfig`, `.gitignore`; minimal test harness + CTest wired; GitHub Actions CI (Windows: configure, build, test); empty `sol_platform` → `game` executable that prints and exits.
**Exit**: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` green in CI; a committed decision record template.

### Phase 1 — Platform + First Triangle ✅ (completed 2026-08-15)
Win32 window + message pump + keyboard/mouse input behind portable API; high-res timing; logging/assert; Vulkan bootstrap in RHI (instance, validation, device/queues, swapchain, one graphics pipeline); triangle on screen; clean resize + shutdown (zero validation errors).
**Exit**: colored triangle, resizable window, validation-clean, ESC quits.

### Phase 2 — Renderer Foundations ✅ (completed 2026-08-15)
Math library complete + fully unit-tested; cooker v0 (glTF mesh + PNG→BC1 texture → individual cooked files; pack files + async loading deferred to the Phase 4–6 asset work, when new content first demands them); runtime asset loading (sync); depth buffer (reversed-Z), perspective camera, push-constant transforms; textured mesh rendering; Dear ImGui integrated (perf overlay, log console); free-fly camera; shader build pipeline in CMake + dev hot-reload (mtime watch → glslc → pipeline reload, F5 to force).
**Exit**: fly around a textured scene at high framerate with ImGui stats; math test suite green. *(Verified: 145+ fps cube scene with overlay/console, live shader hot-reload demonstrated, all suites green, validation clean.)*

### Phase 3 — Core Systems ✅ (completed 2026-08-15)
ECS storage spike (benchmark archetype vs. sparse-set on representative workloads; record decision) then implementation: handles, queries, command buffers; job system + parallel-for; event bus; fixed-timestep loop with render interpolation; binary serialization with versioning; TOML parser; PRNG streams.
**Exit**: 10k+ moving entities simulated at 60 Hz sim / uncapped render with interpolation; world state round-trips through save/load; all core tests green. *(Verified: sparse-set ECS chosen via spike — see `docs/decisions/001-ecs-storage-model.md`; 10,001-entity orbital swarm at ~400 fps release / 60 Hz sim with interpolation, validation clean; world save/load round-trips live via F9/F10 and byte-identically in tests; all suites green. Deferred within scope: pack files + async asset loading ride along with the Phase 4-6 asset work; system-level job-graph parallelism stays "fixed order first" per 2.5.)*

### Phase 4 — Space Flight Vertical Slice ★ ✅ (completed 2026-08-15)
The proof-of-concept milestone. `DVec3` sim space + camera-relative rendering proven at solar-system distances; 6-DoF Newtonian flight + assists, ship controllable in first/third person; starfield cubemap, one planet impostor, one station mesh, sun light + HDR; thruster particles (CPU); ImGui-based provisional HUD (velocity, orientation, target); debug draw.
**Exit**: fly from a station to a planet 100,000+ km away with no precision artifacts; flight feels controllable; this build is the template for all future content. *(Verified: scripted cruise flight from Aster Gateway (0.67 AU from origin) 127,000+ km to the planet at 5,500 km/s, parked 23,000 km above the surface — consecutive-frame pixel diff at rest showed zero jitter at 1e11 m coordinates; assist/boost/cruise flight model unit-tested (rate convergence, damping, caps, cruise-exit braking) and flown under scripted input; HDR + ACES with sun impostor, per-seed starfield cubemap, analytic planet impostor with per-pixel depth; cockpit/chase/free cameras; thruster particles mirror the applied flight acceleration; HUD target marker + readout strip; F3 debug draw; F5 live shader reload of all 12 shaders, F9/F10 save/load on the new component schema; all suites green, validation clean. Cruise is provisional pending the GDD travel-model decision (Q per §5.4). No collision yet — you can fly through the planet; Phase 6 brings collision.)*

### Phase 5 — Lua + Data-Driven Content ✅ (completed 2026-08-15)
Lua VM embedded; binding layer + entity/asset handle scheme; ships, weapons (defs only), factions defined in TOML data loaded through the cooker/registry; script + data hot-reload; in-game Lua console; mod directory layering (base game = mod zero).
**Exit**: change a ship's stats or a Lua behavior and see it live without restarting; spawn ships from data defs via console. *(Verified: Lua 5.4.8 vendored, sandboxed VM (no io/os/package/debug) with every entry behind a protected call + traceback; hand-rolled binding layer (Stack traits, strict integers/strings, context-pointer functions) and metatable-typed entity handles, both unit-tested; strict-schema DefDatabase parses ship/weapon/faction TOML — unknown/mistyped keys are load errors and a failed merge keeps the previous state; layers merge in order `game/data` (mod zero) then `game/mods/<name>` sorted, later ids replacing wholesale in place. Live checks in the running build: `sol.spawn_ship("sol.freighter")` typed into the F1 console spawned a def-driven ship (entities 10→11); editing ships.toml scale 1.0→2.5 hot-reloaded and visibly resized the player ship; editing init.lua re-ran boot scripts and the on_tick announcer picked up new interval + text; a testmod layer overrode the shuttle at boot (scale 5 visible) and deleting the mod dir live reverted it. All 6 suites green, validation clean. Deviation from the original line "loaded through the cooker": defs load as validated TOML at runtime — the same validation the cooker would run, and required anyway for hot-reload; cooking defs to binary tables is deferred to the Phase 7 pack/manifest work when a shipping layout exists. Def-spawned ships are visual-only (no FlightBody) until Phase 6 gives NPCs pilots.)*

### Phase 6 — Combat & AI ✅ (completed 2026-08-15)
Weapons (projectile + hitscan), damage model (shields → hull, per GDD), collision integrated with flight; NPC pilots: C++ steering behaviors (pursue, evade, formation, collision avoidance) driven by Lua state machines (roles: fighter, trader, patrol); target selection + basic threat logic; combat feedback (hit effects, explosions, HUD target info).
**Exit**: a dogfight against 5+ NPCs that is fair, legible, and fun enough to replay voluntarily. *(Verified: swept-sphere collision (no tunneling at cruise's 91 km/tick; station ram parks at the hull), Elite-pips power model (decisions/003; ENG pips measurably move the assist cap 214.5→253.0 m/s), directional fore/aft shields → ablative armor → hull (decisions/002; k·v² impact damage, ram death → respawn), def-driven weapons (bolt entities swept against ship spheres, hitscan pulses, WEP-capacitor draw), NPC pilots (sim steering behaviors closed-loop unit-tested through the flight model; Lua pilot_think at 2 Hz picks states through the sol.pilot_* API; fighter/trader/patrol roles with per-state pip policies), combat feedback (impact sparks, destruction fireballs, T-cycles-ships targeting with [S/H] readout, projectile lead marker, damage flash). Scripted 5-fighter dogfight: 34-43 entities at ~146 fps, fully legible; a weaving player survived ~24 s vs 5 fighters where a static player dies in ~10 s to one — maneuvering defeats intercept prediction, which is the fairness mechanism. All 7 suites green (43 sim tests), validation clean. Caveats: "fun enough to replay voluntarily" needs the user to actually fly it (same caveat Phase 4 carried for flight feel) — tuning levers are all in game/data; armor is one ablative pool until hit zones exist (locational per GDD needs model-based collision); trader/patrol formation flying is unit-tested but only lightly used; NPC-vs-NPC target selection beyond friendly fire waits for factions.)*

### Phase 7 — Universe & Economy ✅ (completed 2026-08-15)
Procedural galaxy generation (seeded; systems, connections, faction territory claims, station placement per handcrafted rules); jump-gate travel + in-system cruise (per GDD travel model); docking (request, autodock or manual per GDD); trading UI (provisional ImGui); agent-based economy on the coarse sim layer; sim-LOD promotion/demotion when the player changes systems; full save/load including galaxy + economy state.
**Exit**: buy low in one system, jump two systems over, sell high — against prices that moved because of simulated NPC traders, not scripts; save/reload mid-run is lossless; same seed ⇒ same galaxy. *(Verified: `sol::sim` galaxy generator (Prim MST + local extra lanes ⇒ always-connected gate graph; core/frontier/fringe by radius; multi-source Dijkstra claims faction territory from spread capitals; per-system content clusters stations/gates around a primary planet inside the decisions/005 cruise-leg budget) — deterministic per seed with elementwise-equality tests, 80 systems / 158 lanes at seed 1701 vs 157 lanes and different names at seed 999. Game instantiates exactly one system at a time (the sim-LOD bubble): gate transit (J in activation range, `sol.jump`/`sol.jump_to` from the console) despawns the system and promotes the destination, arriving at the mirror gate. Docking: G-toggle autodock parks at the pad, save carries docked + last-dock, and death now wakes the player at the last dock (GDD death rule; insurance waits for Phase 8 outfitting). Economy: every station galaxy-wide runs a per-commodity market (price linear in stock/capacity), archetype rates come from strict-schema `[[station]]`/`[[commodity]]` defs, and 40 trader agents haul the best profit-per-second route (deadheading to gluts when stranded) at 1× real time — unit-tested for determinism, hauling against a handcrafted 2-system galaxy, and save/load that tracks the original tick-for-tick. Exit run verified live: after 180 s of divergence, bought 40 machinery at a glut, sold 4 hops away at 31.25 for 1454.50 credits from a 1000 start (+45%), F9 mid-run, full process restart, F10 restored system/dock/credits/market stocks exactly (camera coords identical to the last decimal). Caveats: trading UI is dev-ImGui per plan (real game UI Phase 8); coarse traders are not yet promoted to visible ships in the player's system; docking is autodock-only; remote prices are not visible in-game (scouting is manual); def cooking to binary pack tables — deferred from Phase 5 to "the Phase 7 pack work" — is still deferred, now to Phase 8 performance hardening, since no shipping layout exists yet; and the user has still never personally flown the game.)*

### Phase 8+ — Sandbox Depth (sketch, spec before starting)
Faction simulation (relations, patrols/raids) and player reputation (→ completed as Phase 8b below; territory/border shifts deliberately deferred to a later war item); mission/contract system (Lua-authored; must host the authored campaign spine per `decisions/008`; → completed as Phase 8c below); ship outfitting + progression + purchasing (→ spec'd as Phase 8a below); custom game UI replacing provisional ImGui screens (→ spec'd as Phase 8d below); audio system; mining/salvage loops; exploration/scanning; performance hardening (incl. def cooking to binary packs, deferred from Phases 5/7); Linux platform backend when it earns priority. ⚑ Each item gets its own spec added to this document before implementation.

#### Phase 8a — Outfitting, Progression & Purchasing ✅ (completed 2026-08-15)

Implements GDD §8 Outfitting **[core]** plus the two decisions that live here: the trivial crew system (`decisions/006`) and the death-penalty insurance/hardcore rules (`decisions/007`).

**Data model (assets layer).** New strict-schema def types in `game/data`:
- `[[module]]` — `id`, `name`, `slot` ∈ {`shield`, `engine`, `cargo`, `utility`}, `price`, `mass` (kg), `power_draw`, and stat modifiers.
- `[[crew]]` — `id`, `name`, `role`, `price` (one-time hire fee), and stat modifiers (no mass/power — crew occupy berths, not slots).
- Modifier vocabulary shared by modules and crew: `<stat>_add` and `<stat>_mul` keys over a fixed stat list (`forward_accel`, `reverse_accel`, `lateral_accel`, `vertical_accel`, `max_speed`, `turn_rate` (uniform over all three axes), `cruise_speed_scale`, `shield_strength`, `shield_regen`, `armor`, `hull`, `weapon_capacitor`, `weapon_recharge`, `cargo`). Unknown keys are schema errors, per the Phase 5 defs contract.
- `[[ship]]` gains `price`, `mass`, `power_output`, per-type slot counts (`slots_shield`, `slots_engine`, `slots_cargo`, `slots_utility`), and `crew_berths`. `[[weapon]]` gains `price`.

**Loadout resolution (assets layer, `sol::assets` `loadout.hpp`).** Pure function: base `ShipDef` + fitted modules + crew → effective `ShipDef`. Order-independent math: all `_add` modifiers sum onto the base, then all `_mul` multiply, then the mass penalty scales linear/angular accelerations by `mass / (mass + Σ module mass)`. Validation rejects a fit that overflows any slot count, the crew berths, or the power budget (`Σ power_draw ≤ power_output`). Lives in assets (below sim) so the game applies effective defs through the existing `applyShipDef` path unchanged; unit-tested like all def machinery (parsing, ordering, budgets, determinism).

**Weapons.** The single weapon mount becomes part of the fit: the fit stores a weapon id (initialized from the ship def), stations sell weapons, and swapping is outfitting like any module. Multiple hardpoints are **out of scope** until combat supports more than one `ShipWeapon`.

**Fleet & purchasing (game layer).** The player owns a fleet: each owned ship records its def id, weapon, fitted modules, hired crew, and where it is (active, or stored at a specific station). Buy ships and modules while docked; sell-back at a flat resale fraction. Switching ships happens only while docked at the station where the target ship is stored; the current ship stays stored there. Cargo remains player-global; a switch that would strand cargo over the new ship's capacity is refused. v1 availability: every station sells every ship/module/weapon/crew def (region- and faction-gated catalogs arrive with the faction item).

**Death (decisions/007).** Default: respawn at last dock in the same ship + fit + crew, cargo lost, deductible = fixed fraction of (hull price + fitted module and weapon prices) charged, clamped at zero credits. Hardcore: new-game flag (`--hardcore`), stored in the save header; death deletes the save and starts a fresh run. Deductible fraction and resale fraction are game-side constants for now (tuning levers; move to data when a settings def exists).

**UI & console.** The docked ImGui screen grows Shipyard / Outfitting / Crew tabs beside Trade (still the provisional dev UI — the custom game UI is its own Phase 8 item). Console API: `sol.modules`, `sol.crew_defs`, `sol.fit`, `sol.fleet`, `sol.buy_module`, `sol.sell_module`, `sol.buy_weapon`, `sol.buy_ship`, `sol.sell_ship`, `sol.select_ship` (fleet indices 1-based), `sol.hire_crew`, `sol.fire_crew`, `sol.insurance_quote`.

**Save v4.** Header gains the hardcore flag; body gains the fleet (active index; per ship: def id, weapon id, module ids, crew ids, stored system/station). On load the active fit re-resolves and re-applies, so def edits between sessions land coherently (same rule as hot-reload).

**Exit**: take trade profits to a station and buy + refit a second ship; switch between stored ships and feel the stat difference in flight; die once — wake at the last dock in the same fit with cargo gone and the deductible charged; a hardcore run's save is gone after death; save/reload preserves fleet, fits, and crew exactly; fit math fully unit-tested. *(Verified live 2026-08-15, seed 1701: cargo pod fitted at Lyrioa Alpha raised capacity 50→75 on the station-panel header and the insurance quote read exactly 490 cr = 5% × (8000 hull + 600 pod + 1200 cannon); death to a 4-fighter pack charged min(deductible, credits) (299 cr → 0, clamped), dropped 10 food, and woke the player docked in the same fit; a bought freighter (−60,000) refit with a Mk2 pod + engineer + heavy cannon left credits at 34,000 — exact to the ledger including the 300 cr mining-laser resale — showed cargo 10/260, and crawled at 47.5 m/s where the shuttle leaps; F9/F10 in-session and a cold process restart both restored fleet/fit/crew (fit-derived cargo capacity recomputed on load without touching the exact ECS snapshot); a --hardcore death logged "run over", deleted world.sav from disk, and restarted a fresh run (1000 cr, bare shuttle) at the new-game system. 5 new assets tests (61 total), all 6 suites green, zero warnings. Design consequence discovered live: respawning at the last dock under fire was a spawn-kill loop (fatal under hardcore), so a docked ship now takes no damage — dock = safe room. Caveats: every station sells every def (region/faction-gated catalogs arrive with the faction item); deductible (5%) and resale (50%) fractions are constants in space_world.hpp, not data; one weapon mount until combat supports multiple hardpoints; the station screen is still dev-ImGui; `sol.add_credits` dev cheat added for outfitting tests.)*

#### Phase 8b — Faction Simulation & Reputation ✅ (completed 2026-08-15)

Implements GDD §7 **[likely]**. Scope decided with the user: **dynamic relations on fixed borders** — the faction-pair standing matrix lives and moves from simulated events, but territory never changes hands (border shifts join a later war/conquest item); v1 reputation consequences are **NPC hostility, docking rights, and catalog access** (price modifiers deferred); the roster grows to **4–6 authored majors plus generated pirate clans**.

**Data model (assets layer).** `[[faction]]` grows: `kind` ∈ {`major`, `pirate`}; personality weights `aggression` and `forgiveness` (0..1, consumed by the Lua decision rules); `relations` (inline table of faction id → initial standing, −100..100; unlisted pairs default to 0; the matrix is symmetric, and a pair declared from both sides must agree or it is a schema error); and spawn rosters `ships_patrol` / `ships_raider` (ship def ids). Majors claim territory exactly as today (generator capitals); `pirate` factions claim no systems. Pirate clans are partly generated per GDD §7: the galaxy generator instantiates clans in the lawless fringe from the `pirate` defs as templates (seeded name/color/personality jitter, one clan per fringe neighborhood, deterministic per seed), and lawless-system stations become clan property. The runtime faction table = authored majors + generated clans; everything downstream (relations, reputation, spawns, saves) is sized to that table and re-derived from galaxy + defs on load, the same rule the economy uses.

**Faction sim (`sol::sim::FactionSim`, coarse layer).** A sibling of `Economy`: the symmetric relations matrix (−100..100) with **war** below −50 and peace restored only above −25 (hysteresis), ticking at a slow cadence (order of once per sim minute). Each tick C++ enumerates candidate actions per faction in index order (raid targets: systems of at-war or deeply hostile factions within reach of its territory or fringe base) and calls Lua `faction_think(faction, personality, relations, candidates)` — a pure decision rule weighing aggression/forgiveness (Stellaris-flavored, per plan §2.8) — and C++ executes the choices: a **raid** removes a slice of the victim system's station stock and any coarse trader cargo in transit there (through a small raid entry point on `Economy`, so piracy → shortages → prices per GDD §6), pushes the pair's relations down, and bumps a per-system **raid intensity** that decays over minutes. Relations otherwise drift toward each pair's declared baseline at a forgiveness-scaled rate. Deterministic for (galaxy, defs, seed): Lua sees no entropy beyond its arguments; unit-tested like the economy (fixed galaxy, threshold hysteresis, tick-for-tick save/load).

**Player reputation (game layer).** Per-faction standing, −100..100; a new game starts at 0 with majors and −20 with pirate clans. Sources in v1: **kills** — destroying a faction's ship costs standing with the owner and pays standing with factions at war with (or deeply hostile to) the owner, scaled by relation depth (the GDD web: helping one side is taken personally by their enemies) — plus a small **commerce trickle** for trading at a faction's stations. Consequences at thresholds: below −30 a faction is **hostile** — its ships attack the player on sight and its stations refuse docking requests. Edge cases: death respawn ignores docking rights (dock = safe room stands; undocking is one-way until standing recovers), and a ship stored at a now-hostile station is stranded until standing recovers. **Catalog access** fixes the Phase 8a caveat: a station's Trade/Outfitting/Shipyard/Crew catalogs are the owner faction's — ship/module/weapon/crew defs gain optional `factions` (allowlist of sellers; unset = everyone) and optional `min_rep` (standing gate with the owner); pirate-clan stations fence everything their defs allow but still require non-hostile clan standing to dock.

**In-bubble presence (game layer).** `loadSystem` now populates ambient NPCs: owner-faction patrol wings near stations and gates (count by region security — core heavy, frontier light, fringe none) on the existing patrol role, and raider wings from resident clans scaled by current raid intensity on the existing fighter role. `ShipPilot` gains a faction index; targeting honors the matrix plus player standing — patrols engage raiders and hostile players, raiders engage patrols and any player not friendly with their clan — replacing today's unconditionally player-hostile fighters. Lua `pilot_think` receives the faction/attitude context; console spawns keep working with an explicit faction argument. Promoting coarse economy traders to visible bubble ships stays **out of scope** (unchanged Phase 7 caveat).

**UI & console.** The docked ImGui screen gains a **Factions** tab (per-faction standing bar, war/peace lines, recent raids); the HUD target readout shows the target's faction and attitude. Console API: `sol.factions`, `sol.relations`, `sol.rep`, `sol.raids`, `sol.set_rep` (dev cheat); `sol.spawn_pilot` grows a faction argument.

**Save v5.** Body gains the faction block: relations matrix, war flags, per-system raid intensity, player standings, and the sim's cadence accumulator. Generated clan identities are re-derived from the seed, not saved; on load the block is validated against galaxy + defs (economy rule), and the version bump follows the same compatibility policy as v4.

**Exit**: two majors slide into war from raids with no player involvement, visible in the Factions tab; killing a patrol moves standings across the web (owner down, owner's enemies up); at hostile standing that faction's patrols attack on sight and its stations refuse docking; a `min_rep`-gated item is refused, then sold once standing recovers (dev cheat allowed for pacing); a fringe raid visibly dents a station's stock and prices; save/reload restores relations, war flags, raid intensity, and standings exactly; FactionSim is deterministic per seed and unit-tested. *(Verified live 2026-08-15, seed 1701: boot builds 15 factions — 5 authored majors + 10 generated clans, one per lawless fringe neighborhood, with seeded name/color/personality jitter ("Norea Reavers", "Kayx Syndicate", "Cara Cartel"…) — and an ambient Solar Navy patrol wing flies the start system. The Lua `faction_think` rule raided from minute one (console: "[factions] Norqros Raiders raids Beyryx (relation -60)…"), and after 8 unattended minutes the authored -35 Navy/Hegemony rivalry sat at **-50.9 (WAR)** and Hegemony/Compact at -83.8 (WAR), with a galaxy-wide raid ledger (`sol.raids`) naming majors and clans both. Killing a Solar Navy patrol freighter moved the whole web exactly: Navy 0 → -10.0, Hegemony +1.8 (= 10 × 0.5 × 0.35), each clan up by 10 × 0.5 × depth of its own Navy relation (-15.3…-17.0 from a -20 start — clans whose raids had deepened the war gained more). At -40 with the Navy its interceptors attacked on sight (killed the player twice; the death respawn docked at a hostile station per the dock-safe-room rule, deductible charged exactly) and "docking denied at 'Lyrioa Alpha': Solar Navy is hostile" refused re-entry. Catalog gates: Shield Booster Mk2 (min_rep 10) refused at rep 0 and sold at rep 20; Heavy Cannon (navy/hegemony/pirate sellers, min_rep 15) mounted at a Navy station. A Hegemony raid on Lyrioa cut station stocks 25% on-screen (machinery 1000 → 750) with prices reacting. F9/F10 restored standings exactly (`sol.rep()` string-equal across the reload); tick-for-tick trajectory equality is unit-tested. HUD target line shows "Solar Navy [neutral/hostile]" color-coded; the docked screen's Factions tab shows standing bars, war lists, and recent raids. 10 new tests (sim suite 65, assets 13), all 6 suites green, zero warnings. Implementation notes: player standings live in `sol::sim::FactionSim` beside the relations matrix (unit-testable, saved with it) with consequences applied game-side; `sol.spawn_pilot` kept its 2-arg form (unaffiliated, pre-8b behavior) and `sol.spawn_pilot_faction` adds allegiance; dev QoL `sol.target_ship(name)` and a "[local]" owner marker in `sol.rep()` were added for scripted verification. Design rules discovered live: a patrol drops a grudge against the player once standing recovers above hostile (permanent aggro besieged the pad; raiders keep theirs), and the Lua raid policy sends majors at rival majors when one is in reach — under pure worst-relation targeting the -60 clans soaked every raid and major wars never ignited. Caveats: raider-incursion and clan-home wings share the verified ambient-spawn path but weren't visited live; coarse traders still aren't promoted to bubble ships (Phase 7 caveat stands); def hot-reload does not rebuild the faction table mid-run (faction identity edits need a restart); border/territory shifts remain deferred to a later war item.)*

#### Phase 8c — Missions & Contracts ✅ (completed 2026-08-16)

Implements GDD §8 Missions/contracts **[likely]** and hosts the campaign spine (`decisions/008`: authored, stateful, multi-step questlines are first-class citizens, not only generators). Scope decided with the user: v1 generates **haul contracts** from real Economy shortages and **bounty contracts** from FactionSim raid intensity (GDD §8's two examples, both driven by live sim state), plus an authored **opening act of 3–4 spine missions** that proves the questline machinery; the full campaign is later content work. Courier runs, mission-item cargo, and mission-driven unlocks (the jump drive stays future) are out of scope.

**Mission model (`sol::sim::MissionSim`, coarse layer).** A sibling of `Economy`/`FactionSim` so the machinery is unit-testable and saves like the rest. A mission is typed data: poster faction, reward credits, standing reward, standing penalty (abandon/expiry), optional deadline (sim seconds), campaign flag + authored id, and an ordered list of typed objectives — `Dock{system, station}`, `Deliver{system, station, commodity, units}` (hand-in at dock), `Kill{faction, count, system-or-anywhere}`, `FlyTo{system, position, radius}`. The sim advances the current objective on events the game reports (docked, delivered, ship killed, position sampled), ticks deadlines on sim time, and surfaces completions/failures as results the game consumes. Candidate enumeration lives here too: **haul candidates** are markets within a jump budget whose stock of some commodity sits below a shortage fraction of capacity (severity = the gap); **bounty candidates** are systems whose raid intensity exceeds a threshold, paired with the raiding clan (`FactionSim::lastRaider`). Deterministic for (galaxy, params, seed, event sequence); unit-tested like the economy (progression, candidates, deadlines, tick-for-tick save/load).

**The board (offers, faction_think pattern).** On docking — and on a refresh interval while docked — C++ enumerates candidates plus one seeded roll and calls Lua `mission_board(station, ownerFaction, candidates, roll)`; the hook composes concrete offers via `sol.post_mission{...}`, validated in C++ against the candidates (the roll is the only entropy, so board policy stays a pure function; scriptless fallback posts nothing). Offers price from severity and distance; offers may carry `min_rep` with the poster (GDD §7 mission tiers) and are hidden below it. A haul hand-in moves the delivered units into the destination market's stock through a new `Economy` delivery entry point — the contract literally fills the shortage it advertised, and prices react. Bounties count kills of the named clan in the named system and pay on the final kill (no return leg in v1).

**Campaign spine (authored, Lua).** Content-pipeline extension: a layer's boot scripts grow from `scripts/init.lua` to every `scripts/*.lua` sorted (init first), all hot-reloaded, so campaign content gets its own file instead of bloating init. The opening act is 3–4 authored missions mixing objective kinds, chained by a saved **campaign stage**; the stage's current mission is posted on the appropriate faction's boards, is declinable, and is re-offered on the next dock (the `decisions/008` guardrail: the spine is ignorable and the sandbox is complete without it — abandoning costs no standing on campaign missions and nothing is gated behind them). Objective transitions call Lua `mission_event(mission, step, "start"|"complete"|"failed")` for authored flavor — dialog prints, ambush spawns via the existing spawn API — so authored missions are stateful without per-mission C++.

**Consequences & rules.** Active missions cap at 4. Completion pays credits plus standing with the poster (`FactionSim` gains a clamped `addStanding`); abandoning or expiring a procedural contract charges its standing penalty. Deadlines apply to haul contracts only in v1. Death leaves missions active (losing your cargo dooms a haul naturally, no extra rule).

**UI & console.** The docked ImGui screen gains a **Missions** tab (board with accept buttons; journal with objective text, progress, deadline); the HUD gains a tracked-mission objective line under the target readout. Console: `sol.missions`, `sol.mission_board`, `sol.accept_mission`, `sol.abandon_mission`, `sol.mission_candidates`, `sol.post_mission` (board-hook API), `sol.campaign_stage` / `sol.set_campaign_stage` (dev cheat for pacing verification).

**Save v6.** Body gains the mission block: active missions with per-objective progress and remaining deadlines, the current board (offers + refresh accumulator), and the campaign stage. Validated against galaxy + defs on load (economy rule); version bump follows the v4/v5 compatibility policy.

**Exit**: a station near a genuine simulated shortage offers a haul contract naming that commodity and destination; delivering visibly refills the market stock (prices react) and pays credits + standing; a raid-hot system generates a bounty on the resident clan that progresses on player kills there and pays on the last one; abandoning or timing out a contract costs standing with the poster; the authored opening act plays end-to-end — multi-step objectives, scripted flavor firing on transitions, campaign stage advancing — and declining it leaves the sandbox board fully functional with the spine re-offered later; save/reload mid-mission restores journal, board, progress counts, deadlines, and campaign stage exactly; MissionSim is deterministic per seed and unit-tested. *(Verified live 2026-08-16, seed 1701: docking opens the Missions tab with the stage-0 campaign offer plus contracts composed from live sim state — after Ironstar Hegemony organically raided Lyrum (dev-forced raids deepened the shortage), `sol.mission_candidates` read "haul: Lyrum Alpha @ Lyrum needs 279 sol.ore (78%, 1 jumps)" / "bounty: Lyrum raided by Ironstar Hegemony (0.96, 1 jumps)" and the board offered exactly those, deadline-priced by init.lua's board policy. A "55 food to Lyrum Alpha (1117 cr, 900s)" haul: partial hand-in logged "handed in 54 units" at the dock, the destination's food stock visibly refilled ~0 → 50 with the price easing, and topping up completed it for +1117 cr exactly and Solar Navy +3 (read back +3.9 with the commerce trickle). The "Destroy 4 Ironstar Hegemony raiders in Lyrum (4100 cr)" bounty ignored wrong-faction and wrong-system kills, counted only player kills in Lyrum, and paid +4100 exactly on the fourth. The tier gate held: "Haul: 105 ore … rep 10+" was refused at rep 0 (journal unchanged; the UI disables un-acceptable offers); abandon charged the poster's penalty on the next tick (set_rep 10 → accept → abandon → 8.0), with expiry sharing the unit-tested Failed path. Campaign Act 1 played end-to-end: Shakedown Cruise (+400, stage 1), Teeth in the Dark's scripted ambush ("Norea Reavers ships on the scanner!", 2 spawns on the fly-to completing; +900, Navy +4, stage 2), Relief Run's named-destination delivery completed across multiple partial hand-ins (+1400, +5, stage 3), and Line in the Dark's 3-kill picket (+2500, +8, "ACT 1 COMPLETE", stage 4) — with the campaign offer re-posted on every dock it was declined at across two separate runs, the sandbox board fully functional around it, and a mid-journal player death leaving missions intact. F9/F10 in-session restored journal|board|stage string-equal (deadline seconds normalized — they keep ticking); a full process restart + F10 restored stage 4, the docked station, the journal, and the board line-for-line at identical rewards (no re-roll on a docked load) with the restored deadline continuing on the HUD (14:53 → 14:45). 5 new sim tests (70 total), all 6 suites green, zero warnings. Live pacing used dev levers, disclosed: a temporary mod weakening target ships for scripted kills (deleted after) and a new `sol.warp` station-offset teleport cheat; the mission machinery itself ran unmodified. Findings: mod layers created after boot are not discovered until restart (the layer list is built at initialize) — new caveat; mission consequences (payout/penalty) land one sim tick after a console-driven action (event queue drains in the world tick) — cosmetic; the hour-old economy runs ore/food dry near the core, which mission sourcing feels — tuning lever noted; campaign objective text must name its resolved destination (fixed in campaign.lua during verification). Caveats: expiry penalty live-verified only via the shared abandon path; FlyTo/kill legs were dev-paced rather than hand-flown (the standing "user has never personally flown the game" note stands); the station screen remains dev-ImGui; reputation price modifiers stay deferred per 8b.)*

#### Phase 8d — Custom Game UI (spec'd 2026-08-16, not started)

Implements §2.9's Game UI line. Replaces every player-facing Dear ImGui surface — the provisional flight HUD (Phase 4) and the docked station screen (Phases 7/8a/8b/8c) — with an in-repo UI stack, and adds the menu shell the game has never had. Dear ImGui stays exactly where the dependency policy puts it (`AGENTS.md` §5): dev overlay, perf stats, and the F1 Lua console, none of which ship as game UI.

Scope decided with the user: **v1 covers HUD + station screen + menu shell**; system/galaxy **map screens are deferred to the exploration/scanning item**, which has to build them anyway and knows what they must show. Screens are **authored in C++**, with §2.9's Lua layout/flow deferred to a later modding pass (§2.9 revised in the same change). Text comes from an **in-repo TrueType rasterizer** — no new dependency, consistent with the from-scratch rule that already produced the TOML, JSON, PNG/inflate, and glTF readers.

**Text stack (`sol::assets` + cooker).** A from-scratch TrueType reader and rasterizer, `sol::assets::TrueTypeFont`: `cmap` (format 4, plus format 12 when the charset needs it), `glyf`/`loca` quadratic outlines including composite glyphs, `head`/`hhea`/`hmtx` metrics, and `kern` if the shipped font carries one. CFF/PostScript outlines are **out of scope** — the shipped font must have `glyf` outlines. Rasterization is a scanline converter with analytic coverage anti-aliasing to an 8-bit alpha bitmap; it is a pure function of (font bytes, pixel size, codepoint), which is what makes it testable and its cooked output byte-stable. Lives in `assets` (above `renderer`, below `ui`) so both the cooker and the UI layer can reach it, matching how `png.cpp`/`gltf.cpp` already drive `core`/`assets` primitives from the tool side.

**Cooked font asset.** The cooker grows `.ttf` → `.sfont` beside its `.png` → `.stex` and `.gltf` → `.smesh` cases: it bakes a **glyph atlas** (single-channel coverage; `formats.hpp` gains `TextureFormat::R8` — uncompressed, since BC1 is opaque-RGB and would smear glyph edges) plus per-glyph metrics (atlas rect, bearing, advance), line metrics, and a codepoint→glyph table. **Discrete pixel sizes, not SDF**: the manifest beside the font names the sizes to bake (HUD/body/heading, e.g. 16/24/40 px), which keeps small text genuinely crisp — SDF's win is arbitrary scaling, which a fixed set of UI scales does not need. Charset is ASCII 32–126 plus the handful of symbols the game's strings actually use; unmapped codepoints resolve to a visible `.notdef` box rather than silently vanishing. Recooking is byte-identical for the same inputs (cooker suite test).

**UI renderer (`sol::renderer::UiRenderer`).** One pass drawing a batched stream of colored, textured, clipped quads in screen space: pixel-coordinate orthographic projection, alpha blending, no depth, per-batch texture (font atlas, UI texture, or a 1×1 white pixel for solid fills) and scissor rect. Per-frame dynamic vertex/index buffers, one per frame in flight, sized generously and asserted on overflow — the `ParticleRenderer` pattern (`particle_renderer.hpp`), including `reloadPipeline()` for shader hot-reload. New `shaders/ui.vert`/`ui.frag` in the existing `shaders/CMakeLists.txt` list. Draws inside the scene pass after tonemap and before `DevUi`, so the dev overlay always sits on top of game UI.

**UI framework (`sol::ui`).** Immediate-mode with retained hover/active state — §2.9's "retained-or-immediate hybrid", and it preserves the per-frame fill-then-execute seam the game already uses (`StationPanel` in, `StationAction` out), so no gameplay logic moves in this phase.
- `DrawList` — primitives: `rect`, `roundedRect`, `line`, `text`, `texturedQuad`, `pushClip`/`popClip`. Emits the vertex stream `UiRenderer` consumes; nothing in it touches Vulkan.
- `Font` — cooked atlas + metrics with `measure()` and codepoint iteration, so layout can size text before drawing it.
- `UiContext` — per-frame input snapshot (mouse, keyboard, text), widget id stack, hover/active/focus tracking, and a `Theme` (colors, spacing, radii, font sizes) that is data, not scattered constants.
- Widgets v1: label, button, tabs, table/list rows, scroll region, meter/progress bar, slider, checkbox, text input, tooltip, modal. Layout is a stack/flow helper (row/column, padding, alignment, fixed/flex sizing) — deliberately **not** a full flexbox implementation.
- **Keyboard-operable is a requirement, not a nicety**: every screen navigable and actionable without the mouse, which is also the groundwork for gamepad later.
- Headless-testable by construction: layout math, text measurement, clipping, id stacking, and `DrawList` output are all pure data. New **`ui.unit` suite** (first UI tests in the repo; suite count 6 → 7).

**Screens (game layer).** HUD at parity with today's `FlightHud` (`dev_ui.hpp`), redrawn properly: reticle + lead marker, speed/throttle, fore/aft shield arcs + hull, power pips (`decisions/003`), target readout with faction and attitude, tracked-mission objective + deadline, system name with jump/dock prompts, damage flash. Station screen keeps its six tabs (Trade, Outfitting, Shipyard, Crew, Factions, Missions) and its existing fill/execute plumbing in `game/src/station_ui.*`. **New menu shell**: main menu (New Game / Continue / Load / Settings / Quit), Esc pause in flight, settings (resolution, vsync, mouse sensitivity/invert, UI scale) persisted to a small TOML beside the save, and a save/load browser. New Game becomes explicit rather than implicit-at-boot, and `--hardcore` becomes a checkbox on it instead of a command-line flag (the flag stays for scripted runs). `main.cpp` gains an explicit game state (`Menu`/`Flying`/`Docked`/`Paused`) in place of today's implicit docked/flying booleans — the shell is what forces that cleanup.

**Dev UI relationship.** `DevUi` keeps the overlay and console unchanged. Once parity is verified live, the provisional HUD/station code is **deleted** from `dev_ui.cpp` rather than left to rot; `OverlayStats` stays on the dev side, and `FlightHud`/`StationPanel`/the row structs move into the real `sol::ui` headers.

**No save bump.** UI carries no sim state; settings live in their own file so saves stay clean. Save v6 is unchanged by this phase.

**Exit**: fly a combat engagement with no ImGui in the flight view — reticle, lead marker, shields/hull, pips, target faction/attitude, and the tracked-mission line all correct under fire; dock and run a full loop on the new station screen (buy cargo, fit a module, hire crew, accept a mission, read the Factions tab), mouse **and** keyboard-only; boot to a main menu, start a new game, pause mid-flight, change a setting that visibly applies, save and load from the menu, and quit — without touching the console; text is crisp at native and non-native resolutions with no atlas bleeding, and honors the UI-scale setting; recooking the font is byte-identical; `ui.unit` green with all suites green and zero warnings.

**Open decision before implementation** — ⚑ **which TTF ships**, as a source-asset policy call (`AGENTS.md` §5). It must be permissively licensed (SIL OFL or similar), carry `glyf` outlines, and land in `assets/fonts/` with its license file recorded. Dev work on the rasterizer can proceed against any local TTF; nothing may be committed until the user picks one.

---

## 5. Standing Design Questions

Tracked here so they're decided consciously (record outcomes in `docs/decisions/`):

1. ECS storage model — ✅ decided: sparse-set (`docs/decisions/001-ecs-storage-model.md`).
2. Fixed-point vs. double for sim positions — default double; revisit only with evidence.
3. Render graph — introduce when pass management hurts (est. Phase 4–6), not speculatively.
4. Travel model baseline — ✅ decided: gates + cruise, drive as late-game unlock; no time compression (`docs/decisions/004-travel-model.md`, `docs/decisions/005-time-compression.md`).
5. Audio decoder dependency (ogg) — decide at Phase 8; requires user approval per dependency policy.
6. GPU particles, atmosphere shaders, planet detail — post-Phase 7 polish tier.
