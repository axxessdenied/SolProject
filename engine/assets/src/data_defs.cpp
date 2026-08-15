#include "sol/assets/data_defs.hpp"

#include "sol/core/toml.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace sol::assets {

using core::TomlValue;

namespace {

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

    // Strict schema: any key outside the allowed set is an error.
    void rejectUnknownKeys(std::initializer_list<const char*> allowed)
    {
        for (const auto& [key, value] : table.members()) {
            const bool known = std::any_of(allowed.begin(), allowed.end(),
                                           [&](const char* k) { return key == k; });
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

    reader.rejectUnknownKeys({"id", "name", "model", "scale", "forward_accel", "reverse_accel",
                              "lateral_accel", "vertical_accel", "max_speed", "max_turn_rate",
                              "angular_accel", "boost_accel_scale", "boost_speed_scale",
                              "cruise_speed_scale", "cruise_accel_scale"});
    if (!reader.failed && def.scale <= 0.0f) {
        reader.fail("'scale' must be > 0");
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

    reader.rejectUnknownKeys(
        {"id", "name", "kind", "damage", "rate_of_fire", "range", "projectile_speed"});
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

} // namespace

void DefDatabase::clear()
{
    m_ships.clear();
    m_weapons.clear();
    m_factions.clear();
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
        } else {
            if (outError != nullptr) {
                *outError = std::string(sourceName) + ": unknown def kind '" + key +
                            "' (expected ship, weapon, or faction)";
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

} // namespace sol::assets
