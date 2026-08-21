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
    "scan_range",     "scan_speed",  "collector_range",
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

    void optionalStringList(const char* key, std::vector<std::string>& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isArray()) {
            fail(std::string("key '") + key + "' must be an array of strings");
            return;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            const TomlValue& element = (*value)[i];
            if (!element.isString()) {
                fail(std::string("key '") + key + "' must be an array of strings");
                return;
            }
            out.push_back(element.asString());
        }
    }

    // "station_id:multiplier" strings, multiplier >= 0 (Phase 13). Zero is a
    // legal, deliberate "this faction never builds one" — which is why the
    // bound is >= 0 rather than > 0.
    void optionalBiasList(const char* key, std::vector<StationBias>& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isArray()) {
            fail(std::string("key '") + key + "' must be an array of \"id:weight\" strings");
            return;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            const TomlValue& element = (*value)[i];
            if (!element.isString()) {
                fail(std::string("key '") + key + "' must be an array of \"id:weight\" strings");
                return;
            }
            const std::string& text = element.asString();
            const std::size_t colon = text.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
                fail(std::string("'") + key + "' entry '" + text + "' is not \"id:weight\"");
                return;
            }
            StationBias bias;
            bias.stationId = text.substr(0, colon);
            char* end = nullptr;
            bias.weight = std::strtof(text.c_str() + colon + 1, &end);
            if (end != text.c_str() + text.size() || bias.weight < 0.0f) {
                fail(std::string("'") + key + "' entry '" + text +
                     "' needs a non-negative numeric weight");
                return;
            }
            out.push_back(std::move(bias));
        }
    }

    // "faction_id:standing" strings, standing in [-100, 100] (Phase 8b
    // relations; unlike rate lists, negative values are the point).
    void optionalRelationList(const char* key, std::vector<FactionRelation>& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isArray()) {
            fail(std::string("key '") + key + "' must be an array of \"id:standing\" strings");
            return;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            const TomlValue& element = (*value)[i];
            if (!element.isString()) {
                fail(std::string("key '") + key + "' must be an array of \"id:standing\" strings");
                return;
            }
            const std::string& text = element.asString();
            const std::size_t colon = text.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
                fail(std::string("'") + key + "' entry '" + text + "' is not \"id:standing\"");
                return;
            }
            FactionRelation relation;
            relation.otherId = text.substr(0, colon);
            char* end = nullptr;
            relation.standing = std::strtof(text.c_str() + colon + 1, &end);
            if (end != text.c_str() + text.size() || relation.standing < -100.0f ||
                relation.standing > 100.0f) {
                fail(std::string("'") + key + "' entry '" + text +
                     "' needs a numeric standing in [-100, 100]");
                return;
            }
            out.push_back(std::move(relation));
        }
    }

    // The shared catalog gate keys on sellable defs (Phase 8b).
    void optionalGate(CatalogGate& out)
    {
        optionalStringList("factions", out.factions);
        optionalFloat("min_rep", out.minRep);
        if (!failed && (out.minRep < -100.0f || out.minRep > 100.0f)) {
            fail("'min_rep' must be in [-100, 100]");
        }
    }

    void optionalBool(const char* key, bool& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isBool()) {
            fail(std::string("key '") + key + "' must be true or false");
            return;
        }
        out = value->asBool();
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
    reader.optionalFloat("scan_range", def.scanRange);
    reader.optionalFloat("scan_speed", def.scanSpeed);
    reader.optionalFloat("collector_range", def.collectorRange);

    reader.optionalFloat("price", def.price);
    reader.optionalFloat("mass", def.mass);
    reader.optionalFloat("power_output", def.powerOutput);
    reader.optionalUint("slots_shield", def.slotsShield);
    reader.optionalUint("slots_engine", def.slotsEngine);
    reader.optionalUint("slots_cargo", def.slotsCargo);
    reader.optionalUint("slots_utility", def.slotsUtility);
    reader.optionalUint("crew_berths", def.crewBerths);
    reader.optionalGate(def.gate);

    reader.rejectUnknownKeys({"id", "name", "model", "scale", "forward_accel", "reverse_accel",
                              "lateral_accel", "vertical_accel", "max_speed", "max_turn_rate",
                              "angular_accel", "boost_accel_scale", "boost_speed_scale",
                              "cruise_speed_scale", "cruise_accel_scale", "shield_strength",
                              "shield_regen", "shield_regen_delay", "armor", "hull",
                              "weapon_capacitor", "weapon_recharge", "weapon", "cargo",
                              "scan_range", "scan_speed", "collector_range", "price",
                              "mass", "power_output", "slots_shield", "slots_engine",
                              "slots_cargo", "slots_utility", "crew_berths", "factions",
                              "min_rep"});
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
    reader.optionalFloat("mining_power", def.miningPower);
    reader.optionalFloat("price", def.price);
    reader.optionalString("model", def.model);
    reader.optionalGate(def.gate);

    reader.rejectUnknownKeys({"id", "name", "kind", "damage", "rate_of_fire", "range",
                              "projectile_speed", "energy_cost", "mining_power", "price",
                              "model", "factions", "min_rep"});
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
    std::string kind = "major";
    reader.optionalString("kind", kind);
    reader.optionalFloat("aggression", def.aggression);
    reader.optionalFloat("forgiveness", def.forgiveness);
    reader.optionalRelationList("relations", def.relations);
    reader.optionalStringList("ships_patrol", def.shipsPatrol);
    reader.optionalStringList("ships_raider", def.shipsRaider);
    reader.optionalStringList("ships_trader", def.shipsTrader);
    reader.optionalBiasList("station_bias", def.stationBias);

    reader.rejectUnknownKeys({"id", "name", "description", "color", "kind", "aggression",
                              "forgiveness", "relations", "ships_patrol", "ships_raider",
                              "ships_trader", "station_bias"});
    if (!reader.failed) {
        if (kind == "major") {
            def.kind = FactionKind::Major;
        } else if (kind == "pirate") {
            def.kind = FactionKind::Pirate;
        } else {
            reader.fail("'kind' must be \"major\" or \"pirate\"");
        }
    }
    if (!reader.failed && (def.aggression < 0.0f || def.aggression > 1.0f ||
                           def.forgiveness < 0.0f || def.forgiveness > 1.0f)) {
        reader.fail("'aggression' and 'forgiveness' must be in [0, 1]");
    }
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
    reader.optionalFloat("ore_weight_core", def.oreWeightCore);
    reader.optionalFloat("ore_weight_frontier", def.oreWeightFrontier);
    reader.optionalFloat("ore_weight_fringe", def.oreWeightFringe);
    reader.optionalString("model", def.model);
    reader.optionalString("chunk_model", def.chunkModel);

    reader.rejectUnknownKeys({"id", "name", "base_price", "ore_weight_core", "ore_weight_frontier",
                              "ore_weight_fringe", "model", "chunk_model"});
    if (!reader.failed && def.basePrice <= 0.0f) {
        reader.fail("'base_price' must be > 0");
    }
    if (!reader.failed
        && (def.oreWeightCore < 0.0f || def.oreWeightFrontier < 0.0f
            || def.oreWeightFringe < 0.0f)) {
        reader.fail("'ore_weight_*' must be >= 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseSound(const TomlValue& table, const char* sourceName, std::vector<SoundDef>& out,
                std::string* outError)
{
    SoundDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": sound '" + def.id + "'";
    }
    reader.requireString("asset", def.asset);
    reader.optionalFloat("gain", def.gain);
    reader.optionalFloat("pitch_jitter", def.pitchJitter);
    reader.optionalFloat("rolloff", def.rolloff);
    reader.optionalUint("max_instances", def.maxInstances);

    reader.rejectUnknownKeys({"id", "asset", "gain", "pitch_jitter", "rolloff", "max_instances"});
    if (!reader.failed && def.gain < 0.0f) {
        reader.fail("'gain' must be >= 0");
    }
    // Jitter is a fraction of the playback rate; half is already a musical
    // fifth either way, and past 1.0 the rate would reach zero.
    if (!reader.failed && (def.pitchJitter < 0.0f || def.pitchJitter > 0.5f)) {
        reader.fail("'pitch_jitter' must be within [0, 0.5]");
    }
    if (!reader.failed && def.rolloff <= 0.0f) {
        reader.fail("'rolloff' must be > 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseModel(const TomlValue& table, const char* sourceName, std::vector<ModelDef>& out,
                std::string* outError)
{
    ModelDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": model '" + def.id + "'";
    }
    reader.requireString("mesh", def.mesh);
    reader.requireString("texture", def.texture);
    reader.optionalFloat("radius", def.radius);
    reader.optionalFloat("avoid_radius", def.avoidRadius);
    reader.optionalFloat("emissive", def.emissive);
    reader.optionalBool("solid", def.solid);
    reader.optionalBool("translucent", def.translucent);
    reader.optionalFloat("alpha", def.alpha);

    reader.rejectUnknownKeys({"id", "mesh", "texture", "radius", "avoid_radius", "emissive",
                              "solid", "translucent", "alpha"});
    if (!reader.failed && def.radius <= 0.0f) {
        reader.fail("'radius' must be > 0");
    }
    // Coverage outside 0..1 is meaningless under premultiplied blending and
    // would read as a wrongly-lit model rather than as a bad number, so it is
    // rejected at load where the file name is still in hand.
    if (!reader.failed && (def.alpha < 0.0f || def.alpha > 1.0f)) {
        reader.fail("'alpha' must be between 0 and 1");
    }
    // 0 is the sentinel for "same as radius"; anything positive but smaller
    // would put the avoidance sphere inside the collision sphere, i.e. steering
    // that clears the obstacle it is about to hit. Larger is always safe.
    if (!reader.failed && def.avoidRadius != 0.0f && def.avoidRadius < def.radius) {
        reader.fail("'avoid_radius' must be 0 (meaning 'same as radius') or >= 'radius'");
    }
    if (!reader.failed && def.emissive < 0.0f) {
        reader.fail("'emissive' must be >= 0");
    }
    if (reader.failed) {
        return false;
    }
    if (def.avoidRadius == 0.0f) {
        def.avoidRadius = def.radius;
    }
    mergeDef(out, std::move(def));
    return true;
}

// A `[[role]]` row (Phase 19). Two keys and no validation beyond the schema:
// whether the model exists, and whether the role is one the engine asks for,
// are both CROSS-def questions that need the merged database - a role may
// legitimately be defined in an earlier layer than the model that fills it.
// `validateRoles` is where those are answered, which is the same split
// `validateFactions` already draws.
bool parseRole(const TomlValue& table, const char* sourceName, std::vector<RoleDef>& out,
               std::string* outError)
{
    RoleDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": role '" + def.id + "'";
    }
    reader.requireString("model", def.model);
    reader.rejectUnknownKeys({"id", "model"});
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
    reader.optionalString("model", def.model);
    reader.optionalFloat("weight_core", def.weightCore);
    reader.optionalFloat("weight_frontier", def.weightFrontier);
    reader.optionalFloat("weight_fringe", def.weightFringe);
    reader.optionalRateList("produces", def.produces);
    reader.optionalRateList("consumes", def.consumes);
    reader.optionalRateList("feedstock", def.feedstock);
    reader.optionalString("produces_from", def.producesFrom);
    reader.optionalFloat("stock_capacity", def.stockCapacity);
    reader.optionalString("refine_input", def.refineInput);
    reader.optionalString("refine_output", def.refineOutput);

    reader.rejectUnknownKeys({"id", "name", "model", "weight_core", "weight_frontier",
                              "weight_fringe", "produces", "consumes", "feedstock",
                              "produces_from", "stock_capacity", "refine_input",
                              "refine_output"});
    if (!reader.failed && def.model.empty()) {
        reader.fail("'model' must name a [[model]] row when given");
    }
    if (!reader.failed && def.stockCapacity <= 0.0f) {
        reader.fail("'stock_capacity' must be > 0");
    }
    if (!reader.failed && def.refineInput.empty() != def.refineOutput.empty()) {
        reader.fail("'refine_input' and 'refine_output' must be given together");
    }
    if (!reader.failed && !def.producesFrom.empty() && def.producesFrom != "field") {
        reader.fail("'produces_from' must be \"field\" when given");
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
    reader.optionalGate(def.gate);

    reader.rejectUnknownKeys({"id", "name", "slot", "price", "mass", "power_draw", "factions",
                              "min_rep"},
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
    reader.optionalGate(def.gate);

    reader.rejectUnknownKeys({"id", "name", "role", "price", "factions", "min_rep"},
                             /*allowModifiers=*/true);
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
    m_sounds.clear();
    m_models.clear();
    m_roles.clear();
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
    std::vector<SoundDef> sounds = m_sounds;
    std::vector<ModelDef> models = m_models;
    std::vector<RoleDef> roles = m_roles;

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
        } else if (key == "sound") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseSound(t, s, *static_cast<std::vector<SoundDef>*>(v), e);
            };
            target = &sounds;
        } else if (key == "model") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseModel(t, s, *static_cast<std::vector<ModelDef>*>(v), e);
            };
            target = &models;
        } else if (key == "role") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseRole(t, s, *static_cast<std::vector<RoleDef>*>(v), e);
            };
            target = &roles;
        } else {
            if (outError != nullptr) {
                *outError = std::string(sourceName) + ": unknown def kind '" + key +
                            "' (expected ship, weapon, faction, commodity, station, module, "
                            "crew, sound, model, or role)";
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
    m_sounds = std::move(sounds);
    m_models = std::move(models);
    m_roles = std::move(roles);
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

bool DefDatabase::validateFactions(std::string* outError) const
{
    for (const FactionDef& faction : m_factions) {
        for (const FactionRelation& relation : faction.relations) {
            const FactionDef* other = findFaction(relation.otherId.c_str());
            if (other == nullptr) {
                continue; // unknown ids are downstream warnings, not errors
            }
            for (const FactionRelation& mirrored : other->relations) {
                if (mirrored.otherId == faction.id &&
                    mirrored.standing != relation.standing) {
                    if (outError != nullptr) {
                        *outError = "factions '" + faction.id + "' and '" + other->id +
                                    "' declare mismatched relations (" +
                                    std::to_string(relation.standing) + " vs " +
                                    std::to_string(mirrored.standing) + ")";
                    }
                    return false;
                }
            }
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

const SoundDef* DefDatabase::findSound(const char* id) const
{
    const auto it =
        std::find_if(m_sounds.begin(), m_sounds.end(), [&](const SoundDef& d) { return d.id == id; });
    return it != m_sounds.end() ? &*it : nullptr;
}

const ModelDef* DefDatabase::findModel(const char* id) const
{
    const std::uint32_t index = modelIndex(id);
    return index == kNoModel ? nullptr : &m_models[index];
}

std::uint32_t DefDatabase::modelIndex(const char* id) const
{
    for (std::size_t i = 0; i < m_models.size(); ++i) {
        if (m_models[i].id == id) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNoModel;
}

const RoleDef* DefDatabase::findRole(const char* id) const
{
    const auto it =
        std::find_if(m_roles.begin(), m_roles.end(), [&](const RoleDef& d) { return d.id == id; });
    return it != m_roles.end() ? &*it : nullptr;
}

std::uint32_t DefDatabase::roleModelIndex(const char* id) const
{
    const RoleDef* role = findRole(id);
    return role == nullptr ? kNoModel : modelIndex(role->model.c_str());
}

bool DefDatabase::validateRoles(std::span<const char* const> required,
                                std::string* outError) const
{
    // Every role the caller asks for must be filled, and filled by a model
    // that exists. Both are refusals rather than warnings - see the header.
    for (const char* id : required) {
        const RoleDef* role = findRole(id);
        if (role == nullptr) {
            if (outError != nullptr) {
                *outError = std::string("no [[role]] row for '") + id +
                            "'; every slot the engine draws must be filled by data";
            }
            return false;
        }
        if (modelIndex(role->model.c_str()) == kNoModel) {
            if (outError != nullptr) {
                *outError = role->source + ": role '" + role->id + "' names model '" +
                            role->model + "', which no [[model]] row defines";
            }
            return false;
        }
    }
    // A role OUTSIDE the caller's vocabulary is a typo that would otherwise do
    // nothing at all, quietly, forever - the failure mode a strict schema
    // exists to prevent, so it is rejected the same way an unknown key is.
    for (const RoleDef& role : m_roles) {
        const bool known = std::any_of(required.begin(), required.end(),
                                       [&](const char* id) { return role.id == id; });
        if (!known) {
            if (outError != nullptr) {
                *outError = role.source + ": unknown role '" + role.id +
                            "'; the engine draws no such thing";
            }
            return false;
        }
    }
    return true;
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
