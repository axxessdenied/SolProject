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
│   core       math, memory, containers, jobs,         │
│              log/assert, events, serialization       │
├─────────────────────────────────────────────────────┤
│   platform   window, input, filesystem, time, dylib  │
│              (win32/ backend now, linux/ later)      │
└─────────────────────────────────────────────────────┘
tools/cooker/   offline asset compiler (own executable)
```

### 2.1 Platform layer (`sol::platform`)

Window creation, input (keyboard/mouse first; gamepad/HOTAS later), filesystem paths + file IO + change watching (for hot-reload), high-resolution clock, dynamic library loading. Portable API in public headers; `win32/` implementation selected at compile time (no virtual dispatch needed — one backend per build). Vulkan surface creation is the one sanctioned platform↔rhi touch point.

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

Custom ECS; final storage design chosen via a short spike in Phase 3 (archetype/SoA vs. sparse-set — decide with a benchmark of our actual access patterns: thousands of ships/projectiles iterated linearly per sim tick, sparse component churn on projectiles).

- Entities are generational handles. Components are plain structs (POD-leaning, serializable).
- Systems are free functions scheduled explicitly (fixed order first; job-graph parallelism later, using the core job system).
- Queries/views for iteration; deferred structural changes via command buffers so systems can run in parallel safely.
- The ECS is the save-game backbone: world state serialization walks component storage.

### 2.6 Asset pipeline (`tools/cooker` + `sol::assets`)

- **Cooker** (separate executable, links `core` only): imports source assets → engine-native binary formats with stable asset ids + a pack manifest. Importers written in-repo: glTF (JSON + buffers) for meshes, PNG for textures (→ BCn-compressed at cook time; BCn encoder in-repo, quality over speed initially), WAV for audio (later), TOML/JSON data defs → validated binary tables.
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
- **Game UI** (later phase): custom retained-or-immediate hybrid renderer for HUD (targeting reticles, velocity indicators, shield/hull), map screens (system + galaxy), menus, trade screens. Driven from Lua for layout/flow, C++ for drawing. Design deferred until Phase 4 teaches us what the HUD needs.

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

### Phase 2 — Renderer Foundations
Math library complete + fully unit-tested; cooker v0 (glTF mesh + PNG→BCn texture → pack file); runtime asset registry (sync loading fine for now); depth buffer, perspective camera, transform uniforms; textured mesh rendering; Dear ImGui integrated (perf overlay, console); free-fly camera; shader build pipeline in CMake + dev hot-reload.
**Exit**: fly around a textured scene at high framerate with ImGui stats; math test suite green.

### Phase 3 — Core Systems
ECS storage spike (benchmark archetype vs. sparse-set on representative workloads; record decision) then implementation: handles, queries, command buffers; job system + parallel-for; event bus; fixed-timestep loop with render interpolation; binary serialization with versioning; TOML parser; PRNG streams.
**Exit**: 10k+ moving entities simulated at 60 Hz sim / uncapped render with interpolation; world state round-trips through save/load; all core tests green.

### Phase 4 — Space Flight Vertical Slice ★
The proof-of-concept milestone. `DVec3` sim space + camera-relative rendering proven at solar-system distances; 6-DoF Newtonian flight + assists, ship controllable in first/third person; starfield cubemap, one planet impostor, one station mesh, sun light + HDR; thruster particles (CPU); ImGui-based provisional HUD (velocity, orientation, target); debug draw.
**Exit**: fly from a station to a planet 100,000+ km away with no precision artifacts; flight feels controllable; this build is the template for all future content.

### Phase 5 — Lua + Data-Driven Content
Lua VM embedded; binding layer + entity/asset handle scheme; ships, weapons (defs only), factions defined in TOML data loaded through the cooker/registry; script + data hot-reload; in-game Lua console; mod directory layering (base game = mod zero).
**Exit**: change a ship's stats or a Lua behavior and see it live without restarting; spawn ships from data defs via console.

### Phase 6 — Combat & AI
Weapons (projectile + hitscan), damage model (shields → hull, per GDD), collision integrated with flight; NPC pilots: C++ steering behaviors (pursue, evade, formation, collision avoidance) driven by Lua state machines (roles: fighter, trader, patrol); target selection + basic threat logic; combat feedback (hit effects, explosions, HUD target info).
**Exit**: a dogfight against 5+ NPCs that is fair, legible, and fun enough to replay voluntarily.

### Phase 7 — Universe & Economy
Procedural galaxy generation (seeded; systems, connections, faction territory claims, station placement per handcrafted rules); jump-gate travel + in-system cruise (per GDD travel model); docking (request, autodock or manual per GDD); trading UI (provisional ImGui); agent-based economy on the coarse sim layer; sim-LOD promotion/demotion when the player changes systems; full save/load including galaxy + economy state.
**Exit**: buy low in one system, jump two systems over, sell high — against prices that moved because of simulated NPC traders, not scripts; save/reload mid-run is lossless; same seed ⇒ same galaxy.

### Phase 8+ — Sandbox Depth (sketch, spec before starting)
Faction simulation (relations, territory shifts, patrols/raids) and player reputation; mission/contract system (Lua-authored generators); ship outfitting + progression + purchasing; custom game UI replacing provisional ImGui screens; audio system; mining/salvage loops; exploration/scanning; performance hardening; Linux platform backend when it earns priority. ⚑ Each item gets its own spec added to this document before implementation.

---

## 5. Standing Design Questions

Tracked here so they're decided consciously (record outcomes in `docs/decisions/`):

1. ECS storage model — Phase 3 spike (archetype vs. sparse-set).
2. Fixed-point vs. double for sim positions — default double; revisit only with evidence.
3. Render graph — introduce when pass management hurts (est. Phase 4–6), not speculatively.
4. Travel model baseline (gates + cruise vs. charged jump drive) — GDD open question; engine supports either.
5. Audio decoder dependency (ogg) — decide at Phase 8; requires user approval per dependency policy.
6. GPU particles, atmosphere shaders, planet detail — post-Phase 7 polish tier.
