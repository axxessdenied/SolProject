#pragma once

// Human-authored game data definitions (engine plan Phase 5): ships, weapons,
// factions, commodities, stations, modules, and crew, parsed from TOML and validated
// against a strict schema (unknown or mistyped keys are errors, so typos die
// at load time, not silently at play time). Documents merge in layer order —
// a def re-using an earlier id replaces it wholesale, which is what gives mod
// directory layering (base game = mod zero) its override semantics.
//
// Defs use flat snake_case keys inside [[ship]] / [[weapon]] / [[faction]]
// array-of-table elements; see game/data/ for the base-game examples.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sol::assets {

// The stats outfitting modifiers may touch (engine plan Phase 8a). TurnRate
// applies uniformly to all three max_turn_rate axes.
enum class FitStat : std::uint32_t
{
    ForwardAccel = 0,
    ReverseAccel,
    LateralAccel,
    VerticalAccel,
    MaxSpeed,
    TurnRate,
    CruiseSpeedScale,
    ShieldStrength,
    ShieldRegen,
    Armor,
    Hull,
    WeaponCapacitor,
    WeaponRecharge,
    Cargo,
    ScanRange,      // Phase 8e: pulse/target-scan reach
    ScanSpeed,      // Phase 8e: target-scan progress rate
    CollectorRange, // Phase 8f: how far mined ore is drawn in from
    Count,
};

inline constexpr std::size_t kFitStatCount = static_cast<std::size_t>(FitStat::Count);

// Shared modifier vocabulary for modules and crew ("<stat>_add"/"<stat>_mul"
// def keys). Resolution is order-independent: adds sum onto the base stat,
// then muls multiply the result (see loadout.hpp).
struct StatModifiers
{
    static constexpr std::array<float, kFitStatCount> ones()
    {
        std::array<float, kFitStatCount> a{};
        a.fill(1.0f);
        return a;
    }

    std::array<float, kFitStatCount> add{};
    std::array<float, kFitStatCount> mul = ones();
};

// Mirrors sim::ShipTuning field-for-field as plain floats (assets sits below
// sim in the layering, so it cannot include the sim type; the game converts).
struct ShipFlightTuning
{
    float forwardAccel = 60.0f;
    float reverseAccel = 40.0f;
    float lateralAccel = 30.0f;
    float verticalAccel = 30.0f;
    float maxSpeed = 220.0f;
    float maxTurnRate[3] = {1.6f, 1.2f, 2.6f}; // pitch, yaw, roll (rad/s)
    float angularAccel[3] = {6.0f, 4.5f, 9.0f};
    float boostAccelScale = 3.0f;
    float boostSpeedScale = 1.75f;
    float cruiseSpeedScale = 25'000.0f;
    float cruiseAccelScale = 12'000.0f;
};

// Mirrors sim::DefenseTuning (directional shields per decisions/002).
struct ShipDefenseTuning
{
    float shieldStrength = 100.0f; // hp per facing
    float shieldRegen = 8.0f;      // hp/s before pips scaling
    float shieldRegenDelay = 4.0f; // seconds after a hit
    float armor = 50.0f;           // ablative
    float hull = 100.0f;
};

// Mirrors the def-driven part of sim::PowerTuning (pips per decisions/003).
struct ShipPowerTuning
{
    float weaponCapacitor = 100.0f;
    float weaponRecharge = 15.0f; // units/s at scale 1
};

// Catalog gating shared by sellable defs (Phase 8b): which faction's stations
// stock the item (empty = every faction) and the player standing the owner
// requires before selling it (pirate-kind owners fence past min_rep).
struct CatalogGate
{
    std::vector<std::string> factions; // seller allowlist of faction ids
    float minRep = -100.0f;            // owner-standing gate, -100..100
};

struct ShipDef
{
    std::string id;   // stable, namespaced, e.g. "sol.shuttle"
    std::string name; // display name
    std::string model = "ship";
    // The interior drawn when the player flies THIS ship from the seat
    // (Phase 19). Empty means the `cockpit` role, which is what all three
    // shipped hulls take - so a freighter and an interceptor still share one
    // interior until somebody authors a second. Only ever consulted for the
    // player's active ship; an NPC has no seat.
    std::string cockpit;
    float scale = 1.0f;
    ShipFlightTuning flight;
    ShipDefenseTuning defense;
    ShipPowerTuning power;
    std::string weaponId;        // weapon def id; empty = unarmed
    float cargoCapacity = 50.0f; // trade goods, units
    // Scanning (engine plan Phase 8e): how far a pulse reaches and how fast a
    // target scan resolves. Scanner modules move both like any other stat.
    float scanRange = 2.5e8f; // meters
    float scanSpeed = 1.0f;   // target-scan progress multiplier
    // Mining (engine plan Phase 8f): how far loose ore is drawn in from.
    // Deliberately far shorter than a mining beam reaches, so an unfitted ship
    // has to fly in and scoop what it cut; a collector rig is what buys the
    // right to sit still and mine.
    float collectorRange = 120.0f; // meters
    // Outfitting (engine plan Phase 8a): hull price, fitting budgets, slots.
    float price = 10'000.0f;
    float mass = 10'000.0f;    // kg; module mass dilutes accelerations
    float powerOutput = 10.0f; // fit budget: sum of module power_draw
    std::uint32_t slotsShield = 1;
    std::uint32_t slotsEngine = 1;
    std::uint32_t slotsCargo = 1;
    std::uint32_t slotsUtility = 1;
    std::uint32_t crewBerths = 1;
    CatalogGate gate;
    std::string source; // document that last defined this id (diagnostics)
};

enum class ModuleSlot : std::uint32_t
{
    Shield = 0,
    Engine,
    Cargo,
    Utility,
    Count,
};

inline constexpr std::size_t kModuleSlotCount = static_cast<std::size_t>(ModuleSlot::Count);

// An outfitting module: occupies one typed slot, costs power from the ship's
// budget, adds mass, and modifies stats (engine plan Phase 8a).
struct ModuleDef
{
    std::string id;
    std::string name;
    ModuleSlot slot = ModuleSlot::Utility;
    float price = 100.0f;
    float mass = 0.0f;      // kg
    float powerDraw = 0.0f; // against ShipDef::powerOutput
    StatModifiers modifiers;
    CatalogGate gate;
    std::string source;
};

// A hireable crew member (decisions/006: trivial passive bonuses): occupies a
// berth, costs a one-time hire fee, and modifies stats like a module without
// mass or power draw.
struct CrewDef
{
    std::string id;
    std::string name;
    std::string role; // display flavor, e.g. "Engineer"
    float price = 200.0f;
    StatModifiers modifiers;
    CatalogGate gate;
    std::string source;
};

struct WeaponDef
{
    std::string id;
    std::string name;
    std::string kind; // "projectile" | "hitscan"
    float damage = 0.0f;
    float rateOfFire = 1.0f;      // shots/s
    float range = 1'000.0f;       // meters
    float projectileSpeed = 0.0f; // m/s; 0 for hitscan
    float energyCost = 10.0f;     // capacitor draw per shot
    // Mining (engine plan Phase 8f): yield units cut out of a rock or wreck
    // per second of held beam. 0 leaves a weapon a weapon — a mining laser is
    // an ordinary hardpoint choice, not a mode.
    float miningPower = 0.0f;
    float price = 500.0f; // shipyard price (Phase 8a outfitting)
    // What its bolt is drawn as (Phase 19). Empty means the `bolt` role, so
    // adding this key changed nothing for the four shipped weapons. A hitscan
    // weapon spawns no projectile and ignores it.
    //
    // ⚑ Under the unit-radius contract: a bolt is drawn at 0.3 x 0.3 x 4 m,
    // so the model must be authored at radius 1.
    std::string model;
    CatalogGate gate;
    std::string source;
};

enum class FactionKind : std::uint32_t
{
    Major = 0, // authored territory claimant (generator capital)
    Pirate,    // clan template; instantiated per lawless fringe neighborhood
};

// One side's declared initial standing toward another faction ("id:standing"
// strings in TOML, -100..100). The matrix is symmetric: a pair declared from
// both sides must agree (validateFactions).
struct FactionRelation
{
    std::string otherId;
    float standing = 0.0f;
};

// How strongly a faction favours one station archetype in the territory it
// holds ("sol.station_factory:2.5" in TOML), Phase 13.
//
// A MULTIPLIER on the archetype's region weight rather than a replacement for
// it, so the economy's regional logic — ore in the fringe, factories in the
// core — still sets the baseline and a faction only tilts it. 0 is a legal and
// deliberate "never builds one"; the default, for an archetype a faction says
// nothing about, is 1.0.
struct StationBias
{
    std::string stationId;
    float weight = 1.0f;
};

struct FactionDef
{
    std::string id;
    std::string name;
    std::string description;
    float color[3] = {1.0f, 1.0f, 1.0f}; // sRGB accent for HUD/markers
    FactionKind kind = FactionKind::Major;
    // Personality weights (0..1) consumed by the Lua decision rules
    // (Phase 8b faction sim): raid appetite and drift-back-to-baseline rate.
    float aggression = 0.5f;
    float forgiveness = 0.5f;
    std::vector<FactionRelation> relations;
    std::vector<std::string> shipsPatrol; // ship def ids for ambient wings
    std::vector<std::string> shipsRaider;
    // Hulls this faction's haulers fly (Phase 8x). A trader puppet is a body
    // for a coarse EconomyTrader, and it needs a roster for the same reason a
    // patrol wing does: the def decides what the player sees coming.
    std::vector<std::string> shipsTrader;
    // What this faction BUILDS where it holds territory (Phase 13). Empty —
    // the default — means it builds to the region's baseline like everyone
    // else, which is what every faction did before this existed.
    //
    // ⚑ These descriptions have asserted economic characters since 8b — an
    // "authoritarian industrial bloc", "traders and haulers", "settler
    // militias" — and the galaxy generator read none of it. This is the key
    // that turns that prose into a number.
    std::vector<StationBias> stationBias;
    std::string source;
};

struct CommodityDef
{
    std::string id;
    std::string name;
    float basePrice = 10.0f; // credits/unit at neutral stock
    // How often asteroids in each region tier are made of this (Phase 8f).
    // All zero — the default — means it is not something you mine.
    float oreWeightCore = 0.0f;
    float oreWeightFrontier = 0.0f;
    float oreWeightFringe = 0.0f;
    // What a rock of this ore, and a chunk cut out of one, are drawn as
    // (Phase 19). Empty means the `rock` and `ore_chunk` roles, so adding
    // these changed nothing for the shipped commodities. Only a commodity
    // with an ore weight is ever mined, so on anything else they are inert.
    //
    // ⚑ Both are under the unit-radius contract: a rock's instance scale IS
    // its radius in metres and a chunk is drawn at 6 m, so a model of any
    // other radius silently resizes every instance AND its mining hit sphere.
    std::string model;
    std::string chunkModel;
    std::string source;
};

// A sound cue (Phase 8t): what the game asks for by id, pointing at a cooked
// .saud by name. Data rather than an enum so a cue can be retuned or replaced
// on a script reload, and so a mod layer can override one without a rebuild.
struct SoundDef
{
    std::string id;
    std::string asset; // cooked file stem, e.g. "weapon_fire" -> weapon_fire.saud
    float gain = 1.0f;
    // Playback rate is jittered by +-pitchJitter around 1, so a cue fired
    // repeatedly (a gun, a mining beam) does not comb-filter against itself.
    float pitchJitter = 0.0f;
    // Meters at which a positional voice has fallen to half gain. Ignored
    // when the cue is played 2D.
    float rolloff = 500.0f;
    // Concurrent voices of this cue; 0 is unlimited. A cap keeps one chattering
    // source from crowding the whole mix out.
    std::uint32_t maxInstances = 0;
    std::string source;
};

// "no material", the value a model's resolved index holds until the merged
// database has been walked. Namespace scope rather than a `DefDatabase`
// static because `ModelDef` carries one and is declared first.
inline constexpr std::uint32_t kNoMaterial = 0xFFFFFFFFu;

// How colour leaves a material's fragment shader and reaches the target.
// ⚑ A MIRROR OF `rhi::BlendMode` AND DELIBERATELY NOT THAT TYPE: `sol::assets`
// does not depend on `sol::rhi`, and a def layer that included Vulkan headers
// to name three states would be the wrong end of that dependency. The mapping
// is one switch in `MaterialRegistry`, and a test asserts it stays total.
enum class MaterialBlend
{
    Opaque,
    Alpha,    // premultiplied: src.rgb + dst.rgb * (1 - src.a)
    Additive, // src.rgb + dst.rgb
};

// One extra texture a material declares beyond its albedo (Phase 25 stage C).
//
// ⚑ THE ALBEDO IS NOT ONE OF THESE, and that asymmetry is deliberate rather
// than a leftover. `texture` is the one every mesh material has, it is bound at
// set 0 exactly as it has been since Phase 3, and keeping it there is what lets
// `mesh.frag`, `membrane.frag` and the Forge's whole viewport stay untouched by
// this stage. These are the material's OWN slots, bound at set 1 in the order
// written, which is the order the shader must declare them in.
struct MaterialSlot
{
    std::string name;    // the shader's variable name, for an error a reader can act on
    std::string texture; // cooked file stem, e.g. "cockpit_glow" -> cockpit_glow.stex
};

// One scalar a material tunes on its shader (Phase 25 stage C). Matched into
// the shader's uniform block BY NAME, never by position - see `spirv_reflect`
// for why position is the silent-misalignment bug this exists to avoid.
struct MaterialParam
{
    std::string name;
    float value = 0.0f;
};

// HOW A SURFACE IS DRAWN (Phase 25 stage A), split out of the model that wears
// it, WHAT DRAWS IT (stage B), and WHAT THAT SHADER GETS FED (stage C).
//
// ⚑ THE PREFIX `sol.auto.` IS RESERVED and an authored row using it is
// refused. Every `[[model]]` row that names no material gets one SYNTHESISED
// from its own `texture`/`emissive`/`translucent`/`alpha` under that prefix,
// which is what lets every committed def file keep working untouched while the
// renderer stops reading a model's fields at all.
struct MaterialDef
{
    std::string id;
    std::string texture; // cooked file stem, e.g. "hull" -> hull.stex
    // Unlit albedo glow added at draw time; see `ModelDef` for what it is for.
    float emissive = 0.0f;
    // Phase 12's alpha-blended pipeline, recorded after the sky. `alpha` is
    // coverage in 0..1 and is only meaningful when `translucent` is set; the
    // opaque path passes 1.0, which the shader premultiplies by and so leaves
    // its output unchanged.
    bool translucent = false;
    float alpha = 1.0f;

    // ⚑⚑ PHASE 25 STAGE B: WHAT DRAWS IT. SPIR-V stems, so "mesh" is
    // mesh.vert.spv and mesh.frag.spv, found on the shader search path. Two
    // keys rather than one because a material may keep the stock vertex stage
    // and bring only its own fragment stage, which is the common case.
    std::string vertexShader = "mesh";
    std::string fragmentShader = "mesh";

    // ⚑⚑ PIPELINE STATE, AND ITS DEFAULTS ARE PHASE 12's HARDCODED VARIANT.
    // `mesh_renderer.cpp` used to build the translucent pipeline as "the same
    // shaders, layout and vertex format with three fields moved"; those three
    // fields are these, and `translucent` now SEEDS them rather than deciding
    // them. So a row that says nothing draws exactly as it always did, and a
    // row that wants blending without giving up its depth write can say so.
    //
    // ⚑ `translucent` still decides which PASS a draw is recorded in - after
    // the sky rather than in the opaque block - because that is a fact about
    // the frame, not about the pipeline. Blending in the opaque block would be
    // painted over by the sky, which reads as broken blending and is in fact a
    // misplaced pass.
    MaterialBlend blend = MaterialBlend::Opaque;
    bool depthTest = true;
    bool depthWrite = true;
    bool cullBackFaces = true;

    // ⚑⚑ PHASE 25 STAGE C: WHAT THE SHADER GETS FED, DECLARED RATHER THAN
    // REFLECTED (decision 2). `slots` are extra textures at set 1 bindings
    // 0..n-1 in written order; `params` are floats packed into a uniform buffer
    // at the binding after them. The declaration is what the descriptor layout
    // is BUILT from; the SPIR-V is what it is CHECKED against, so a material
    // that lies about its shader is refused at load with the slot named
    // instead of reaching the validation layer - which is on in dev builds and
    // off in shipping, i.e. exactly backwards from where it is needed.
    //
    // ⚑ A material with neither declares no set 1 at all and gets the same
    // pipeline layout every material had before this stage. That is what keeps
    // the other eight rows free.
    std::vector<MaterialSlot> slots;
    std::vector<MaterialParam> params;

    // True for a row this database derived from a `[[model]]`, false for one
    // somebody wrote. The derived set is rebuilt from scratch after every
    // merge, so a later layer editing a model row cannot leave a stale one
    // behind, and nothing outside this file has to remember to ask for it.
    bool synthesised = false;
    std::string source;
};

// A drawable model (Phase 9): the cooked mesh and texture the renderer binds,
// and the figures the sim measures the thing by. Data rather than an enum
// because until this existed the whole set was a five-member C++ enum behind
// four hardcoded switches - so a mesh could be authored and cooked and still
// have no way of reaching the game. `[[model]]` rows are what the authoring
// tool writes; the def order is the runtime model index.
struct ModelDef
{
    std::string id;
    std::string mesh; // cooked file stem, e.g. "ship" -> ship.smesh
    // ⚑ Phase 25 stage A: `material` names a `[[material]]` row, and a row
    // that names one carries NONE of the four surface keys below - they are
    // that row's business now, and a model carrying both is refused at load
    // rather than resolved by a precedence rule nobody can see in the file.
    // A row that names no material gets one synthesised from the four, so the
    // committed catalog and every mod written before this stage keep working
    // exactly as written.
    std::string material;
    // The index into `materials()` this row resolved to, or `kNoMaterial`
    // while the named row has not been merged yet. `validateMaterials` is what
    // turns "still unresolved once every layer is in" into a refusal.
    std::uint32_t materialIndex = kNoMaterial;
    std::string texture; // cooked file stem, e.g. "hull" -> hull.stex
    // Radius in meters at instance scale 1; the sim multiplies by the scale
    // for collision and weapon hit tests. A model authored at unit radius
    // (the asteroid) therefore takes its size from the scale, and one
    // authored at its real size (the ship, the station) is drawn at scale 1.
    float radius = 1.0f;
    // What NPC steering and autopilot dodge, meters at scale 1; 0 means "the
    // same as radius". A station's is deliberately wider than what you can
    // hit, because 8r's berths ring at 200 m and that approach was tuned
    // against the wider figure - larger than what you can hit is always safe.
    float avoidRadius = 0.0f;
    // Unlit albedo glow added at draw time. Vacuum ambient is 1.2%, which is
    // right for a hull and pitch black for a room the player is sitting in.
    float emissive = 0.0f;
    // Whether the model blocks anything at all. A gate is a doorway you fly
    // through (Phase 8w), and before this field that fact was expressed as
    // "the only Cube left among statics" - which would have silently
    // un-solidified any future Cube-shaped static, as 8w's own gaps recorded.
    bool solid = true;
    // Phase 12: drawn through the alpha-blended pipeline instead of the opaque
    // one, and recorded after the sky. Declared on the MODEL rather than on
    // the instance so that the next translucent thing in this game is a def
    // row and no C++ at all. `alpha` is coverage in 0..1 and is only
    // meaningful when `translucent` is set; the opaque path passes 1.0, which
    // the shader premultiplies by and so leaves its output unchanged.
    bool translucent = false;
    float alpha = 1.0f;
    std::string source;
};

// A SLOT the engine draws into, and the model that fills it (Phase 19).
//
// Stage A of Phase 9 replaced the ModelId enum with a def lookup and left a
// second wall standing: the only model resolution that read data was a ship
// def's, and every other thing in the world - the gate, its membrane, a rock,
// an ore chunk, a bolt, the cockpit - was fetched by a string literal compiled
// into C++. A `[[role]]` row is how those names leave the source.
//
// ⚑ The ROLE ids stay in C++ and only the ANSWERS move out, which is the
// distinction the whole phase turns on: the set of slots is a property of the
// engine (it either draws a cockpit or it does not), while what fills one is a
// property of the content. See `game/src/model_roles.hpp` for the list this
// game asks for.
//
// ⚑ It is an id-keyed ARRAY rather than a singleton `[world]` table because a
// singleton does not parse - `mergeToml` requires an array of tables - and
// because `id` is what `mergeDef` merges on, so a mod re-points a role exactly
// the way it re-points a ship, with no merge rule invented for the occasion.
struct RoleDef
{
    std::string id;    // the slot, e.g. "gate"; must be one the engine asks for
    std::string model; // a `[[model]]` id; must exist, checked at load
    std::string source;
};

// One production or consumption line on a station ("sol.food:0.5" in TOML).
struct StationRate
{
    std::string commodityId;
    float rate = 0.0f; // units/s
};

// A station archetype: how often the galaxy generator places it per region
// tier, and what its market produces/consumes (Phase 7 economy).
struct StationDef
{
    std::string id;
    std::string name;
    // Which `[[model]]` this archetype is drawn as (Phase 9 stage H).
    //
    // ⚑ Before this key existed a station's LOOK was C++: `space_world.cpp`
    // resolved `modelByName("station")` once and handed the same model to every
    // station in the galaxy, so an authored station mesh had no way of reaching
    // the game at all. Stage A took `ModelId` out of the enum and left it a
    // hardcoded NAME, and this is the first of those names to become data.
    // The default is the model every archetype already drew, so it changes
    // nothing until somebody authors a second one.
    std::string model = "station";
    float weightCore = 1.0f;
    float weightFrontier = 1.0f;
    float weightFringe = 1.0f;
    std::vector<StationRate> produces;
    // Upkeep (Phase 8g): burned regardless of what the station makes, clamped
    // at zero, and it never gates production. A hungry station is a hook for a
    // later unrest mechanic, not a production stall.
    std::vector<StationRate> consumes;
    // Feedstock (Phase 8g): the thing the station transforms. Production is
    // throttled by how much of it is on hand, so a refinery with no ore makes
    // no metal. Kept separate from `consumes` because the production graph is
    // a cycle (food -> ore -> metal -> machinery -> food) and gating on every
    // input would let one empty link seize the whole galaxy permanently.
    std::vector<StationRate> feedstock;
    // Where the archetype's output comes from (Phase 8g). Empty — the
    // default — means it is made from stocked feedstock like everything else.
    // "field" means it comes out of the ground: a Mining Outpost draws ore
    // from the asteroid fields in its own system, so a system with no rock
    // supports no mine.
    std::string producesFrom;
    float stockCapacity = 1'000.0f; // per commodity
    // Refinery service (Phase 8f): the archetype takes refine_input off the
    // player's hands and hands back refine_output later. Both empty — the
    // default — means the station refines nothing.
    std::string refineInput;
    std::string refineOutput;
    std::string source;
};

// ---------------------------------------------------------------------------
// Authored systems (Phase 29). A `[[system]]` row is a place somebody PUT
// somewhere, as opposed to the eighty the seed produces, and the campaign can
// name it because its id is invented here rather than rolled.
//
// ⚑⚑⚑ EVERY OPTIONAL FIELD CARRIES AN EXPLICIT `has…` FLAG RATHER THAN A
// SENTINEL, AND THAT IS FORCED RATHER THAN CHOSEN. Two of the fields the
// generator holds have no free sentinel to spare: `SystemSpec::factionIndex`
// is `kNoFaction` both when nobody has said anything and when an author
// deliberately says "lawless", and `primaryPlanet == 0` is both "unset" and
// "the first planet". Comparing against a default therefore cannot answer
// "did the author write this?", which is the only question the generator needs
// to ask - so the parser answers it once and records the answer.
// ---------------------------------------------------------------------------

// A planet an author named. Positions are deliberately absent: the generator
// still lays the orbits out, so writing a system by hand never means writing
// coordinates in metres.
struct AuthoredPlanetDef
{
    std::string name;
    double radius = 0.0; // metres; unset means the generator rolls one
    bool hasRadius = false;
};

// A station an author placed. `stationId` names a `[[station]]` archetype and
// is resolved to its index at the same place, and by the same rule, that
// `faction_station_bias` already resolves one.
struct AuthoredStationDef
{
    std::string name;
    std::string stationId;
};

struct SystemDef
{
    std::string id; // invented by the author; the campaign's handle on the place
    // How the generator chooses which node this system becomes. Phase 29
    // stage A ships only "random" (any ordinary system); the other three
    // rules in decisions/018 arrive in stage B.
    std::string placement = "random";
    std::string name;
    bool hasName = false;
    // "core" | "frontier" | "fringe".
    std::string region;
    bool hasRegion = false;
    // A `[[faction]]` id. Mutually exclusive with `lawless`, because the two
    // are exactly the pair that a sentinel could not tell apart.
    std::string factionId;
    bool hasFaction = false;
    bool lawless = false;
    std::uint32_t primaryPlanet = 0;
    bool hasPrimaryPlanet = false;
    // A placement flag and nothing else in this phase: nothing hides a system
    // from the map yet, and the exploration payoff GDD §8 promises is a later
    // phase reading this.
    bool secret = false;
    std::vector<AuthoredPlanetDef> planets;
    std::vector<AuthoredStationDef> stations;
    std::string source;
};

class DefDatabase
{
public:
    void clear();

    // Merges one TOML document (any mix of [[ship]]/[[weapon]]/[[faction]]).
    // sourceName appears in errors and def provenance. On error the database
    // is left as it was before the call.
    [[nodiscard]] bool
    mergeToml(const char* text, std::size_t length, const char* sourceName, std::string* outError = nullptr);

    // Reads and merges every *.toml directly inside directory, sorted by
    // path for determinism. A missing directory is fine (a mod without data).
    [[nodiscard]] bool mergeDirectory(const char* directory, std::string* outError = nullptr);

    // Cross-def checks that need the fully merged database: a faction pair
    // declaring initial relations from both sides must agree (Phase 8b).
    // Relations naming unknown faction ids are ignored downstream, not here —
    // a mod may remove a faction another def still references.
    [[nodiscard]] bool validateFactions(std::string* outError = nullptr) const;

    // Cross-def check for `[[role]]` rows, and it REFUSES rather than warns
    // (Phase 19). `modelIdFromName`'s warn-and-fall-back is right for a ship
    // def - one bad name breaks one ship and a real hull is still there to
    // stand in - but a role has no fallback once the literal it replaced is
    // gone, and a silently invisible gate in all eighty systems is worse than
    // a refusal that names the file. Same treatment `loadModels` gives a mesh
    // that is not on disk.
    //
    // `required` is the caller's vocabulary: `sol::assets` validates whatever
    // list it is handed and does not itself know what a cockpit is.
    [[nodiscard]] bool validateRoles(std::span<const char* const> required,
                                     std::string* outError = nullptr) const;

    // Cross-def check for `[[model]]` rows naming a `[[material]]` (Phase 25
    // stage A), and it REFUSES for the same reason `validateRoles` does: a
    // model whose material is missing has nothing left to draw with, because
    // naming a material is exactly what gave up the four surface keys. It is
    // a separate pass rather than a parse-time check because a material may
    // legitimately live in an earlier or a later layer than the model.
    [[nodiscard]] bool validateMaterials(std::string* outError = nullptr) const;

    // Cross-def check for `[[system]]` rows (Phase 29), and it REFUSES for the
    // reason decision 3 gives: an authored system whose faction or whose
    // station archetype does not exist has no fallback that is not a lie about
    // where the campaign starts. Separate from the parse for the same reason
    // `validateMaterials` is - a faction may legitimately live in an earlier or
    // a later layer than the system that names it.
    [[nodiscard]] bool validateSystems(std::string* outError = nullptr) const;

    [[nodiscard]] const ShipDef* findShip(const char* id) const;
    [[nodiscard]] const WeaponDef* findWeapon(const char* id) const;
    [[nodiscard]] const FactionDef* findFaction(const char* id) const;
    [[nodiscard]] const CommodityDef* findCommodity(const char* id) const;
    [[nodiscard]] const StationDef* findStation(const char* id) const;
    [[nodiscard]] const SystemDef* findSystem(const char* id) const;
    [[nodiscard]] const ModuleDef* findModule(const char* id) const;
    [[nodiscard]] const CrewDef* findCrew(const char* id) const;
    [[nodiscard]] const SoundDef* findSound(const char* id) const;
    [[nodiscard]] const ModelDef* findModel(const char* id) const;
    [[nodiscard]] const MaterialDef* findMaterial(const char* id) const;
    [[nodiscard]] const RoleDef* findRole(const char* id) const;

    // Index of a model by id, or kNoModel. The renderer and the sim both key
    // off this index rather than off the string, so a name is resolved once at
    // spawn and never in a per-tick loop.
    static constexpr std::uint32_t kNoModel = 0xFFFFFFFFu;
    [[nodiscard]] std::uint32_t modelIndex(const char* id) const;

    // Index of a material by id, or `kNoMaterial`. Same bargain as
    // `modelIndex`: the renderer binds off an integer, never off a string.
    [[nodiscard]] std::uint32_t materialIndex(const char* id) const;

    // The model index filling a role, or kNoModel. After `validateRoles` has
    // passed this cannot fail for a required role, so callers resolve once at
    // spawn and do not branch per instance.
    [[nodiscard]] std::uint32_t roleModelIndex(const char* id) const;

    // First-definition order; later layers replace elements in place, so
    // indices stay stable across a reload that only edits values.
    [[nodiscard]] const std::vector<ShipDef>& ships() const { return m_ships; }

    [[nodiscard]] const std::vector<WeaponDef>& weapons() const { return m_weapons; }

    [[nodiscard]] const std::vector<FactionDef>& factions() const { return m_factions; }

    [[nodiscard]] const std::vector<CommodityDef>& commodities() const { return m_commodities; }

    [[nodiscard]] const std::vector<StationDef>& stations() const { return m_stations; }

    // Authored systems in first-definition order, which is the order their
    // placement rules resolve in (decision 4).
    [[nodiscard]] const std::vector<SystemDef>& systems() const { return m_systems; }

    [[nodiscard]] const std::vector<ModuleDef>& modules() const { return m_modules; }

    [[nodiscard]] const std::vector<CrewDef>& crew() const { return m_crew; }

    [[nodiscard]] const std::vector<SoundDef>& sounds() const { return m_sounds; }

    [[nodiscard]] const std::vector<ModelDef>& models() const { return m_models; }

    // Authored rows first, in first-definition order, then the synthesised
    // ones in model order. Both halves are deterministic, which is what lets a
    // model hold an index into this rather than a name.
    [[nodiscard]] const std::vector<MaterialDef>& materials() const { return m_materials; }

    [[nodiscard]] const std::vector<RoleDef>& roles() const { return m_roles; }

private:
    std::vector<ShipDef> m_ships;
    std::vector<WeaponDef> m_weapons;
    std::vector<FactionDef> m_factions;
    std::vector<CommodityDef> m_commodities;
    std::vector<StationDef> m_stations;
    std::vector<SystemDef> m_systems;
    std::vector<ModuleDef> m_modules;
    std::vector<CrewDef> m_crew;
    std::vector<SoundDef> m_sounds;
    std::vector<ModelDef> m_models;
    std::vector<MaterialDef> m_materials;
    std::vector<RoleDef> m_roles;

    // Drops every synthesised row, derives one afresh for each model that
    // names no material, and re-resolves every model's index. Runs at the tail
    // of each merge so the database is never half-resolved and no caller has
    // to remember a second call - the trap `validateRoles` avoids only by
    // being const.
    void resolveMaterials();
};

} // namespace sol::assets
