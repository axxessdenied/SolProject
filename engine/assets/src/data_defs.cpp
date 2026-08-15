#include "sol/assets/data_defs.hpp"

#include "sol/core/toml.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace sol::assets {

using core::TomlValue;

namespace {

// TOML key stems for the FitStat enum, in enum order (Phase 8a modifiers:
// "<stem>_add" / "<stem>_mul" on modules and crew).
constexpr const char* kFitStatKeys[kFitStatCount] = {
    "forward_accel",  "reverse_accel", "lateral_accel",   "vertical_accel", "max_speed",
    "turn_rate",      "cruise_speed_scale", "shield_strength", "shield_regen", "armor",
    "hull",           "weapon_capacitor", "weapon_recharge", "cargo",
};

// Accumulates the first error and short-circuits later reads.
struct FieldReader
{
    const TomlValue& table;
    std::string context; // e.g. "ships.toml: ship 'sol.shuttle'"
    std::string* outError = nullptr;
    bool failed = false;

    void fail(const std::string& message)
    {
        if (!failed && outError != nullptr) {
            *outError = context + ": " + message;
        }
        failed = true;
    }

    void requireString(const char* key, std::string& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            fail(std::string("missing key '") + key + "'");
            return;
        }
        if (!value->isString()) {
            fail(std::string("key '") + key + "' must be a string");
            return;
        }
        out = value->asString();
    }

    void optionalString(const char* key, std::string& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isString()) {
            fail(std::string("key '") + key + "' must be a string");
            return;
        }
        out = value->asString();
    }

    void optionalFloat(const char* key, float& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isFloat() && !value->isInteger()) {
            fail(std::string("key '") + key + "' must be a number");
            return;
        }
        out = static_cast<float>(value->asFloat());
    }

    void optionalFloat3(const char* key, float (&out)[3])
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isArray() || value->size() != 3) {
            fail(std::string("key '") + key + "' must be an array of 3 numbers");
            return;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            const TomlValue& element = (*value)[i];
            if (!element.isFloat() && !element.isInteger()) {
                fail(std::string("key '") + key + "' must be an array of 3 numbers");
                return;
            }
            out[i] = static_cast<float>(element.asFloat());
        }
    }

    // "commodity_id:rate" strings, e.g. produces = ["sol.food:0.5"].
    void optionalRateList(const char* key, std::vector<StationRate>& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isArray()) {
            fail(std::string("key '") + key + "' must be an array of \"id:rate\" strings");
            return;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            const TomlValue& element = (*value)[i];
            if (!element.isString()) {
                fail(std::string("key '") + key + "' must be an array of \"id:rate\" strings");
                return;
            }
            const std::string& text = element.asString();
            const std::size_t colon = text.rfind(':');
            StationRate rate;
            char* end = nullptr;
            if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
                fail(std::string("'") + key + "' entry '" + text + "' is not \"id:rate\"");
                return;
            }
            rate.commodityId = text.substr(0, colon);
            rate.rate = std::strtof(text.c_str() + colon + 1, &end);
            if (end != text.c_str() + text.size() || rate.rate < 0.0f) {
                fail(std::string("'") + key + "' entry '" + text +
                     "' needs a non-negative numeric rate");
                return;
            }
            out.push_back(std::move(rate));
        }
    }

    void optionalUint(const char* key, std::uint32_t& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isInteger() || value->asInteger() < 0) {
            fail(std::string("key '") + key + "' must be a non-negative integer");
            return;
        }
        out = static_cast<std::uint32_t>(value->asInteger());
    }

    // Reads every "<stat>_add"/"<stat>_mul" modifier key (Phase 8a).
    void optionalModifiers(StatModifiers& out)
    {
        for (std::size_t i = 0; i < kFitStatCount; ++i) {
            const std::string addKey = std::string(kFitStatKeys[i]) + "_add";
            const std::string mulKey = std::string(kFitStatKeys[i]) + "_mul";
            optionalFloat(addKey.c_str(), out.add[i]);
            optionalFloat(mulKey.c_str(), out.mul[i]);
        }
    }

    // Strict schema: any key outside the allowed set is an error.
    // allowModifiers additionally accepts the stat modifier vocabulary.
    void rejectUnknownKeys(std::initializer_list<const char*> allowed,
                           bool allowModifiers = false)
    {
        for (const auto& [key, value] : table.members()) {
            bool known = std::any_of(allowed.begin(), allowed.end(),
                                     [&](const char* k) { return key == k; });
            if (!known && allowModifiers) {
                for (std::size_t i = 0; i < kFitStatCount && !known; ++i) {
                    known = key == std::string(kFitStatKeys[i]) + "_add" ||
                            key == std::string(kFitStatKeys[i]) + "_mul";
                }
            }
            if (!known) {
                fail("unknown key '" + key + "'");
                return;
            }
        }
    }
};

template <typename DefT>
void mergeDef(std::vector<DefT>& defs, DefT&& def)
{
    const auto existing = std::find_if(defs.begin(), defs.end(),
                                       [&](const DefT& d) { return d.id == def.id; });
    if (existing != defs.end()) {
        *existing = std::move(def); // later layer replaces wholesale, in place
    } else {
        defs.push_back(std::move(def));
    }
}

bool parseShip(const TomlValue& table, const char* sourceName, std::vector<ShipDef>& out,
               std::string* outError)
{
    ShipDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": ship '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.optionalString("model", def.model);
    reader.optionalFloat("scale", def.scale);

    ShipFlightTuning& flight = def.flight;
    reader.optionalFloat("forward_accel", flight.forwardAccel);
    reader.optionalFloat("reverse_accel", flight.reverseAccel);
    reader.optionalFloat("lateral_accel", flight.lateralAccel);
    reader.optionalFloat("vertical_accel", flight.verticalAccel);
    reader.optionalFloat("max_speed", flight.maxSpeed);
    reader.optionalFloat3("max_turn_rate", flight.maxTurnRate);
    reader.optionalFloat3("angular_accel", flight.angularAccel);
    reader.optionalFloat("boost_accel_scale", flight.boostAccelScale);
    reader.optionalFloat("boost_speed_scale", flight.boostSpeedScale);
    reader.optionalFloat("cruise_speed_scale", flight.cruiseSpeedScale);
    reader.optionalFloat("cruise_accel_scale", flight.cruiseAccelScale);

    ShipDefenseTuning& defense = def.defense;
    reader.optionalFloat("shield_strength", defense.shieldStrength);
    reader.optionalFloat("shield_regen", defense.shieldRegen);
    reader.optionalFloat("shield_regen_delay", defense.shieldRegenDelay);
    reader.optionalFloat("armor", defense.armor);
    reader.optionalFloat("hull", defense.hull);

    ShipPowerTuning& power = def.power;
    reader.optionalFloat("weapon_capacitor", power.weaponCapacitor);
    reader.optionalFloat("weapon_recharge", power.weaponRecharge);
    reader.optionalString("weapon", def.weaponId);
    reader.optionalFloat("cargo", def.cargoCapacity);

    reader.optionalFloat("price", def.price);
    reader.optionalFloat("mass", def.mass);
    reader.optionalFloat("power_output", def.powerOutput);
    reader.optionalUint("slots_shield", def.slotsShield);
    reader.optionalUint("slots_engine", def.slotsEngine);
    reader.optionalUint("slots_cargo", def.slotsCargo);
    reader.optionalUint("slots_utility", def.slotsUtility);
    reader.optionalUint("crew_berths", def.crewBerths);

    reader.rejectUnknownKeys({"id", "name", "model", "scale", "forward_accel", "reverse_accel",
                              "lateral_accel", "vertical_accel", "max_speed", "max_turn_rate",
                              "angular_accel", "boost_accel_scale", "boost_speed_scale",
                              "cruise_speed_scale", "cruise_accel_scale", "shield_strength",
                              "shield_regen", "shield_regen_delay", "armor", "hull",
                              "weapon_capacitor", "weapon_recharge", "weapon", "cargo", "price",
                              "mass", "power_output", "slots_shield", "slots_engine",
                              "slots_cargo", "slots_utility", "crew_berths"});
    if (!reader.failed && def.scale <= 0.0f) {
        reader.fail("'scale' must be > 0");
    }
    if (!reader.failed && def.mass <= 0.0f) {
        reader.fail("'mass' must be > 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseWeapon(const TomlValue& table, const char* sourceName, std::vector<WeaponDef>& out,
                 std::string* outError)
{
    WeaponDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": weapon '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.requireString("kind", def.kind);
    reader.optionalFloat("damage", def.damage);
    reader.optionalFloat("rate_of_fire", def.rateOfFire);
    reader.optionalFloat("range", def.range);
    reader.optionalFloat("projectile_speed", def.projectileSpeed);
    reader.optionalFloat("energy_cost", def.energyCost);
    reader.optionalFloat("price", def.price);

    reader.rejectUnknownKeys({"id", "name", "kind", "damage", "rate_of_fire", "range",
                              "projectile_speed", "energy_cost", "price"});
    if (!reader.failed && def.kind != "projectile" && def.kind != "hitscan") {
        reader.fail("'kind' must be \"projectile\" or \"hitscan\"");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseFaction(const TomlValue& table, const char* sourceName, std::vector<FactionDef>& out,
                  std::string* outError)
{
    FactionDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": faction '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.optionalString("description", def.description);
    reader.optionalFloat3("color", def.color);

    reader.rejectUnknownKeys({"id", "name", "description", "color"});
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseCommodity(const TomlValue& table, const char* sourceName,
                    std::vector<CommodityDef>& out, std::string* outError)
{
    CommodityDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": commodity '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.optionalFloat("base_price", def.basePrice);

    reader.rejectUnknownKeys({"id", "name", "base_price"});
    if (!reader.failed && def.basePrice <= 0.0f) {
        reader.fail("'base_price' must be > 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseStation(const TomlValue& table, const char* sourceName, std::vector<StationDef>& out,
                  std::string* outError)
{
    StationDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": station '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.optionalFloat("weight_core", def.weightCore);
    reader.optionalFloat("weight_frontier", def.weightFrontier);
    reader.optionalFloat("weight_fringe", def.weightFringe);
    reader.optionalRateList("produces", def.produces);
    reader.optionalRateList("consumes", def.consumes);
    reader.optionalFloat("stock_capacity", def.stockCapacity);

    reader.rejectUnknownKeys({"id", "name", "weight_core", "weight_frontier", "weight_fringe",
                              "produces", "consumes", "stock_capacity"});
    if (!reader.failed && def.stockCapacity <= 0.0f) {
        reader.fail("'stock_capacity' must be > 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseModule(const TomlValue& table, const char* sourceName, std::vector<ModuleDef>& out,
                 std::string* outError)
{
    ModuleDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": module '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    std::string slot;
    reader.requireString("slot", slot);
    reader.optionalFloat("price", def.price);
    reader.optionalFloat("mass", def.mass);
    reader.optionalFloat("power_draw", def.powerDraw);
    reader.optionalModifiers(def.modifiers);

    reader.rejectUnknownKeys({"id", "name", "slot", "price", "mass", "power_draw"},
                             /*allowModifiers=*/true);
    if (!reader.failed) {
        if (slot == "shield") {
            def.slot = ModuleSlot::Shield;
        } else if (slot == "engine") {
            def.slot = ModuleSlot::Engine;
        } else if (slot == "cargo") {
            def.slot = ModuleSlot::Cargo;
        } else if (slot == "utility") {
            def.slot = ModuleSlot::Utility;
        } else {
            reader.fail("'slot' must be \"shield\", \"engine\", \"cargo\", or \"utility\"");
        }
    }
    if (!reader.failed && (def.mass < 0.0f || def.powerDraw < 0.0f)) {
        reader.fail("'mass' and 'power_draw' must be >= 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseCrew(const TomlValue& table, const char* sourceName, std::vector<CrewDef>& out,
               std::string* outError)
{
    CrewDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": crew '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.requireString("role", def.role);
    reader.optionalFloat("price", def.price);
    reader.optionalModifiers(def.modifiers);

    reader.rejectUnknownKeys({"id", "name", "role", "price"}, /*allowModifiers=*/true);
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

} // namespace

void DefDatabase::clear()
{
    m_ships.clear();
    m_weapons.clear();
    m_factions.clear();
    m_commodities.clear();
    m_stations.clear();
    m_modules.clear();
    m_crew.clear();
}

bool DefDatabase::mergeToml(const char* text, std::size_t length, const char* sourceName,
                            std::string* outError)
{
    TomlValue root;
    std::string parseError;
    if (!TomlValue::parse(text, length, root, &parseError)) {
        if (outError != nullptr) {
            *outError = std::string(sourceName) + ": " + parseError;
        }
        return false;
    }

    // Stage into copies so a validation error leaves the database untouched.
    std::vector<ShipDef> ships = m_ships;
    std::vector<WeaponDef> weapons = m_weapons;
    std::vector<FactionDef> factions = m_factions;
    std::vector<CommodityDef> commodities = m_commodities;
    std::vector<StationDef> stations = m_stations;
    std::vector<ModuleDef> modules = m_modules;
    std::vector<CrewDef> crew = m_crew;

    for (const auto& [key, value] : root.members()) {
        bool (*parse)(const TomlValue&, const char*, void*, std::string*) = nullptr;
        void* target = nullptr;
        if (key == "ship") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseShip(t, s, *static_cast<std::vector<ShipDef>*>(v), e);
            };
            target = &ships;
        } else if (key == "weapon") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseWeapon(t, s, *static_cast<std::vector<WeaponDef>*>(v), e);
            };
            target = &weapons;
        } else if (key == "faction") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseFaction(t, s, *static_cast<std::vector<FactionDef>*>(v), e);
            };
            target = &factions;
        } else if (key == "commodity") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseCommodity(t, s, *static_cast<std::vector<CommodityDef>*>(v), e);
            };
            target = &commodities;
        } else if (key == "station") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseStation(t, s, *static_cast<std::vector<StationDef>*>(v), e);
            };
            target = &stations;
        } else if (key == "module") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseModule(t, s, *static_cast<std::vector<ModuleDef>*>(v), e);
            };
            target = &modules;
        } else if (key == "crew") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseCrew(t, s, *static_cast<std::vector<CrewDef>*>(v), e);
            };
            target = &crew;
        } else {
            if (outError != nullptr) {
                *outError = std::string(sourceName) + ": unknown def kind '" + key +
                            "' (expected ship, weapon, faction, commodity, station, module, or crew)";
            }
            return false;
        }

        if (!value.isArray()) {
            if (outError != nullptr) {
                *outError = std::string(sourceName) + ": '" + key +
                            "' must be an array of tables ([[" + key + "]])";
            }
            return false;
        }
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (!value[i].isTable() || !parse(value[i], sourceName, target, outError)) {
                return false;
            }
        }
    }

    m_ships = std::move(ships);
    m_weapons = std::move(weapons);
    m_factions = std::move(factions);
    m_commodities = std::move(commodities);
    m_stations = std::move(stations);
    m_modules = std::move(modules);
    m_crew = std::move(crew);
    return true;
}

bool DefDatabase::mergeDirectory(const char* directory, std::string* outError)
{
    std::vector<std::string> paths = platform::listFiles(directory);
    std::sort(paths.begin(), paths.end());
    for (const std::string& path : paths) {
        if (path.size() < 5 || path.compare(path.size() - 5, 5, ".toml") != 0) {
            continue;
        }
        std::vector<std::uint8_t> bytes;
        if (!platform::readFileBytes(path.c_str(), bytes)) {
            if (outError != nullptr) {
                *outError = "cannot read " + path;
            }
            return false;
        }
        if (!mergeToml(reinterpret_cast<const char*>(bytes.data()), bytes.size(), path.c_str(),
                       outError)) {
            return false;
        }
    }
    return true;
}

const ShipDef* DefDatabase::findShip(const char* id) const
{
    const auto it = std::find_if(m_ships.begin(), m_ships.end(),
                                 [&](const ShipDef& d) { return d.id == id; });
    return it != m_ships.end() ? &*it : nullptr;
}

const WeaponDef* DefDatabase::findWeapon(const char* id) const
{
    const auto it = std::find_if(m_weapons.begin(), m_weapons.end(),
                                 [&](const WeaponDef& d) { return d.id == id; });
    return it != m_weapons.end() ? &*it : nullptr;
}

const FactionDef* DefDatabase::findFaction(const char* id) const
{
    const auto it = std::find_if(m_factions.begin(), m_factions.end(),
                                 [&](const FactionDef& d) { return d.id == id; });
    return it != m_factions.end() ? &*it : nullptr;
}

const CommodityDef* DefDatabase::findCommodity(const char* id) const
{
    const auto it = std::find_if(m_commodities.begin(), m_commodities.end(),
                                 [&](const CommodityDef& d) { return d.id == id; });
    return it != m_commodities.end() ? &*it : nullptr;
}

const StationDef* DefDatabase::findStation(const char* id) const
{
    const auto it = std::find_if(m_stations.begin(), m_stations.end(),
                                 [&](const StationDef& d) { return d.id == id; });
    return it != m_stations.end() ? &*it : nullptr;
}

const ModuleDef* DefDatabase::findModule(const char* id) const
{
    const auto it = std::find_if(m_modules.begin(), m_modules.end(),
                                 [&](const ModuleDef& d) { return d.id == id; });
    return it != m_modules.end() ? &*it : nullptr;
}

const CrewDef* DefDatabase::findCrew(const char* id) const
{
    const auto it = std::find_if(m_crew.begin(), m_crew.end(),
                                 [&](const CrewDef& d) { return d.id == id; });
    return it != m_crew.end() ? &*it : nullptr;
}

} // namespace sol::assets
