#pragma once

// Human-authored game data definitions (engine plan Phase 5): ships, weapons
// (defs only until Phase 6), and factions, parsed from TOML and validated
// against a strict schema (unknown or mistyped keys are errors, so typos die
// at load time, not silently at play time). Documents merge in layer order —
// a def re-using an earlier id replaces it wholesale, which is what gives mod
// directory layering (base game = mod zero) its override semantics.
//
// Defs use flat snake_case keys inside [[ship]] / [[weapon]] / [[faction]]
// array-of-table elements; see game/data/ for the base-game examples.

#include <cstddef>
#include <string>
#include <vector>

namespace sol::assets {

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

struct ShipDef
{
    std::string id;   // stable, namespaced, e.g. "sol.shuttle"
    std::string name; // display name
    std::string model = "ship";
    float scale = 1.0f;
    ShipFlightTuning flight;
    ShipDefenseTuning defense;
    ShipPowerTuning power;
    std::string weaponId; // weapon def id; empty = unarmed
    std::string source;   // document that last defined this id (diagnostics)
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
    std::string source;
};

struct FactionDef
{
    std::string id;
    std::string name;
    std::string description;
    float color[3] = {1.0f, 1.0f, 1.0f}; // sRGB accent for HUD/markers
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

    [[nodiscard]] const ShipDef* findShip(const char* id) const;
    [[nodiscard]] const WeaponDef* findWeapon(const char* id) const;
    [[nodiscard]] const FactionDef* findFaction(const char* id) const;

    // First-definition order; later layers replace elements in place, so
    // indices stay stable across a reload that only edits values.
    [[nodiscard]] const std::vector<ShipDef>& ships() const { return m_ships; }
    [[nodiscard]] const std::vector<WeaponDef>& weapons() const { return m_weapons; }
    [[nodiscard]] const std::vector<FactionDef>& factions() const { return m_factions; }

private:
    std::vector<ShipDef> m_ships;
    std::vector<WeaponDef> m_weapons;
    std::vector<FactionDef> m_factions;
};

} // namespace sol::assets
