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
    ScanRange, // Phase 8e: pulse/target-scan reach
    ScanSpeed, // Phase 8e: target-scan progress rate
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
    float scale = 1.0f;
    ShipFlightTuning flight;
    ShipDefenseTuning defense;
    ShipPowerTuning power;
    std::string weaponId;        // weapon def id; empty = unarmed
    float cargoCapacity = 50.0f; // trade goods, units
    // Scanning (engine plan Phase 8e): how far a pulse reaches and how fast a
    // target scan resolves. Scanner modules move both like any other stat.
    float scanRange = 2.5e8f;  // meters
    float scanSpeed = 1.0f;    // target-scan progress multiplier
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
    float rateOfFire = 1.0f;        // shots/s
    float range = 1'000.0f;         // meters
    float projectileSpeed = 0.0f;   // m/s; 0 for hitscan
    float energyCost = 10.0f;       // capacitor draw per shot
    // Mining (engine plan Phase 8f): yield units cut out of a rock or wreck
    // per second of held beam. 0 leaves a weapon a weapon — a mining laser is
    // an ordinary hardpoint choice, not a mode.
    float miningPower = 0.0f;
    float price = 500.0f;           // shipyard price (Phase 8a outfitting)
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

// A drawable model (Phase 9): the cooked mesh and texture the renderer binds,
// and the figures the sim measures the thing by. Data rather than an enum
// because until this existed the whole set was a five-member C++ enum behind
// four hardcoded switches - so a mesh could be authored and cooked and still
// have no way of reaching the game. `[[model]]` rows are what the authoring
// tool writes; the def order is the runtime model index.
struct ModelDef
{
    std::string id;
    std::string mesh;    // cooked file stem, e.g. "ship" -> ship.smesh
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

class DefDatabase
{
public:
    void clear();

    // Merges one TOML document (any mix of [[ship]]/[[weapon]]/[[faction]]).
    // sourceName appears in errors and def provenance. On error the database
    // is left as it was before the call.
    [[nodiscard]] bool mergeToml(const char* text, std::size_t length, const char* sourceName,
                                 std::string* outError = nullptr);

    // Reads and merges every *.toml directly inside directory, sorted by
    // path for determinism. A missing directory is fine (a mod without data).
    [[nodiscard]] bool mergeDirectory(const char* directory, std::string* outError = nullptr);

    // Cross-def checks that need the fully merged database: a faction pair
    // declaring initial relations from both sides must agree (Phase 8b).
    // Relations naming unknown faction ids are ignored downstream, not here —
    // a mod may remove a faction another def still references.
    [[nodiscard]] bool validateFactions(std::string* outError = nullptr) const;

    [[nodiscard]] const ShipDef* findShip(const char* id) const;
    [[nodiscard]] const WeaponDef* findWeapon(const char* id) const;
    [[nodiscard]] const FactionDef* findFaction(const char* id) const;
    [[nodiscard]] const CommodityDef* findCommodity(const char* id) const;
    [[nodiscard]] const StationDef* findStation(const char* id) const;
    [[nodiscard]] const ModuleDef* findModule(const char* id) const;
    [[nodiscard]] const CrewDef* findCrew(const char* id) const;
    [[nodiscard]] const SoundDef* findSound(const char* id) const;
    [[nodiscard]] const ModelDef* findModel(const char* id) const;

    // Index of a model by id, or kNoModel. The renderer and the sim both key
    // off this index rather than off the string, so a name is resolved once at
    // spawn and never in a per-tick loop.
    static constexpr std::uint32_t kNoModel = 0xFFFFFFFFu;
    [[nodiscard]] std::uint32_t modelIndex(const char* id) const;

    // First-definition order; later layers replace elements in place, so
    // indices stay stable across a reload that only edits values.
    [[nodiscard]] const std::vector<ShipDef>& ships() const { return m_ships; }
    [[nodiscard]] const std::vector<WeaponDef>& weapons() const { return m_weapons; }
    [[nodiscard]] const std::vector<FactionDef>& factions() const { return m_factions; }
    [[nodiscard]] const std::vector<CommodityDef>& commodities() const { return m_commodities; }
    [[nodiscard]] const std::vector<StationDef>& stations() const { return m_stations; }
    [[nodiscard]] const std::vector<ModuleDef>& modules() const { return m_modules; }
    [[nodiscard]] const std::vector<CrewDef>& crew() const { return m_crew; }
    [[nodiscard]] const std::vector<SoundDef>& sounds() const { return m_sounds; }
    [[nodiscard]] const std::vector<ModelDef>& models() const { return m_models; }

private:
    std::vector<ShipDef> m_ships;
    std::vector<WeaponDef> m_weapons;
    std::vector<FactionDef> m_factions;
    std::vector<CommodityDef> m_commodities;
    std::vector<StationDef> m_stations;
    std::vector<ModuleDef> m_modules;
    std::vector<CrewDef> m_crew;
    std::vector<SoundDef> m_sounds;
    std::vector<ModelDef> m_models;
};

} // namespace sol::assets
