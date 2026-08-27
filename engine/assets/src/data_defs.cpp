#include "sol/assets/data_defs.hpp"

#include "sol/core/toml.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace sol::assets {

using core::TomlValue;

namespace {

// TOML key stems for the FitStat enum, in enum order (Phase 8a modifiers:
// "<stem>_add" / "<stem>_mul" on modules and crew).
constexpr const char* kFitStatKeys[kFitStatCount] = {
    "forward_accel",
    "reverse_accel",
    "lateral_accel",
    "vertical_accel",
    "max_speed",
    "turn_rate",
    "cruise_speed_scale",
    "shield_strength",
    "shield_regen",
    "armor",
    "hull",
    "weapon_capacitor",
    "weapon_recharge",
    "cargo",
    "scan_range",
    "scan_speed",
    "collector_range",
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
                fail(std::string("'") + key + "' entry '" + text + "' needs a non-negative numeric rate");
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
                fail(std::string("'") + key + "' entry '" + text + "' needs a non-negative numeric weight");
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
    void rejectUnknownKeys(std::initializer_list<const char*> allowed, bool allowModifiers = false)
    {
        for (const auto& [key, value] : table.members()) {
            bool known = std::any_of(allowed.begin(), allowed.end(), [&](const char* k) { return key == k; });
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
    const auto existing =
        std::find_if(defs.begin(), defs.end(), [&](const DefT& d) { return d.id == def.id; });
    if (existing != defs.end()) {
        *existing = std::move(def); // later layer replaces wholesale, in place
    } else {
        defs.push_back(std::move(def));
    }
}

bool parseShip(const TomlValue& table,
               const char* sourceName,
               std::vector<ShipDef>& out,
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
    reader.optionalString("cockpit", def.cockpit);
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

    reader.rejectUnknownKeys({"id",
                              "name",
                              "model",
                              "cockpit",
                              "scale",
                              "forward_accel",
                              "reverse_accel",
                              "lateral_accel",
                              "vertical_accel",
                              "max_speed",
                              "max_turn_rate",
                              "angular_accel",
                              "boost_accel_scale",
                              "boost_speed_scale",
                              "cruise_speed_scale",
                              "cruise_accel_scale",
                              "shield_strength",
                              "shield_regen",
                              "shield_regen_delay",
                              "armor",
                              "hull",
                              "weapon_capacitor",
                              "weapon_recharge",
                              "weapon",
                              "cargo",
                              "scan_range",
                              "scan_speed",
                              "collector_range",
                              "price",
                              "mass",
                              "power_output",
                              "slots_shield",
                              "slots_engine",
                              "slots_cargo",
                              "slots_utility",
                              "crew_berths",
                              "factions",
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

bool parseWeapon(const TomlValue& table,
                 const char* sourceName,
                 std::vector<WeaponDef>& out,
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

    reader.rejectUnknownKeys({"id",
                              "name",
                              "kind",
                              "damage",
                              "rate_of_fire",
                              "range",
                              "projectile_speed",
                              "energy_cost",
                              "mining_power",
                              "price",
                              "model",
                              "factions",
                              "min_rep"});
    if (!reader.failed && def.kind != "projectile" && def.kind != "hitscan") {
        reader.fail("'kind' must be \"projectile\" or \"hitscan\"");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseFaction(const TomlValue& table,
                  const char* sourceName,
                  std::vector<FactionDef>& out,
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

    reader.rejectUnknownKeys({"id",
                              "name",
                              "description",
                              "color",
                              "kind",
                              "aggression",
                              "forgiveness",
                              "relations",
                              "ships_patrol",
                              "ships_raider",
                              "ships_trader",
                              "station_bias"});
    if (!reader.failed) {
        if (kind == "major") {
            def.kind = FactionKind::Major;
        } else if (kind == "pirate") {
            def.kind = FactionKind::Pirate;
        } else {
            reader.fail("'kind' must be \"major\" or \"pirate\"");
        }
    }
    if (!reader.failed && (def.aggression < 0.0f || def.aggression > 1.0f || def.forgiveness < 0.0f ||
                           def.forgiveness > 1.0f)) {
        reader.fail("'aggression' and 'forgiveness' must be in [0, 1]");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseCommodity(const TomlValue& table,
                    const char* sourceName,
                    std::vector<CommodityDef>& out,
                    std::string* outError)
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

    reader.rejectUnknownKeys({"id",
                              "name",
                              "base_price",
                              "ore_weight_core",
                              "ore_weight_frontier",
                              "ore_weight_fringe",
                              "model",
                              "chunk_model"});
    if (!reader.failed && def.basePrice <= 0.0f) {
        reader.fail("'base_price' must be > 0");
    }
    if (!reader.failed &&
        (def.oreWeightCore < 0.0f || def.oreWeightFrontier < 0.0f || def.oreWeightFringe < 0.0f)) {
        reader.fail("'ore_weight_*' must be >= 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseSound(const TomlValue& table,
                const char* sourceName,
                std::vector<SoundDef>& out,
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

// The prefix a synthesised material's id carries, reserved so that the rebuild
// after every merge cannot collide with something a person wrote.
constexpr const char* kSynthesisedMaterialPrefix = "sol.auto.";

bool hasSynthesisedMaterialPrefix(const std::string& id)
{
    const std::string_view prefix{kSynthesisedMaterialPrefix};
    return id.size() >= prefix.size() && id.compare(0, prefix.size(), prefix) == 0;
}

// The four surface keys a `[[model]]` row carried before Phase 25 stage A, and
// which it gives up entirely the moment it names a material. Named once here
// because both the conflict check and the synthesis have to agree on the list.
constexpr const char* kModelSurfaceKeys[] = {"texture", "emissive", "translucent", "alpha"};

// ⚑⚑ PHASE 12's TRANSLUCENT VARIANT, AS DEFAULTS RATHER THAN AS A BRANCH.
// `mesh_renderer.cpp` built it by hand as "the same shaders, layout and vertex
// format with three fields moved"; these are those three fields. Shared by
// `parseMaterial` and by the synthesis in `resolveMaterials` so the two cannot
// drift - a derived material and a hand-written one that say the same thing
// must produce the same pipeline, or stage B's cache is keyed on a lie.
void applyBlendDefaults(MaterialDef& def)
{
    def.blend = def.translucent ? MaterialBlend::Alpha : MaterialBlend::Opaque;
    // No depth write so it does not occlude what is behind it, and no back-face
    // cull because the player flies through it and would otherwise watch it
    // vanish on the way past.
    def.depthWrite = !def.translucent;
    def.cullBackFaces = !def.translucent;
}

// A `[[material]]` row (Phase 25 stage A): how a surface is drawn, split out of
// the model that wears it. Stage B added the shader pair and the pipeline
// state, which is everything `GraphicsPipelineDesc` can be told that this
// engine has a reason to vary.
bool parseMaterial(const TomlValue& table,
                   const char* sourceName,
                   std::vector<MaterialDef>& out,
                   std::string* outError)
{
    MaterialDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": material '" + def.id + "'";
    }
    reader.requireString("texture", def.texture);
    reader.optionalFloat("emissive", def.emissive);
    reader.optionalBool("translucent", def.translucent);
    reader.optionalFloat("alpha", def.alpha);
    reader.optionalString("vertex_shader", def.vertexShader);
    reader.optionalString("fragment_shader", def.fragmentShader);

    // ⚑ ORDER IS LOAD-BEARING: the three state keys are SEEDED from
    // `translucent` and then overridden by whatever the row says explicitly.
    // Reading them before this call would have the defaults silently overwrite
    // the author.
    applyBlendDefaults(def);
    if (!reader.failed) {
        std::string blend;
        reader.optionalString("blend", blend);
        if (!reader.failed && !blend.empty()) {
            if (blend == "opaque") {
                def.blend = MaterialBlend::Opaque;
            } else if (blend == "alpha") {
                def.blend = MaterialBlend::Alpha;
            } else if (blend == "additive") {
                def.blend = MaterialBlend::Additive;
            } else {
                reader.fail("'blend' must be \"opaque\", \"alpha\" or \"additive\"");
            }
        }
    }
    reader.optionalBool("depth_test", def.depthTest);
    reader.optionalBool("depth_write", def.depthWrite);
    reader.optionalBool("cull", def.cullBackFaces);

    reader.rejectUnknownKeys({"id",
                              "texture",
                              "emissive",
                              "translucent",
                              "alpha",
                              "vertex_shader",
                              "fragment_shader",
                              "blend",
                              "depth_test",
                              "depth_write",
                              "cull"});
    // A shader stem reaches the filesystem as "<stem>.vert.spv". An empty one
    // would build that into ".vert.spv" and fail with a path nobody can read
    // back to a row, so it dies here where the file name is still in hand.
    if (!reader.failed && (def.vertexShader.empty() || def.fragmentShader.empty())) {
        reader.fail("'vertex_shader' and 'fragment_shader' name a SPIR-V stem and cannot be empty");
    }
    // ⚑ The synthesis rebuilds its rows from scratch after every merge, so an
    // authored row wearing that prefix would be shadowed by a derived one with
    // the same id - a name that resolves to something the file does not say.
    // Refused where the file name is still in hand instead.
    if (!reader.failed && hasSynthesisedMaterialPrefix(def.id)) {
        reader.fail(std::string("'") + kSynthesisedMaterialPrefix +
                    "' is reserved for materials this engine derives from a [[model]] row");
    }
    if (!reader.failed && (def.alpha < 0.0f || def.alpha > 1.0f)) {
        reader.fail("'alpha' must be between 0 and 1");
    }
    if (!reader.failed && def.emissive < 0.0f) {
        reader.fail("'emissive' must be >= 0");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseModel(const TomlValue& table,
                const char* sourceName,
                std::vector<ModelDef>& out,
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
    // ⚑⚑ PHASE 25 STAGE A: A ROW EITHER NAMES A MATERIAL OR DESCRIBES ONE, AND
    // NEVER BOTH. The alternative - a precedence rule - is invisible in the
    // file, and the file is the whole point: an author reading a row with both
    // has no way to tell which half is doing anything. So `texture` stays
    // required exactly as it was for a row that names no material, and every
    // one of the four surface keys is refused by name for a row that does.
    reader.optionalString("material", def.material);
    if (!reader.failed && table.find("material") != nullptr && def.material.empty()) {
        reader.fail("'material' cannot be empty - omit the key to describe the surface on this row");
    }
    if (!reader.failed && !def.material.empty()) {
        for (const char* key : kModelSurfaceKeys) {
            if (table.find(key) != nullptr) {
                reader.fail(std::string("names material '") + def.material + "' and also sets '" + key +
                            "' - a material owns the surface, so move the key onto the [[material]] row");
                break;
            }
        }
    } else if (!reader.failed) {
        reader.requireString("texture", def.texture);
    }
    reader.optionalFloat("radius", def.radius);
    reader.optionalFloat("avoid_radius", def.avoidRadius);
    reader.optionalFloat("emissive", def.emissive);
    reader.optionalBool("solid", def.solid);
    reader.optionalBool("translucent", def.translucent);
    reader.optionalFloat("alpha", def.alpha);

    reader.rejectUnknownKeys({"id",
                              "mesh",
                              "material",
                              "texture",
                              "radius",
                              "avoid_radius",
                              "emissive",
                              "solid",
                              "translucent",
                              "alpha"});
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
bool parseRole(const TomlValue& table,
               const char* sourceName,
               std::vector<RoleDef>& out,
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

bool parseStation(const TomlValue& table,
                  const char* sourceName,
                  std::vector<StationDef>& out,
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

    reader.rejectUnknownKeys({"id",
                              "name",
                              "model",
                              "weight_core",
                              "weight_frontier",
                              "weight_fringe",
                              "produces",
                              "consumes",
                              "feedstock",
                              "produces_from",
                              "stock_capacity",
                              "refine_input",
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

bool parseModule(const TomlValue& table,
                 const char* sourceName,
                 std::vector<ModuleDef>& out,
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

    reader.rejectUnknownKeys({"id", "name", "slot", "price", "mass", "power_draw", "factions", "min_rep"},
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

bool parseCrew(const TomlValue& table,
               const char* sourceName,
               std::vector<CrewDef>& out,
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
    m_materials.clear();
    m_roles.clear();
}

bool DefDatabase::mergeToml(const char* text,
                            std::size_t length,
                            const char* sourceName,
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
    // ⚑ The synthesised rows are dropped from the staging copy rather than
    // carried, because `resolveMaterials` rebuilds them from the models below
    // and a carried one would be a duplicate. The authored rows are staged like
    // any other def kind.
    std::vector<MaterialDef> materials;
    for (const MaterialDef& material : m_materials) {
        if (!material.synthesised) {
            materials.push_back(material);
        }
    }
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
        } else if (key == "material") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseMaterial(t, s, *static_cast<std::vector<MaterialDef>*>(v), e);
            };
            target = &materials;
        } else if (key == "role") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseRole(t, s, *static_cast<std::vector<RoleDef>*>(v), e);
            };
            target = &roles;
        } else {
            if (outError != nullptr) {
                *outError = std::string(sourceName) + ": unknown def kind '" + key +
                            "' (expected ship, weapon, faction, commodity, station, module, "
                            "crew, sound, model, material, or role)";
            }
            return false;
        }

        if (!value.isArray()) {
            if (outError != nullptr) {
                *outError =
                    std::string(sourceName) + ": '" + key + "' must be an array of tables ([[" + key + "]])";
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
    m_materials = std::move(materials);
    m_roles = std::move(roles);
    // ⚑ At the TAIL of every merge, not as a pass a caller has to remember.
    // The database is therefore never half-resolved, and the one thing that
    // genuinely cannot be answered until every layer is in - whether a named
    // material exists - is left to `validateMaterials`.
    resolveMaterials();
    return true;
}

void DefDatabase::resolveMaterials()
{
    m_materials.erase(std::remove_if(m_materials.begin(),
                                     m_materials.end(),
                                     [](const MaterialDef& m) { return m.synthesised; }),
                      m_materials.end());

    for (ModelDef& model : m_models) {
        if (!model.material.empty()) {
            model.materialIndex = materialIndex(model.material.c_str());
            continue;
        }
        // ⚑⚑ ONE DERIVED ROW PER MODEL, DELIBERATELY NOT DEDUPED BY VALUE.
        // Four of the shipped models would share a single "hull, opaque, unlit"
        // row, and sharing it would put a name in every future error message
        // that points at some OTHER model's row. `sol.auto.<model id>` points
        // at the row the author can actually edit. Materials cost nothing to
        // hold here - there is no UBO and no descriptor set until stage C -
        // and de-duplication is a PIPELINE concern that stage B owns by
        // caching on state rather than on identity.
        MaterialDef derived;
        derived.id = std::string(kSynthesisedMaterialPrefix) + model.id;
        derived.texture = model.texture;
        derived.emissive = model.emissive;
        derived.translucent = model.translucent;
        derived.alpha = model.alpha;
        // ⚑ Stage B: the shader stems keep their defaults (the stock lambert
        // pair), and the three state fields come from the SAME function the
        // parser seeds from - so a derived material and an authored one that
        // say the same thing land on the same cache key, which is the whole
        // basis of pipeline sharing.
        applyBlendDefaults(derived);
        derived.synthesised = true;
        derived.source = model.source;
        model.materialIndex = static_cast<std::uint32_t>(m_materials.size());
        m_materials.push_back(std::move(derived));
    }
}

bool DefDatabase::validateMaterials(std::string* outError) const
{
    for (const ModelDef& model : m_models) {
        if (model.materialIndex != kNoMaterial) {
            continue;
        }
        if (outError != nullptr) {
            *outError = model.source + ": model '" + model.id + "' names material '" + model.material +
                        "', which no [[material]] row defines";
        }
        return false;
    }
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
        if (!mergeToml(reinterpret_cast<const char*>(bytes.data()), bytes.size(), path.c_str(), outError)) {
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
                if (mirrored.otherId == faction.id && mirrored.standing != relation.standing) {
                    if (outError != nullptr) {
                        *outError = "factions '" + faction.id + "' and '" + other->id +
                                    "' declare mismatched relations (" + std::to_string(relation.standing) +
                                    " vs " + std::to_string(mirrored.standing) + ")";
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
    const auto it =
        std::find_if(m_ships.begin(), m_ships.end(), [&](const ShipDef& d) { return d.id == id; });
    return it != m_ships.end() ? &*it : nullptr;
}

const WeaponDef* DefDatabase::findWeapon(const char* id) const
{
    const auto it =
        std::find_if(m_weapons.begin(), m_weapons.end(), [&](const WeaponDef& d) { return d.id == id; });
    return it != m_weapons.end() ? &*it : nullptr;
}

const FactionDef* DefDatabase::findFaction(const char* id) const
{
    const auto it =
        std::find_if(m_factions.begin(), m_factions.end(), [&](const FactionDef& d) { return d.id == id; });
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

const MaterialDef* DefDatabase::findMaterial(const char* id) const
{
    const std::uint32_t index = materialIndex(id);
    return index == kNoMaterial ? nullptr : &m_materials[index];
}

std::uint32_t DefDatabase::materialIndex(const char* id) const
{
    for (std::size_t i = 0; i < m_materials.size(); ++i) {
        if (m_materials[i].id == id) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kNoMaterial;
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

bool DefDatabase::validateRoles(std::span<const char* const> required, std::string* outError) const
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
                *outError = role->source + ": role '" + role->id + "' names model '" + role->model +
                            "', which no [[model]] row defines";
            }
            return false;
        }
    }
    // A role OUTSIDE the caller's vocabulary is a typo that would otherwise do
    // nothing at all, quietly, forever - the failure mode a strict schema
    // exists to prevent, so it is rejected the same way an unknown key is.
    for (const RoleDef& role : m_roles) {
        const bool known =
            std::any_of(required.begin(), required.end(), [&](const char* id) { return role.id == id; });
        if (!known) {
            if (outError != nullptr) {
                *outError = role.source + ": unknown role '" + role.id + "'; the engine draws no such thing";
            }
            return false;
        }
    }
    return true;
}

const CommodityDef* DefDatabase::findCommodity(const char* id) const
{
    const auto it = std::find_if(
        m_commodities.begin(), m_commodities.end(), [&](const CommodityDef& d) { return d.id == id; });
    return it != m_commodities.end() ? &*it : nullptr;
}

const StationDef* DefDatabase::findStation(const char* id) const
{
    const auto it =
        std::find_if(m_stations.begin(), m_stations.end(), [&](const StationDef& d) { return d.id == id; });
    return it != m_stations.end() ? &*it : nullptr;
}

const ModuleDef* DefDatabase::findModule(const char* id) const
{
    const auto it =
        std::find_if(m_modules.begin(), m_modules.end(), [&](const ModuleDef& d) { return d.id == id; });
    return it != m_modules.end() ? &*it : nullptr;
}

const CrewDef* DefDatabase::findCrew(const char* id) const
{
    const auto it = std::find_if(m_crew.begin(), m_crew.end(), [&](const CrewDef& d) { return d.id == id; });
    return it != m_crew.end() ? &*it : nullptr;
}

} // namespace sol::assets
