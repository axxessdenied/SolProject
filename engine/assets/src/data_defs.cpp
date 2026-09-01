#include "sol/assets/data_defs.hpp"

#include "sol/core/toml.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

namespace sol::assets {

using core::TomlValue;

namespace {

// TOML key stems for the FitStat enum, in enum order (Phase 8a modifiers:
// "<stem>_add" / "<stem>_mul" on components and crew).
//
// ⚑⚑ SIZED BY THE INITIALISER AND CHECKED AGAINST THE ENUM, which is the rule
// `kMountKindNames` already wrote down further down this file and which this
// table was NOT obeying. `[kFitStatCount]` accepts a SHORT list and leaves a
// null pointer at the end - and every reader below does `std::string(key) +
// "_add"`, so adding a member to `FitStat` and forgetting its spelling was
// undefined behaviour reached through an enum edit. Phase 36 stage E is the
// first entry added since Phase 8f and it found the shape rather than the bug.
constexpr const char* kFitStatKeys[] = {
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
    "signature",
};

static_assert(std::size(kFitStatKeys) == kFitStatCount, "a fit stat is missing its def spelling");

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

    // ⚑ The optional `present` flag exists for Phase 29 and is the whole of
    // decision 2: an authored field has to record that the author WROTE it,
    // because two of the generator's fields have no spare sentinel and
    // comparing against a default cannot tell "unset" from a real value.
    void optionalString(const char* key, std::string& out, bool* present = nullptr)
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
        if (present != nullptr) {
            *present = true;
        }
    }

    void optionalFloat(const char* key, float& out, bool* present = nullptr)
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
        if (present != nullptr) {
            *present = true;
        }
    }

    // Metres, and a station or a planet radius is a number a float cannot hold
    // to the digit an author wrote, so authored distances read as double.
    void optionalDouble(const char* key, double& out, bool* present = nullptr)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isFloat() && !value->isInteger()) {
            fail(std::string("key '") + key + "' must be a number");
            return;
        }
        out = value->asFloat();
        if (present != nullptr) {
            *present = true;
        }
    }

    // ⚑ `present` joined this one in Phase 31 stage A2 for the same reason it
    // exists on the scalars: a mount's `at` decides EXTERNAL-or-internal by
    // being written at all, and the origin is a position an author may well
    // mean, so comparing against the default cannot tell unset from authored.
    void optionalFloat3(const char* key, float (&out)[3], bool* present = nullptr)
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
        if (present != nullptr) {
            *present = true;
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

    // "module_id" or "module_id:chance" strings, e.g.
    // modules = ["sol.mod_market_floor:0.8"] (Phase 34 stage B). A bare id is
    // chance 1.0 - the common case is a module every station of the archetype
    // has, and writing ":1.0" on every one of those would bury the rows that
    // actually vary.
    void optionalRecipeList(const char* key, std::vector<StationModuleEntry>& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isArray()) {
            fail(std::string("key '") + key + "' must be an array of \"id\" or \"id:chance\" strings");
            return;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            const TomlValue& element = (*value)[i];
            if (!element.isString()) {
                fail(std::string("key '") + key + "' must be an array of \"id\" or \"id:chance\" strings");
                return;
            }
            const std::string& text = element.asString();
            StationModuleEntry entry;
            const std::size_t colon = text.rfind(':');
            if (colon == std::string::npos) {
                entry.moduleId = text;
            } else {
                if (colon == 0 || colon + 1 >= text.size()) {
                    fail(std::string("'") + key + "' entry '" + text + "' is not \"id\" or \"id:chance\"");
                    return;
                }
                entry.moduleId = text.substr(0, colon);
                char* end = nullptr;
                entry.chance = std::strtof(text.c_str() + colon + 1, &end);
                if (end != text.c_str() + text.size() || !(entry.chance > 0.0f) || entry.chance > 1.0f) {
                    fail(std::string("'") + key + "' entry '" + text + "' needs a chance in (0, 1]");
                    return;
                }
            }
            if (entry.moduleId.empty()) {
                fail(std::string("'") + key + "' entry '" + text + "' names no module");
                return;
            }
            // ⚑ One line per module. Two lines for one id would be two chances
            // for one thing, which has no reading the composer could honour -
            // and a module that belongs twice says so in its own numbers.
            for (const StationModuleEntry& seen : out) {
                if (seen.moduleId == entry.moduleId) {
                    fail(std::string("'") + key + "' names '" + entry.moduleId + "' twice");
                    return;
                }
            }
            out.push_back(std::move(entry));
        }
    }

    // "class:capacity" strings, e.g. stores = ["bulk:1200"] (Phase 34 stage A).
    // Sibling of `optionalRateList` above and deliberately the same shape: an
    // author who can read one list can read the other.
    void optionalStorageList(const char* key, std::vector<ModuleStorage>& out)
    {
        const TomlValue* value = table.find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->isArray()) {
            fail(std::string("key '") + key + "' must be an array of \"class:capacity\" strings");
            return;
        }
        for (std::size_t i = 0; i < value->size(); ++i) {
            const TomlValue& element = (*value)[i];
            if (!element.isString()) {
                fail(std::string("key '") + key + "' must be an array of \"class:capacity\" strings");
                return;
            }
            const std::string& text = element.asString();
            const std::size_t colon = text.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
                fail(std::string("'") + key + "' entry '" + text + "' is not \"class:capacity\"");
                return;
            }
            ModuleStorage store;
            if (!parseGoodsClass(std::string_view(text).substr(0, colon), store.goods)) {
                fail(std::string("'") + key + "' entry '" + text +
                     "' names no goods class (bulk, cryo, hazardous)");
                return;
            }
            char* end = nullptr;
            store.capacity = std::strtof(text.c_str() + colon + 1, &end);
            if (end != text.c_str() + text.size() || store.capacity <= 0.0f) {
                fail(std::string("'") + key + "' entry '" + text + "' needs a capacity > 0");
                return;
            }
            // ⚑ Two holds of one class on ONE module is an authoring slip: a
            // module with more bulk capacity says so in its number. Across a
            // station they add up, and that sum is stage D's arithmetic.
            for (const ModuleStorage& seen : out) {
                if (seen.goods == store.goods) {
                    fail(std::string("'") + key + "' names the '" + goodsClassName(store.goods) +
                         "' class twice");
                    return;
                }
            }
            out.push_back(store);
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

    // The mount vocabulary a FITTING declares (Phase 31 stage B): which kind
    // of place it goes in and how big a one it needs. Required, both of them,
    // and for the same reason `[[ship.mount]]` requires them - a kind or a
    // size this parser invented is content it wrote on the author's behalf.
    void requireMountFit(MountKind& kind, MountSize& size)
    {
        std::string kindText;
        std::string sizeText;
        requireString("mount", kindText);
        requireString("size", sizeText);
        if (!failed && !parseMountKind(kindText, kind)) {
            fail("'mount' is not a mount kind: '" + kindText + "'");
        }
        if (!failed && !parseMountSize(sizeText, size)) {
            fail("'size' must be \"small\", \"medium\", \"large\" or \"xlarge\", not \"" + sizeText + "\"");
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
        // Phase 33 stage B. Not resolved to an index here for the reason every
        // id in this file is left a string: a def layer may name a commodity a
        // later layer defines, so the check is `validateCatalogGates`'s once
        // every layer has merged.
        optionalString("requires", out.requiresCommodity);
    }

    void optionalBool(const char* key, bool& out, bool* present = nullptr)
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
        if (present != nullptr) {
            *present = true;
        }
    }

    void optionalUint(const char* key, std::uint32_t& out, bool* present = nullptr)
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
        if (present != nullptr) {
            *present = true;
        }
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

    // ⚑ Phase 32 stage A. Both optional, both a WORD out of a closed set,
    // NEITHER defaulted: a class this parser picked would be gdd.md 11.1's
    // weight, mount budget and crew written on the author's behalf, and a role
    // it picked would be 11.2's answer to what the ship is for. `present` is
    // what says so, because `skiff` and `line` are both real answers and
    // neither can double as "unset".
    std::string classText;
    std::string roleText;
    reader.optionalString("class", classText, &def.hasHullClass);
    reader.optionalString("role", roleText, &def.hasRole);
    if (!reader.failed && def.hasHullClass && !parseHullClass(classText, def.hullClass)) {
        reader.fail("'class' is not a hull class: '" + classText +
                    "' (skiff, light, medium, heavy, cruiser, capital, supercapital, titan)");
    }
    if (!reader.failed && def.hasRole && !parseHullRole(roleText, def.role)) {
        reader.fail("'role' is not a hull role: '" + roleText +
                    "' (line, carrier, logistics, support, covert, industrial)");
    }

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
    reader.optionalFloat("cargo", def.cargoCapacity);
    reader.optionalFloat("scan_range", def.scanRange);
    reader.optionalFloat("scan_speed", def.scanSpeed);
    reader.optionalFloat("collector_range", def.collectorRange);
    reader.optionalFloat("signature", def.signature);

    reader.optionalFloat("price", def.price);
    reader.optionalFloat("mass", def.mass);
    reader.optionalFloat("power_output", def.powerOutput);
    reader.optionalUint("crew_berths", def.crewBerths);
    reader.optionalGate(def.gate);

    // Nested rows, by hand and in the shape `parseSystem` already uses for
    // `[[system.planet]]` - the second def kind in the game to have any, and
    // still not enough callers to be worth a shared helper.
    if (const TomlValue* mounts = table.find("mount"); mounts != nullptr && !reader.failed) {
        if (!mounts->isArray()) {
            reader.fail("'mount' must be an array of tables ([[ship.mount]])");
        }
        for (std::size_t i = 0; !reader.failed && i < mounts->size(); ++i) {
            const TomlValue& row = (*mounts)[i];
            if (!row.isTable()) {
                reader.fail("'mount' must be an array of tables ([[ship.mount]])");
                break;
            }
            ShipMount mount;
            FieldReader inner{.table = row,
                              .context = reader.context + " mount " + std::to_string(i),
                              .outError = outError};
            inner.requireString("id", mount.id);
            if (!inner.failed) {
                inner.context = reader.context + " mount '" + mount.id + "'";
            }

            // ⚑ BOTH ARE REQUIRED, and neither gets a default. A `kind` this
            // file invented would be a hull silently accepting kit its author
            // never said it takes, and a `size` this file invented would be
            // the hull's fitting BUDGET written by the parser - which is the
            // one number gdd.md 11.1 makes a hull class mean.
            std::string kindText;
            std::string sizeText;
            inner.requireString("kind", kindText);
            inner.requireString("size", sizeText);
            if (!inner.failed && !parseMountKind(kindText, mount.kind)) {
                inner.fail("'kind' is not a mount kind: '" + kindText + "'");
            }
            if (!inner.failed && !parseMountSize(sizeText, mount.size)) {
                inner.fail("'size' must be \"small\", \"medium\", \"large\" or \"xlarge\", not \"" +
                           sizeText + "\"");
            }

            bool hasAim = false;
            bool hasArc = false;
            inner.optionalFloat3("at", mount.at, &mount.external);
            inner.optionalFloat3("aim", mount.aim, &hasAim);
            inner.optionalFloat("arc", mount.arc, &hasArc);
            inner.optionalString("fit", mount.fit);
            inner.rejectUnknownKeys({"id", "kind", "size", "at", "aim", "arc", "fit"});

            // ⚑ REFUSED RATHER THAN IGNORED, and it is decisions/014 rule 2
            // read backwards. `at` is the ONE key that decides external, so a
            // row carrying `aim` or `arc` without it has written a facing and
            // a traverse for something that is never drawn and never aimed -
            // and silently dropping them reads to the author as a parser that
            // ate their turret.
            if (!inner.failed && !mount.external && hasAim) {
                inner.fail("'aim' needs an 'at': a mount with no position is internal, and an internal "
                           "mount has nothing to face");
            }
            if (!inner.failed && !mount.external && hasArc) {
                inner.fail("'arc' needs an 'at': a mount with no position is internal, and an internal "
                           "mount has nothing to traverse");
            }
            if (!inner.failed && hasAim && mount.aim[0] == 0.0f && mount.aim[1] == 0.0f &&
                mount.aim[2] == 0.0f) {
                inner.fail("'aim' must not be the zero vector");
            }
            if (!inner.failed && (mount.arc < 0.0f || mount.arc > 360.0f)) {
                inner.fail("'arc' must be between 0 and 360 degrees");
            }
            // ⚑ decisions/014 rule 1 enforced where it can still be enforced.
            // A save names a fitting by its mount id; two mounts sharing one id
            // makes that name ambiguous, and the ambiguity would not surface
            // until a player loaded a campaign.
            if (!inner.failed && def.findMount(mount.id) != nullptr) {
                inner.fail("duplicate mount id '" + mount.id + "' on this hull");
            }
            if (inner.failed) {
                reader.failed = true;
                break;
            }
            def.mounts.push_back(std::move(mount));
        }
    }

    reader.rejectUnknownKeys({"id",
                              "name",
                              "model",
                              "cockpit",
                              "scale",
                              "class",
                              "role",
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
                              "cargo",
                              "scan_range",
                              "scan_speed",
                              "collector_range",
                              "signature",
                              "price",
                              "mass",
                              "power_output",
                              "crew_berths",
                              "mount",
                              "factions",
                              "min_rep",
                              "requires"});
    if (!reader.failed && def.scale <= 0.0f) {
        reader.fail("'scale' must be > 0");
    }
    if (!reader.failed && def.mass <= 0.0f) {
        reader.fail("'mass' must be > 0");
    }
    // ⚑ A hull that is silent to everybody is not a covert hull, it is the
    // inspection mechanic switched off in a data file - and zero would also
    // divide the scan clock by nothing. Refused at the DEF, where the author
    // finds out, rather than clamped at the consumer where they never would.
    if (!reader.failed && def.signature <= 0.0f) {
        reader.fail("'signature' must be > 0");
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
    reader.requireMountFit(def.mount, def.size);
    reader.optionalFloat("damage", def.damage);
    reader.optionalFloat("rate_of_fire", def.rateOfFire);
    reader.optionalFloat("range", def.range);
    reader.optionalFloat("projectile_speed", def.projectileSpeed);
    reader.optionalFloat("energy_cost", def.energyCost);
    reader.optionalFloat("mining_power", def.miningPower);
    reader.optionalFloat("price", def.price);
    reader.optionalString("model", def.model);
    reader.optionalString("bolt_model", def.boltModel);
    reader.optionalGate(def.gate);

    reader.rejectUnknownKeys({"id",
                              "name",
                              "kind",
                              "mount",
                              "size",
                              "damage",
                              "rate_of_fire",
                              "range",
                              "projectile_speed",
                              "energy_cost",
                              "mining_power",
                              "price",
                              "model",
                              "bolt_model",
                              "factions",
                              "min_rep",
                              "requires"});
    if (!reader.failed && def.kind != "projectile" && def.kind != "hitscan") {
        reader.fail("'kind' must be \"projectile\" or \"hitscan\"");
    }
    // The mirror of the component check, and the same reason: a weapon in a
    // shield mount is an id the fit model would look up in the wrong table.
    if (!reader.failed && !mountTakesWeapon(def.mount)) {
        reader.fail(std::string("'mount' is \"") + mountKindName(def.mount) +
                    "\", which takes a [[component]]; a weapon goes in a turret, fixed, launcher or bay "
                    "mount");
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
    // Phase 32 stage C. Read as words and refused as words, so `builds_no =
    // ["trade"]` is a parse error rather than a cell nobody notices is unset.
    {
        std::vector<std::string> cells;
        reader.optionalStringList("builds_no", cells);
        for (const std::string& text : cells) {
            RosterCell cell = RosterCell::Patrol;
            if (!parseRosterCell(text, cell)) {
                reader.fail("'builds_no' entry '" + text + "' is not a roster cell (patrol, raider, trader)");
                break;
            }
            def.buildsNo[static_cast<std::size_t>(cell)] = true;
        }
    }
    reader.optionalBiasList("station_bias", def.stationBias);
    // Phase 33 stage D. Commodity ids, checked for existence by
    // `validateLegality` once every layer has merged - a mod may add the good
    // in one file and the law about it in another.
    reader.optionalStringList("contraband", def.contraband);
    reader.optionalStringList("restricted", def.restricted);

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
                              "builds_no",
                              "station_bias",
                              "contraband",
                              "restricted"});
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
    // ⚑⚑ SAYING BOTH IS REFUSED RATHER THAN RESOLVED, and this is the check
    // that makes `builds_no` mean one thing. A row declaring `builds_no =
    // ["trader"]` beside a populated `ships_trader` is not a precedence puzzle
    // for the loader to settle quietly - it is an author who changed their mind
    // in one place and not the other, and the two halves are 40 lines apart in
    // a file where every faction looks like every other. Same bargain a
    // `[[model]]` naming a material AND carrying the four surface keys makes.
    if (!reader.failed) {
        const std::vector<std::string>* rosters[kRosterCellCount] = {
            &def.shipsPatrol, &def.shipsRaider, &def.shipsTrader};
        for (std::size_t i = 0; i < kRosterCellCount; ++i) {
            if (def.buildsNo[i] && !rosters[i]->empty()) {
                reader.fail(std::string("declares 'builds_no' for the ") +
                            rosterCellName(static_cast<RosterCell>(i)) + " cell and also lists ships_" +
                            rosterCellName(static_cast<RosterCell>(i)) + " hulls");
                break;
            }
        }
    }
    // ⚑⚑ A GOOD CANNOT BE BOTH FORBIDDEN AND LICENSED, AND THAT IS REFUSED
    // RATHER THAN RESOLVED - the same bargain `builds_no` makes ten lines up.
    // A faction listing one commodity under both keys is an author who changed
    // their mind in one place, and picking a winner quietly would make the
    // difference between "ten years in a labour camp" and "show me the paper"
    // depend on which loop runs first.
    if (!reader.failed) {
        for (const std::string& id : def.contraband) {
            const bool alsoRestricted =
                std::find(def.restricted.begin(), def.restricted.end(), id) != def.restricted.end();
            if (alsoRestricted) {
                reader.fail("lists '" + id + "' as both 'contraband' and 'restricted'");
                break;
            }
        }
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

    // ⚑ Phase 33 stage B. Optional and NOT defaulted, exactly as `class` is on
    // a hull: `raw` is a real tier, so it cannot double as "unset".
    std::string tierText;
    reader.optionalString("tier", tierText, &def.hasTier);
    if (!reader.failed && def.hasTier && !parseCommodityTier(tierText, def.tier)) {
        reader.fail("'tier' is not a material tier: '" + tierText +
                    "' (raw, refined, component, assembly, consumer)");
    }

    // ⚑ Phase 34 stage D. What it takes to KEEP this good, as opposed to what
    // made it. Optional, and read as `bulk` when unsaid - see the note on
    // `CommodityDef::goodsClass` for why this one defaults where `tier` does
    // not: a good outside the material tree is a real state, a good nobody can
    // warehouse anywhere is not.
    std::string goodsText;
    reader.optionalString("goods_class", goodsText, &def.hasGoodsClass);
    if (!reader.failed && def.hasGoodsClass && !parseGoodsClass(goodsText, def.goodsClass)) {
        reader.fail("'goods_class' is not a goods class: '" + goodsText + "' (bulk, cryo, hazardous)");
    }

    reader.rejectUnknownKeys({"id",
                              "name",
                              "base_price",
                              "ore_weight_core",
                              "ore_weight_frontier",
                              "ore_weight_fringe",
                              "model",
                              "chunk_model",
                              "tier",
                              "goods_class"});
    if (!reader.failed && def.basePrice <= 0.0f) {
        reader.fail("'base_price' must be > 0");
    }
    if (!reader.failed &&
        (def.oreWeightCore < 0.0f || def.oreWeightFrontier < 0.0f || def.oreWeightFringe < 0.0f)) {
        reader.fail("'ore_weight_*' must be >= 0");
    }
    // ⚑⚑ A GOOD YOU DIG OUT OF A ROCK IS T0, AND THIS IS THE ONE DIRECTION THAT
    // HOLDS. gdd.md 6 puts ores, ices, gases, silicates and organics at T0 and
    // nothing else is ever mined, so an ore weight on a `refined` or `component`
    // row is an authoring slip rather than a design. The converse is NOT checked
    // and must not be: salvage is T0 and comes off a wreck, not out of the ground.
    if (!reader.failed && def.hasTier && def.tier != CommodityTier::Raw &&
        (def.oreWeightCore > 0.0f || def.oreWeightFrontier > 0.0f || def.oreWeightFringe > 0.0f)) {
        reader.fail(std::string("tier '") + commodityTierName(def.tier) +
                    "' carries an ore weight, but only a 'raw' good is mined");
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

// `[material.textures]` (Phase 25 stage C): the extra textures this material
// binds beyond its albedo, in the order the shader declares them.
//
// ⚑ ORDER IS THE CONTRACT AND IT IS WORTH SAYING WHY THE NAME IS NOT. A
// descriptor binding is a number, so what the engine must get right is the
// POSITION; the name is carried so that a refusal can say "slot 'glow'" rather
// than "set 1 binding 0" and send the author counting. `TomlValue` keeps table
// members in file order, which is what makes reading the file enough to know
// what the shader will see.
void readMaterialSlots(FieldReader& reader, MaterialDef& def)
{
    const TomlValue* table = reader.table.find("textures");
    if (table == nullptr) {
        return;
    }
    if (!table->isTable()) {
        reader.fail("'textures' must be a table of slot name to cooked texture stem");
        return;
    }
    for (const std::pair<std::string, TomlValue>& member : table->members()) {
        if (!member.second.isString() || member.second.asString().empty()) {
            reader.fail("texture slot '" + member.first + "' must be a non-empty cooked texture stem");
            return;
        }
        for (const MaterialSlot& existing : def.slots) {
            if (existing.name == member.first) {
                reader.fail("texture slot '" + member.first + "' is declared twice");
                return;
            }
        }
        def.slots.push_back({.name = member.first, .texture = member.second.asString()});
    }
}

// `[material.params]` (Phase 25 stage C): the scalars this material tunes on
// its shader, matched into the shader's uniform block by NAME.
void readMaterialParams(FieldReader& reader, MaterialDef& def)
{
    const TomlValue* table = reader.table.find("params");
    if (table == nullptr) {
        return;
    }
    if (!table->isTable()) {
        reader.fail("'params' must be a table of parameter name to number");
        return;
    }
    for (const std::pair<std::string, TomlValue>& member : table->members()) {
        if (!member.second.isFloat() && !member.second.isInteger()) {
            reader.fail("parameter '" + member.first + "' must be a number");
            return;
        }
        for (const MaterialParam& existing : def.params) {
            if (existing.name == member.first) {
                reader.fail("parameter '" + member.first + "' is declared twice");
                return;
            }
        }
        def.params.push_back({.name = member.first, .value = static_cast<float>(member.second.asFloat())});
    }
}

// A `[[material]]` row (Phase 25 stage A): how a surface is drawn, split out of
// the model that wears it. Stage B added the shader pair and the pipeline
// state, which is everything `GraphicsPipelineDesc` can be told that this
// engine has a reason to vary. Stage C added the declared slots and params -
// what that shader actually gets fed.
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

    // ⚑ PHASE 25 STAGE C. `textures` and `params` are TABLES rather than key
    // lists because their ORDER is their meaning for the first and their NAMES
    // are for the second, and a TOML table preserves both.
    //
    // ⚑⚑ AN INLINE TABLE, NOT A `[material.textures]` SUB-HEADER, AND THE
    // CONSTRAINT COMES FROM THE TOOL RATHER THAN FROM HERE. `def_doc` - the
    // comment-preserving document the Forge edits def files through - models a
    // file as a flat list of `[[table]]` rows holding keys, and REFUSES a
    // nested header rather than silently reassigning its keys to the row above.
    // A sub-header would therefore parse for the game and break the tool, which
    // is the worst of the available failures because only one of the two is in
    // front of a person. Both spellings reach this code as a `TomlValue` table,
    // so nothing below cares.
    //
    // ⚑ Read before `rejectUnknownKeys` so an author's typo inside one still
    // reaches the unknown-key path with the material's name in context.
    if (!reader.failed) {
        readMaterialSlots(reader, def);
    }
    if (!reader.failed) {
        readMaterialParams(reader, def);
    }

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
                              "cull",
                              "textures",
                              "params"});
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
    reader.optionalRecipeList("modules", def.modules);

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
                              "refine_output",
                              "modules"});
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

// One `[[module]]`: a thing a station is built out of (Phase 34 stage A).
//
// ⚑ The noun was ship outfitting until Phase 31 stage A1 renamed it
// `[[component]]` for exactly this, which is why this parser can have the word
// without a migration - see `components.toml`.
bool parseModule(const TomlValue& table, const char* sourceName, std::vector<ModuleDef>& out, std::string* outError)
{
    ModuleDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": module '" + def.id + "'";
    }
    reader.requireString("name", def.name);

    // ⚑ REQUIRED, not optional-with-a-default: every family is a real answer,
    // so there is no spelling left over to mean "nobody said". The same
    // argument `CommodityTier` makes, reached from the other direction - a tier
    // is optional because a commodity can decline to answer, and a module
    // cannot: what it is FOR is the only thing the composer sorts it by.
    std::string familyText;
    reader.requireString("family", familyText);
    if (!reader.failed && !parseModuleFamily(familyText, def.family)) {
        reader.fail("'family' is not a module family: '" + familyText +
                    "' (power, habitat, storage, industry, commerce, recreation, services, shadow)");
    }

    reader.optionalRateList("produces", def.produces);
    reader.optionalRateList("consumes", def.consumes);
    reader.optionalRateList("feedstock", def.feedstock);
    reader.optionalStorageList("stores", def.stores);
    reader.optionalFloat("power_output", def.powerOutput);
    reader.optionalFloat("power_draw", def.powerDraw);
    reader.optionalString("refine_input", def.refineInput);
    reader.optionalString("refine_output", def.refineOutput);

    std::vector<std::string> screenNames;
    reader.optionalStringList("screens", screenNames);
    for (const std::string& text : screenNames) {
        StationScreen screen = StationScreen::Trade;
        if (!parseStationScreen(text, screen)) {
            reader.fail("'screens' names no dock screen: '" + text +
                        "' (trade, outfitting, shipyard, crew, factions, missions, survey, "
                        "refinery, bar)");
            break;
        }
        // ⚑ A duplicate is refused rather than folded, because a screen list is
        // a union across the whole station: two MODULES offering `trade` is an
        // ordinary composition, and one module saying it twice is a typo that
        // would otherwise never be seen again.
        if (std::find(def.screens.begin(), def.screens.end(), screen) != def.screens.end()) {
            reader.fail(std::string("'screens' names '") + stationScreenName(screen) + "' twice");
            break;
        }
        def.screens.push_back(screen);
    }

    reader.rejectUnknownKeys({"id",
                              "name",
                              "family",
                              "produces",
                              "consumes",
                              "feedstock",
                              "stores",
                              "power_output",
                              "power_draw",
                              "screens",
                              "refine_input",
                              "refine_output"});

    if (!reader.failed && (def.powerOutput < 0.0f || def.powerDraw < 0.0f)) {
        reader.fail("'power_output' and 'power_draw' must be >= 0");
    }
    // ⚑⚑ THE ONE CROSS-FIELD RULE, AND IT IS THE COMPOSER'S CONSTRAINT WRITTEN
    // WHERE AN AUTHOR MEETS IT. gdd.md §12 gives the budget to the Power family
    // and every other family draws against it, so a habitat ring that quietly
    // makes its own power would satisfy stage B's validity check by opting out
    // of it. Drawing power is unrestricted: everything draws, including a
    // reactor running its own coolant.
    if (!reader.failed && def.powerOutput > 0.0f && def.family != ModuleFamily::Power) {
        reader.fail(std::string("family '") + moduleFamilyName(def.family) +
                    "' carries a 'power_output', but only the power family makes power");
    }
    if (!reader.failed && def.refineInput.empty() != def.refineOutput.empty()) {
        reader.fail("'refine_input' and 'refine_output' must be given together");
    }
    // ⚑⚑ THE SECOND CROSS-FIELD RULE, AND STAGE C IS THE REASON IT CAN EXIST AT
    // ALL. The Refining tab is now gated on this list, while the service the tab
    // draws still comes from the pair above - two fields on one row that a
    // reader has to find in agreement. Saying one without the other is a tab
    // with nothing behind it, or a service with no way in, and until stage C
    // that mistake showed up as the note it deleted: "(this station refines
    // nothing)", drawn under a header on every station in the galaxy. An empty
    // tab was a place a defect could hide; refusing at load is where it goes
    // instead.
    const bool offersRefining =
        std::find(def.screens.begin(), def.screens.end(), StationScreen::Refinery) != def.screens.end();
    if (!reader.failed && offersRefining == def.refineInput.empty()) {
        reader.fail(offersRefining
                        ? "offers the 'refinery' screen but names no 'refine_input'/'refine_output'"
                        : "names 'refine_input'/'refine_output' but does not offer the 'refinery' screen");
    }
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

// One `[[system]]`: a place somebody put somewhere (Phase 29).
//
// ⚑⚑ THE NESTED ROWS NEEDED NO PARSER WORK, WHICH WAS NOT OBVIOUS AND WAS
// CHECKED RATHER THAN HOPED. `TomlParser::descend` already carries the rule
// "`[a.b]` where a is an array of tables means the last element of a", so
// `[[system.planet]]` lands inside the `[[system]]` above it exactly the way
// standard TOML says it should. No def in the tree had ever used one, so this
// was a supported-but-unexercised path until `toml_tests` started exercising it.
//
// ⚑⚑ SPLIT FROM `parseSystem` IN STAGE C BECAUSE A CONSTELLATION MEMBER IS THE
// SAME ROW READ IN A DIFFERENT PLACE. `parseSystem` merges into the database;
// a member is read into its group and merged with it. `isMember` changes
// exactly one thing - a member may not carry a placement rule of its own,
// because its group carries one - and that is refused by name rather than left
// to `rejectUnknownKeys` to call "unknown key", which would be true and useless.
bool readSystemDef(const TomlValue& table,
                   const char* sourceName,
                   const std::string& contextLead,
                   bool isMember,
                   SystemDef& def,
                   std::string* outError)
{
    def.source = sourceName;

    FieldReader reader{.table = table, .context = contextLead, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = contextLead + ": system '" + def.id + "'";
    }
    reader.optionalString("placement", def.placement);
    reader.optionalString("name", def.name, &def.hasName);
    reader.optionalString("region", def.region, &def.hasRegion);
    reader.optionalString("faction", def.factionId, &def.hasFaction);
    reader.optionalBool("lawless", def.lawless);
    reader.optionalUint("primary_planet", def.primaryPlanet, &def.hasPrimaryPlanet);
    reader.optionalFloat("security", def.security, &def.hasSecurity);
    reader.optionalBool("secret", def.secret);
    reader.optionalString("at_system", def.atSystemFactionId);
    reader.rejectUnknownKeys({"id",
                              "placement",
                              "at_system",
                              "jumps_from",
                              "name",
                              "region",
                              "faction",
                              "lawless",
                              "primary_planet",
                              "security",
                              "secret",
                              "planet",
                              "station"});

    // ⚑ A MEMBER IS PLACED BY ITS GROUP, AND SAYING SO IS BETTER THAN THE THREE
    // KEYS SIMPLY NOT EXISTING. An author who writes `placement` on a member is
    // not making a typo, they are asking a question - "can this one be somewhere
    // else?" - and the answer is that sentence, not "unknown key".
    if (!reader.failed && isMember) {
        for (const char* key : {"placement", "at_system", "jumps_from"}) {
            if (table.find(key) != nullptr) {
                reader.fail(std::string("'") + key +
                            "' is not a constellation member's to write: the whole group takes one "
                            "placement rule, on the [[constellation]] itself");
                break;
            }
        }
    }

    // `jumps_from = { system = "…", min = N, max = M }`. Read by hand for the
    // same reason the nested arrays below are: it is the only inline table in
    // a def row, and a shared helper for one caller is not a helper.
    if (const TomlValue* ring = table.find("jumps_from"); ring != nullptr && !reader.failed) {
        if (!ring->isTable()) {
            reader.fail("'jumps_from' must be a table: { system = \"…\", min = 2, max = 4 }");
        } else {
            FieldReader inner{
                .table = *ring, .context = reader.context + " jumps_from", .outError = outError};
            bool hasMin = false;
            bool hasMax = false;
            inner.requireString("system", def.jumpsFromSystemId);
            inner.optionalUint("min", def.jumpsFromMin, &hasMin);
            inner.optionalUint("max", def.jumpsFromMax, &hasMax);
            if (!inner.failed && !hasMin) {
                inner.fail("'min' is required");
            }
            if (!inner.failed && !hasMax) {
                inner.fail("'max' is required");
            }
            inner.rejectUnknownKeys({"system", "min", "max"});
            if (!inner.failed && def.jumpsFromMin > def.jumpsFromMax) {
                inner.fail("'min' must not be greater than 'max'");
            }
            // Zero jumps from the anchor IS the anchor, and the anchor is
            // already taken by the system that placed it - so the ring would be
            // empty for a reason the author cannot see in their own file.
            if (!inner.failed && def.jumpsFromMax == 0) {
                inner.fail("'max' must be at least 1; 0 jumps from the anchor is the anchor itself");
            }
            if (inner.failed) {
                reader.failed = true;
            }
        }
    }

    // Nested rows, read by hand because they are the only arrays-of-tables
    // inside a def row and a shared helper for one caller is not a helper.
    if (const TomlValue* planets = table.find("planet"); planets != nullptr && !reader.failed) {
        if (!planets->isArray()) {
            reader.fail("'planet' must be an array of tables ([[system.planet]])");
        }
        for (std::size_t i = 0; !reader.failed && i < planets->size(); ++i) {
            const TomlValue& row = (*planets)[i];
            if (!row.isTable()) {
                reader.fail("'planet' must be an array of tables ([[system.planet]])");
                break;
            }
            AuthoredPlanetDef planet;
            FieldReader inner{.table = row,
                              .context = reader.context + " planet " + std::to_string(i),
                              .outError = outError};
            inner.requireString("name", planet.name);
            inner.optionalDouble("radius", planet.radius, &planet.hasRadius);
            inner.rejectUnknownKeys({"name", "radius"});
            if (!inner.failed && planet.hasRadius && planet.radius <= 0.0) {
                inner.fail("'radius' must be > 0 metres");
            }
            if (inner.failed) {
                reader.failed = true;
                break;
            }
            def.planets.push_back(std::move(planet));
        }
    }
    if (const TomlValue* stations = table.find("station"); stations != nullptr && !reader.failed) {
        if (!stations->isArray()) {
            reader.fail("'station' must be an array of tables ([[system.station]])");
        }
        for (std::size_t i = 0; !reader.failed && i < stations->size(); ++i) {
            const TomlValue& row = (*stations)[i];
            if (!row.isTable()) {
                reader.fail("'station' must be an array of tables ([[system.station]])");
                break;
            }
            AuthoredStationDef station;
            FieldReader inner{.table = row,
                              .context = reader.context + " station " + std::to_string(i),
                              .outError = outError};
            inner.requireString("name", station.name);
            inner.requireString("station", station.stationId);
            inner.rejectUnknownKeys({"name", "station"});
            if (inner.failed) {
                reader.failed = true;
                break;
            }
            def.stations.push_back(std::move(station));
        }
    }

    // ⚑ REFUSED, NOT WARNED, AND BY NAME - decision 3. There is no fallback
    // that is not a lie: a rule nobody implements yet would put the campaign's
    // starting system somewhere nobody chose, and an out-of-range primary
    // planet is `spec.planets[spec.primaryPlanet]` at six unguarded call sites.
    if (!reader.failed && def.placement != "random" && def.placement != "anywhere" &&
        def.placement != "at_system" && def.placement != "jumps_from") {
        reader.fail("'placement' must be \"random\", \"anywhere\", \"at_system\" or \"jumps_from\", not \"" +
                    def.placement + "\"");
    }
    // ⚑ A PARAMETER WITHOUT ITS RULE IS THE MISTAKE THIS SHAPE INVITES, so it
    // is refused in both directions rather than silently ignored. Writing
    // `jumps_from = {…}` and forgetting `placement = "jumps_from"` would
    // otherwise place the system at random and read to the author as a parser
    // that ate their ring.
    if (!reader.failed && def.placement == "at_system" && def.atSystemFactionId.empty()) {
        reader.fail("'placement' is \"at_system\" but no 'at_system' key names a [[faction]] whose capital "
                    "to take");
    }
    if (!reader.failed && def.placement != "at_system" && !def.atSystemFactionId.empty()) {
        reader.fail("'at_system' is set but 'placement' is \"" + def.placement +
                    "\"; write placement = \"at_system\" to use it");
    }
    if (!reader.failed && def.placement == "jumps_from" && def.jumpsFromSystemId.empty()) {
        reader.fail("'placement' is \"jumps_from\" but there is no 'jumps_from' table; write "
                    "jumps_from = { system = \"…\", min = 2, max = 4 }");
    }
    if (!reader.failed && def.placement != "jumps_from" && !def.jumpsFromSystemId.empty()) {
        reader.fail("'jumps_from' is set but 'placement' is \"" + def.placement +
                    "\"; write placement = \"jumps_from\" to use it");
    }
    if (!reader.failed && def.hasName && def.name.empty()) {
        reader.fail("'name' must not be empty when given");
    }
    if (!reader.failed && def.hasFaction && def.lawless) {
        reader.fail("'faction' and 'lawless' say different things; give one");
    }
    if (!reader.failed && def.hasFaction && def.factionId.empty()) {
        reader.fail("'faction' must name a [[faction]] row; use lawless = true for no owner");
    }
    // ⚑⚑⚑ THE ONE MISTAKE THIS KEY INVITES, REFUSED WITH THE REASON IN IT. The
    // rating a player reads is SIGNED and `sol.security` prints it signed, so
    // an author who has seen a clan system read -0.75 will write -0.75 here.
    // decisions/019 decision 2 is why that cannot be honoured: the sign is not
    // "how much", it is WHO POLICES THIS PLACE, and it is a fact about the
    // owner rather than a thing to choose. A negative on a system the Navy
    // holds would put "Held by Solar Navy: -0.60" on the map - and worse,
    // `patrolsFor` returns 0 below zero while `raidersFor` is only reached down
    // the pirate branch, so the sky over that station would be EMPTY. Both are
    // lies the generator cannot tell on its own, and stage D's whole promise is
    // that a route planned off the map was planned off the truth.
    if (!reader.failed && def.hasSecurity && (def.security < 0.0f || def.security > 1.0f)) {
        reader.fail("'security' is how hard this place is held, from 0 to 1 - the SIGN is not yours to "
                    "write: it says WHO holds it and the generator takes it from the owner, so a "
                    "clan-held system reads negative on its own");
    }
    // Same shape as the `faction`/`lawless` pair above and the same reason: two
    // adjacent lines saying different things. Nobody holds it, so there is no
    // "how hard" - a system nobody holds is one nobody polices, which is what
    // exactly zero already means.
    if (!reader.failed && def.hasSecurity && def.lawless) {
        reader.fail("'lawless' and 'security' say different things; a place nobody holds is one nobody "
                    "polices, and it reads exactly zero without being told to");
    }
    if (!reader.failed && def.hasRegion && def.region != "core" && def.region != "frontier" &&
        def.region != "fringe") {
        reader.fail("'region' must be \"core\", \"frontier\" or \"fringe\", not \"" + def.region + "\"");
    }
    // The generator's own invariant, stated where an author can be told about
    // it: `populateSystem` has always produced at least one planet and set
    // primaryPlanet inside it, and six call sites index that pair with no
    // guard. An authored system with no planets of its own gets generated ones,
    // so only an explicit primary planet can point past the end.
    if (!reader.failed && def.hasPrimaryPlanet && !def.planets.empty() &&
        def.primaryPlanet >= def.planets.size()) {
        reader.fail("'primary_planet' is " + std::to_string(def.primaryPlanet) + " but only " +
                    std::to_string(def.planets.size()) + " [[system.planet]] row(s) are declared");
    }
    if (!reader.failed && def.hasPrimaryPlanet && def.planets.empty()) {
        reader.fail("'primary_planet' needs [[system.planet]] rows to point into");
    }
    return !reader.failed;
}

bool parseSystem(const TomlValue& table,
                 const char* sourceName,
                 std::vector<SystemDef>& out,
                 std::string* outError)
{
    SystemDef def;
    if (!readSystemDef(table, sourceName, sourceName, false, def, outError)) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

// One `[[constellation]]`: places somebody put somewhere TOGETHER (Phase 29
// stage C).
//
// ⚑⚑⚑ THE NESTING GOES THREE DEEP AND THAT WAS CHECKED RATHER THAN HOPED.
// `TomlParser::descend` walks every header segment but the last, following an
// array of tables to its final element each time - so `[[constellation.system]]`
// lands in the last constellation and `[[constellation.system.planet]]` lands in
// the last system inside it, to any depth. Stage A exercised two levels for the
// first time in this project's history; this is the first three.
bool parseConstellation(const TomlValue& table,
                        const char* sourceName,
                        std::vector<ConstellationDef>& out,
                        std::string* outError)
{
    ConstellationDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": constellation '" + def.id + "'";
    }
    reader.optionalString("placement", def.placement);
    reader.rejectUnknownKeys({"id", "placement", "system", "link"});

    // ⚑⚑ ONE LEGAL VALUE, AND THE REFUSAL CARRIES THE REASON. The other three
    // rules REPLACE a system the generator already made; a group of three
    // cannot replace one node as a unit, and reading `random` as "near a random
    // system" for a group while it means "become a random system" for a system
    // is two rules wearing one word. The key exists so this can be said.
    if (!reader.failed && def.placement != "anywhere") {
        reader.fail("'placement' must be \"anywhere\", not \"" + def.placement +
                    "\": the other rules replace a system the generator already made, and a group "
                    "cannot replace one node as a unit");
    }

    if (const TomlValue* members = table.find("system"); members != nullptr && !reader.failed) {
        if (!members->isArray()) {
            reader.fail("'system' must be an array of tables ([[constellation.system]])");
        }
        for (std::size_t i = 0; !reader.failed && i < members->size(); ++i) {
            const TomlValue& row = (*members)[i];
            if (!row.isTable()) {
                reader.fail("'system' must be an array of tables ([[constellation.system]])");
                break;
            }
            SystemDef member;
            if (!readSystemDef(row, sourceName, reader.context, true, member, outError)) {
                reader.failed = true;
                break;
            }
            def.members.push_back(std::move(member));
        }
    }
    // A group of one is a system, and a group of none is nothing. Refused
    // rather than tolerated because both are almost certainly a file that lost
    // a row, and both would otherwise place silently.
    if (!reader.failed && def.members.size() < 2) {
        reader.fail("a constellation needs at least two [[constellation.system]] rows; " +
                    std::string(def.members.size() == 1 ? "one place on its own is a [[system]]"
                                                        : "this one declares none"));
    }

    if (const TomlValue* links = table.find("link"); links != nullptr && !reader.failed) {
        if (!links->isArray()) {
            reader.fail("'link' must be an array of tables ([[constellation.link]])");
        }
        for (std::size_t i = 0; !reader.failed && i < links->size(); ++i) {
            const TomlValue& row = (*links)[i];
            if (!row.isTable()) {
                reader.fail("'link' must be an array of tables ([[constellation.link]])");
                break;
            }
            ConstellationLinkDef link;
            FieldReader inner{
                .table = row, .context = reader.context + " link " + std::to_string(i), .outError = outError};
            inner.requireString("from", link.fromId);
            inner.requireString("to", link.toId);
            inner.rejectUnknownKeys({"from", "to"});
            if (!inner.failed && link.fromId == link.toId) {
                inner.fail("'from' and 'to' name the same system; a lane needs two ends");
            }
            // ⚑ BOTH ENDS MUST BE THIS GROUP'S OWN MEMBERS. A lane out of the
            // constellation is a gate, and which gates a system gets is the
            // generator's to decide - that is exactly the "authored layout
            // contradicts the gate graph" that decisions/018 refused.
            const std::string* ends[2] = {&link.fromId, &link.toId};
            const char* endNames[2] = {"from", "to"};
            for (int end = 0; end < 2 && !inner.failed; ++end) {
                bool isMember = false;
                for (const SystemDef& member : def.members) {
                    if (member.id == *ends[end]) {
                        isMember = true;
                        break;
                    }
                }
                if (!isMember) {
                    inner.fail(std::string("'") + endNames[end] + "' names '" + *ends[end] +
                               "', which is not a [[constellation.system]] of this constellation");
                }
            }
            for (const ConstellationLinkDef& existing : def.links) {
                if (inner.failed) {
                    break;
                }
                if ((existing.fromId == link.fromId && existing.toId == link.toId) ||
                    (existing.fromId == link.toId && existing.toId == link.fromId)) {
                    inner.fail("'" + link.fromId + "' and '" + link.toId +
                               "' are already linked; one lane is one lane");
                }
            }
            if (inner.failed) {
                reader.failed = true;
                break;
            }
            def.links.push_back(std::move(link));
        }
    }

    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

bool parseComponent(const TomlValue& table,
                    const char* sourceName,
                    std::vector<ComponentDef>& out,
                    std::string* outError)
{
    ComponentDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": component '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.requireMountFit(def.mount, def.size);
    reader.optionalString("model", def.model);
    reader.optionalFloat("price", def.price);
    reader.optionalFloat("mass", def.mass);
    reader.optionalFloat("power_draw", def.powerDraw);
    reader.optionalModifiers(def.modifiers);
    reader.optionalGate(def.gate);

    reader.rejectUnknownKeys(
        {"id", "name", "mount", "size", "model", "price", "mass", "power_draw", "factions", "min_rep",
         "requires"},
        /*allowModifiers=*/true);
    // ⚑ A component cannot claim a gun's mount. The four weapon-taking kinds
    // hold a `[[weapon]]`, and that is what makes a mount's `fit` id
    // unambiguous without the def having to say which table it came from -
    // so a component authored `mount = "turret"` would be a fitting nothing
    // could ever look up rather than a merely useless one.
    if (!reader.failed && mountTakesWeapon(def.mount)) {
        reader.fail(std::string("'mount' is \"") + mountKindName(def.mount) +
                    "\", which takes a [[weapon]]; a component goes in an engine, thruster, shield, "
                    "armor, utility, subsystem, hangar or dock mount");
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

    reader.rejectUnknownKeys({"id", "name", "role", "price", "factions", "min_rep", "requires"},
                             /*allowModifiers=*/true);
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

// Phase 35 stage C. Every field but `id` and `name` is optional, and every
// optional one NARROWS where this person will sit - so a row with none of them
// is a person who could be in any room in the galaxy, which is a legitimate
// thing to write and is the cheapest row an author can add.
bool parseCharacter(const TomlValue& table,
                    const char* sourceName,
                    std::vector<CharacterDef>& out,
                    std::string* outError)
{
    CharacterDef def;
    def.source = sourceName;

    FieldReader reader{.table = table, .context = sourceName, .outError = outError};
    reader.requireString("id", def.id);
    if (!reader.failed) {
        reader.context = std::string(sourceName) + ": character '" + def.id + "'";
    }
    reader.requireString("name", def.name);
    reader.requireString("trade", def.trade);
    reader.optionalString("faction", def.factionId);
    reader.optionalString("archetype", def.archetypeId);
    reader.optionalString("room", def.moduleId);
    reader.optionalString("region", def.region);
    reader.optionalBool("lawless", def.lawless);
    reader.optionalBool("shadow", def.shadow);
    // The one anchor pair that cannot both be true, and it is the same pair
    // `[[system]]` already refuses: a major's space and a clan's space are the
    // two halves of one question, so naming both selects nothing and reads as
    // a typo rather than as a rule.
    if (!reader.failed && def.lawless && !def.factionId.empty()) {
        reader.fail("'lawless' and 'faction' are mutually exclusive");
    }
    if (!reader.failed && !def.region.empty() && def.region != "core" && def.region != "frontier" &&
        def.region != "fringe") {
        reader.fail("key 'region' must be \"core\", \"frontier\" or \"fringe\"");
    }
    reader.rejectUnknownKeys(
        {"id", "name", "trade", "faction", "archetype", "room", "region", "lawless", "shadow"});
    if (reader.failed) {
        return false;
    }
    mergeDef(out, std::move(def));
    return true;
}

} // namespace

// --- Mount vocabulary (Phase 31 stage A2) ---
//
// ⚑ The table is written ONCE and both directions read it, because a name list
// and a parser that drift apart is a mount kind that loads and then prints as
// something else. The order is `MountKind`'s, and the static_assert is what
// says so when somebody adds a kind to the enum and not to the table.
namespace {

constexpr const char* kMountKindNames[] = {"turret",
                                           "fixed",
                                           "launcher",
                                           "bay",
                                           "engine",
                                           "thruster",
                                           "shield",
                                           "armor",
                                           "utility",
                                           "subsystem",
                                           "hangar",
                                           "dock"};

constexpr const char* kMountSizeNames[] = {"small", "medium", "large", "xlarge"};

// ⚑ SIZED BY THE INITIALISER AND CHECKED AGAINST THE ENUM, rather than sized by
// the enum. `[kMountKindCount]` would accept a SHORT list and leave a null at
// the end, which `parseMountKind` then compares a `string_view` against -
// undefined behaviour reached by adding a member to an enum, surfacing as a
// crash inside somebody's def file rather than as a message about a table.
static_assert(std::size(kMountKindNames) == kMountKindCount, "a mount kind is missing its def spelling");
static_assert(std::size(kMountSizeNames) == kMountSizeCount, "a mount size is missing its def spelling");

} // namespace

const char* mountKindName(MountKind kind)
{
    const auto index = static_cast<std::size_t>(kind);
    return index < kMountKindCount ? kMountKindNames[index] : "?";
}

bool parseMountKind(std::string_view text, MountKind& out)
{
    for (std::size_t i = 0; i < kMountKindCount; ++i) {
        if (text == kMountKindNames[i]) {
            out = static_cast<MountKind>(i);
            return true;
        }
    }
    return false;
}

const char* mountSizeName(MountSize size)
{
    const auto index = static_cast<std::size_t>(size);
    return index < kMountSizeCount ? kMountSizeNames[index] : "?";
}

bool parseMountSize(std::string_view text, MountSize& out)
{
    for (std::size_t i = 0; i < kMountSizeCount; ++i) {
        if (text == kMountSizeNames[i]) {
            out = static_cast<MountSize>(i);
            return true;
        }
    }
    return false;
}

// --- the hull spine (Phase 32 stage A) ---------------------------------------

namespace {

constexpr const char* kHullRoleNames[] = {"line", "carrier", "logistics", "support", "covert", "industrial"};

static_assert(std::size(kHullRoleNames) == kHullRoleCount, "a hull role is missing its def spelling");

// gdd.md 11.1, copied straight out of the table and in its order. The upper
// bound is EXCLUSIVE so the eight bands partition the number line from 8 m up:
// exactly 20 m is Light, not Skiff, and there is no length two classes both
// claim. The titan band has no top.
constexpr float kHullClassBounds[] = {8.0f, 20.0f, 45.0f, 120.0f, 300.0f, 600.0f, 1200.0f, 3000.0f};

// ⚑ One word each, lowercase, as every other enumerated def value in this game
// is. 11.1 hyphenates "Super-capital" and this does not: a def spelling is a
// token an author types, and the hyphen is typography.
constexpr const char* kHullClassNames[] = {
    "skiff", "light", "medium", "heavy", "cruiser", "capital", "supercapital", "titan"};

static_assert(std::size(kHullClassBounds) == kHullClassCount, "a hull class is missing its band");
static_assert(std::size(kHullClassNames) == kHullClassCount, "a hull class is missing its def spelling");

// ⚑ The word WITHOUT its `ships_` prefix, so `builds_no = ["trader"]` reads as
// a sentence rather than as a list of key names. The prefix is how the roster
// is spelled; the cell is what it is called.
constexpr const char* kRosterCellNames[] = {"patrol", "raider", "trader"};

static_assert(std::size(kRosterCellNames) == kRosterCellCount, "a roster cell is missing its spelling");

// gdd.md 6's tiers, one lowercase word each. `component` and `assembly` are
// singular where the GDD's headings are plural, because a def value names what
// ONE good is - the same reason `size = "small"` is not `"smalls"`.
constexpr const char* kCommodityTierNames[] = {"raw", "refined", "component", "assembly", "consumer"};

static_assert(std::size(kCommodityTierNames) == kCommodityTierCount,
              "a commodity tier is missing its def spelling");

// gdd.md §12's eight families, one lowercase word each (Phase 34 stage A), and
// the same singular rule the tier names follow: a def value names what ONE
// module is for.
constexpr const char* kModuleFamilyNames[] = {
    "power", "habitat", "storage", "industry", "commerce", "recreation", "services", "shadow"};

static_assert(std::size(kModuleFamilyNames) == kModuleFamilyCount,
              "a module family is missing its def spelling");

// The dock screens a module can offer. Lowercase for the same reason every
// other def value is: these are tokens an author types, not headings.
constexpr const char* kStationScreenNames[] = {
    "trade", "outfitting", "shipyard", "crew", "factions", "missions", "survey", "refinery", "bar"};

static_assert(std::size(kStationScreenNames) == kStationScreenCount,
              "a station screen is missing its def spelling");

constexpr const char* kGoodsClassNames[] = {"bulk", "cryo", "hazardous"};

static_assert(std::size(kGoodsClassNames) == kGoodsClassCount, "a goods class is missing its def spelling");

} // namespace

const char* commodityTierName(CommodityTier tier)
{
    const auto index = static_cast<std::size_t>(tier);
    return index < kCommodityTierCount ? kCommodityTierNames[index] : "?";
}

bool parseCommodityTier(std::string_view text, CommodityTier& out)
{
    for (std::size_t i = 0; i < kCommodityTierCount; ++i) {
        if (text == kCommodityTierNames[i]) {
            out = static_cast<CommodityTier>(i);
            return true;
        }
    }
    return false;
}

// The three vocabularies Phase 34 stage A adds, all three the same shape as the
// commodity tier above: one table, one lookup each way, and a static assertion
// that the table has not fallen behind its enum.
const char* moduleFamilyName(ModuleFamily family)
{
    const auto index = static_cast<std::size_t>(family);
    return index < kModuleFamilyCount ? kModuleFamilyNames[index] : "?";
}

bool parseModuleFamily(std::string_view text, ModuleFamily& out)
{
    for (std::size_t i = 0; i < kModuleFamilyCount; ++i) {
        if (text == kModuleFamilyNames[i]) {
            out = static_cast<ModuleFamily>(i);
            return true;
        }
    }
    return false;
}

const char* stationScreenName(StationScreen screen)
{
    const auto index = static_cast<std::size_t>(screen);
    return index < kStationScreenCount ? kStationScreenNames[index] : "?";
}

bool parseStationScreen(std::string_view text, StationScreen& out)
{
    for (std::size_t i = 0; i < kStationScreenCount; ++i) {
        if (text == kStationScreenNames[i]) {
            out = static_cast<StationScreen>(i);
            return true;
        }
    }
    return false;
}

const char* goodsClassName(GoodsClass goods)
{
    const auto index = static_cast<std::size_t>(goods);
    return index < kGoodsClassCount ? kGoodsClassNames[index] : "?";
}

bool parseGoodsClass(std::string_view text, GoodsClass& out)
{
    for (std::size_t i = 0; i < kGoodsClassCount; ++i) {
        if (text == kGoodsClassNames[i]) {
            out = static_cast<GoodsClass>(i);
            return true;
        }
    }
    return false;
}

// ⚑ Written out rather than tabulated, because unlike the tier and roster-cell
// names these are NOT def spellings - no author ever types "contraband" as a
// value - so there is no parse function to keep them honest. They are display
// strings and a log's vocabulary, and the switch is what makes adding a state a
// compile error.
const char* legalityName(Legality legality)
{
    switch (legality) {
    case Legality::Unpoliced:
        return "unpoliced";
    case Legality::Legal:
        return "legal";
    case Legality::Restricted:
        return "restricted";
    case Legality::Contraband:
        return "contraband";
    }
    return "?";
}

Legality factionLegalityOf(const FactionDef& faction, std::string_view commodityId)
{
    // ⚑ Contraband first, and the parser refuses a good that is on both lists,
    // so the order is belt and braces rather than a precedence rule. If the two
    // ever could overlap, the forbidding one has to win - a licence that
    // overrode a ban would be a bug you only find by being shot.
    for (const std::string& id : faction.contraband) {
        if (id == commodityId) {
            return Legality::Contraband;
        }
    }
    for (const std::string& id : faction.restricted) {
        if (id == commodityId) {
            return Legality::Restricted;
        }
    }
    return Legality::Legal;
}

const char* rosterCellName(RosterCell cell)
{
    const auto index = static_cast<std::size_t>(cell);
    return index < kRosterCellCount ? kRosterCellNames[index] : "?";
}

bool parseRosterCell(std::string_view text, RosterCell& out)
{
    for (std::size_t i = 0; i < kRosterCellCount; ++i) {
        if (text == kRosterCellNames[i]) {
            out = static_cast<RosterCell>(i);
            return true;
        }
    }
    return false;
}

const char* hullRoleName(HullRole role)
{
    const auto index = static_cast<std::size_t>(role);
    return index < kHullRoleCount ? kHullRoleNames[index] : "?";
}

bool parseHullRole(std::string_view text, HullRole& out)
{
    for (std::size_t i = 0; i < kHullRoleCount; ++i) {
        if (text == kHullRoleNames[i]) {
            out = static_cast<HullRole>(i);
            return true;
        }
    }
    return false;
}

const char* hullClassName(HullClass hullClass)
{
    const auto index = static_cast<std::size_t>(hullClass);
    return index < kHullClassCount ? kHullClassNames[index] : "?";
}

bool parseHullClass(std::string_view text, HullClass& out)
{
    for (std::size_t i = 0; i < kHullClassCount; ++i) {
        if (text == kHullClassNames[i]) {
            out = static_cast<HullClass>(i);
            return true;
        }
    }
    return false;
}

HullClassBand hullClassBand(HullClass hullClass)
{
    const auto index = static_cast<std::size_t>(hullClass);
    if (index >= kHullClassCount) {
        return {};
    }
    HullClassBand band;
    band.minLength = kHullClassBounds[index];
    // ⚑ The top of the last band is infinity rather than a big number, because
    // a big number is a length a titan could be authored past. 11.1 writes it
    // as "3 km+" and this is that plus sign.
    band.maxLength =
        index + 1 < kHullClassCount ? kHullClassBounds[index + 1] : std::numeric_limits<float>::infinity();
    return band;
}

bool hullClassForLength(float metres, HullClass& out)
{
    if (!(metres >= kHullClassBounds[0])) {
        return false;
    }
    for (std::size_t i = kHullClassCount; i > 0; --i) {
        if (metres >= kHullClassBounds[i - 1]) {
            out = static_cast<HullClass>(i - 1);
            return true;
        }
    }
    return false;
}

bool hullLengthInBand(HullClass hullClass, float metres)
{
    const HullClassBand band = hullClassBand(hullClass);
    return band.maxLength > 0.0f && metres >= band.minLength && metres < band.maxLength;
}

const ShipMount* ShipDef::findMount(std::string_view mountId) const
{
    const auto it = std::find_if(
        mounts.begin(), mounts.end(), [&](const ShipMount& mount) { return mount.id == mountId; });
    return it != mounts.end() ? &*it : nullptr;
}

void DefDatabase::clear()
{
    m_ships.clear();
    m_weapons.clear();
    m_factions.clear();
    m_commodities.clear();
    m_stations.clear();
    m_modules.clear();
    // ⚑ `m_systems` was missed when stage A added it, and `m_constellations`
    // would have been missed the same way. Nothing calls `clear()` today, which
    // is exactly why the omission was invisible - so it is fixed rather than
    // left as a trap for the first caller.
    m_systems.clear();
    m_constellations.clear();
    m_components.clear();
    m_crew.clear();
    m_sounds.clear();
    m_models.clear();
    m_materials.clear();
    m_roles.clear();
    m_characters.clear();
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
    std::vector<SystemDef> systems = m_systems;
    std::vector<ConstellationDef> constellations = m_constellations;
    std::vector<ComponentDef> components = m_components;
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
    std::vector<CharacterDef> characters = m_characters;

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
        } else if (key == "system") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseSystem(t, s, *static_cast<std::vector<SystemDef>*>(v), e);
            };
            target = &systems;
        } else if (key == "constellation") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseConstellation(t, s, *static_cast<std::vector<ConstellationDef>*>(v), e);
            };
            target = &constellations;
        } else if (key == "component") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseComponent(t, s, *static_cast<std::vector<ComponentDef>*>(v), e);
            };
            target = &components;
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
        } else if (key == "character") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseCharacter(t, s, *static_cast<std::vector<CharacterDef>*>(v), e);
            };
            target = &characters;
        } else if (key == "role") {
            parse = [](const TomlValue& t, const char* s, void* v, std::string* e) {
                return parseRole(t, s, *static_cast<std::vector<RoleDef>*>(v), e);
            };
            target = &roles;
        } else {
            if (outError != nullptr) {
                *outError = std::string(sourceName) + ": unknown def kind '" + key +
                            "' (expected ship, weapon, faction, commodity, station, module, system, "
                            "constellation, component, crew, sound, model, material, role, or "
                            "character)";
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
    m_systems = std::move(systems);
    m_constellations = std::move(constellations);
    m_components = std::move(components);
    m_crew = std::move(crew);
    m_sounds = std::move(sounds);
    m_models = std::move(models);
    m_materials = std::move(materials);
    m_roles = std::move(roles);
    m_characters = std::move(characters);
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

bool DefDatabase::validateRosters(std::string* outError) const
{
    for (const FactionDef& faction : m_factions) {
        const std::vector<std::string>* rosters[kRosterCellCount] = {
            &faction.shipsPatrol, &faction.shipsRaider, &faction.shipsTrader};
        for (std::size_t i = 0; i < kRosterCellCount; ++i) {
            for (const std::string& id : *rosters[i]) {
                if (findShip(id.c_str()) != nullptr) {
                    continue;
                }
                if (outError != nullptr) {
                    *outError = faction.source + ": faction '" + faction.id + "' fields '" + id +
                                "' in its " + rosterCellName(static_cast<RosterCell>(i)) +
                                " roster, which no [[ship]] row defines";
                }
                return false;
            }
        }
    }
    return true;
}

bool DefDatabase::validateCatalogGates(std::string* outError) const
{
    // ⚑⚑ ALL FOUR SELLABLE KINDS, NOT JUST COMPONENTS. `requires` lives on the
    // shared `CatalogGate`, so a ship, a weapon, a component and a crew member
    // can all carry one - and a validator that checked only the kind that
    // happens to use it today would be a validator that stops working the first
    // time somebody authors the obvious second use.
    struct Row
    {
        const char* kind;
        const std::string& id;
        const std::string& requires_;
        const std::string& source;
    };
    std::vector<Row> rows;
    for (const ShipDef& def : m_ships) {
        rows.push_back({"ship", def.id, def.gate.requiresCommodity, def.source});
    }
    for (const WeaponDef& def : m_weapons) {
        rows.push_back({"weapon", def.id, def.gate.requiresCommodity, def.source});
    }
    for (const ComponentDef& def : m_components) {
        rows.push_back({"component", def.id, def.gate.requiresCommodity, def.source});
    }
    for (const CrewDef& def : m_crew) {
        rows.push_back({"crew", def.id, def.gate.requiresCommodity, def.source});
    }
    for (const Row& row : rows) {
        if (row.requires_.empty() || findCommodity(row.requires_.c_str()) != nullptr) {
            continue;
        }
        if (outError != nullptr) {
            *outError = row.source + ": " + row.kind + " '" + row.id + "' requires '" + row.requires_ +
                        "' to be in stock, which no [[commodity]] row defines";
        }
        return false;
    }
    return true;
}

// ⚑⚑ THE EXISTENCE HALF OF THE LEGALITY TABLE (Phase 33 stage D), and it is a
// database check rather than a parse one for `validateCatalogGates`'s reason: a
// mod may add a commodity in one file and the faction that outlaws it in
// another, so the only moment at which "does this good exist" has a true answer
// is after every layer has merged.
//
// ⚑ A law about a good that does not exist is not harmless the way an unused
// key is. It is a rule that can never fire, in a system whose whole job is to
// change what happens when a patrol looks in your hold - and the failure looks
// exactly like a patrol deciding to let you off.
bool DefDatabase::validateLegality(std::string* outError) const
{
    for (const FactionDef& faction : m_factions) {
        struct List
        {
            const char* key;
            const std::vector<std::string>& ids;
        };
        const List lists[] = {{"contraband", faction.contraband}, {"restricted", faction.restricted}};
        for (const List& list : lists) {
            for (const std::string& id : list.ids) {
                if (findCommodity(id.c_str()) != nullptr) {
                    continue;
                }
                if (outError != nullptr) {
                    *outError = faction.source + ": faction '" + faction.id + "' lists '" + id +
                                "' as " + list.key + ", which no [[commodity]] row defines";
                }
                return false;
            }
        }
    }
    return true;
}

bool DefDatabase::validateStationRecipes(std::string* outError) const
{
    for (const StationDef& station : m_stations) {
        for (const StationModuleEntry& entry : station.modules) {
            const ModuleDef* module = findModule(entry.moduleId.c_str());
            if (module == nullptr) {
                if (outError != nullptr) {
                    *outError = station.source + ": station '" + station.id + "' is composed of '" +
                                entry.moduleId + "', which no [[module]] row defines";
                }
                return false;
            }
            // ⚑⚑ POWER IS DERIVED, NOT AUTHORED, AND REFUSING IT HERE IS HOW AN
            // AUTHOR FINDS THAT OUT. The composer fits a plant to the draw a
            // recipe rolls, so a plant written into the recipe would be a
            // second plant nobody asked for - and the author who wrote it was
            // expecting a rule this game does not have.
            if (module->family == ModuleFamily::Power) {
                if (outError != nullptr) {
                    *outError = station.source + ": station '" + station.id + "' names the power module '" +
                                entry.moduleId +
                                "'; power is fitted to the composed draw, never authored into a recipe";
                }
                return false;
            }
        }
    }
    return true;
}

bool DefDatabase::validateCharacters(std::string* outError) const
{
    for (const CharacterDef& character : m_characters) {
        const auto refuse = [&](const std::string& what, const std::string& id, const char* kind) {
            if (outError != nullptr) {
                *outError = character.source + ": character '" + character.id + "' sits " + what + " '" + id +
                            "', which no [[" + kind + "]] row defines";
            }
        };
        if (!character.factionId.empty() && findFaction(character.factionId.c_str()) == nullptr) {
            refuse("in the space of", character.factionId, "faction");
            return false;
        }
        if (!character.archetypeId.empty() && findStation(character.archetypeId.c_str()) == nullptr) {
            refuse("on a", character.archetypeId, "station");
            return false;
        }
        if (!character.moduleId.empty() && findModule(character.moduleId.c_str()) == nullptr) {
            refuse("in a", character.moduleId, "module");
            return false;
        }
        // ⚑ A ROOM THAT IS NOT A ROOM, and it is worth a refusal of its own
        // rather than a shrug: `room = "sol.mod_fence"` parses, names a real
        // module, and selects nothing forever, because the seat a character
        // takes is by construction a RECREATION module - `stationRoom` picks the
        // recreation module with the largest draw and there is nothing else it
        // could return. An author who wrote it was expecting rooms to be
        // arbitrary modules, which is a rule this game does not have.
        if (!character.moduleId.empty()) {
            const ModuleDef* module = findModule(character.moduleId.c_str());
            if (module != nullptr && module->family != ModuleFamily::Recreation) {
                if (outError != nullptr) {
                    *outError = character.source + ": character '" + character.id + "' names the room '" +
                                character.moduleId + "', which is a " + moduleFamilyName(module->family) +
                                " module; a room is always a recreation module";
                }
                return false;
            }
        }
    }
    return true;
}

bool DefDatabase::validateSystems(std::string* outError) const
{
    // ⚑⚑ EVERY AUTHORED SYSTEM IN THE DATABASE, STANDALONE ROWS AND
    // CONSTELLATION MEMBERS ALIKE (Phase 29 stage C). A member is a system - it
    // names a faction, it names station archetypes, it can collide with another
    // system's name - so validating only `m_systems` would leave exactly half of
    // an authored galaxy unchecked, and the half a mod is most likely to break.
    struct Row
    {
        const SystemDef* system = nullptr;
        const ConstellationDef* group = nullptr; // null for a standalone [[system]]
    };

    std::vector<Row> rows;
    rows.reserve(m_systems.size() + m_constellations.size() * 3);
    for (const SystemDef& system : m_systems) {
        rows.push_back({&system, nullptr});
    }
    for (const ConstellationDef& group : m_constellations) {
        for (const SystemDef& member : group.members) {
            rows.push_back({&member, &group});
        }
    }

    const auto refuse = [&](const Row& row, const std::string& message) {
        if (outError != nullptr) {
            *outError = row.system->source +
                        (row.group != nullptr ? ": constellation '" + row.group->id + "'" : "") +
                        ": system '" + row.system->id + "': " + message;
        }
        return false;
    };
    // Is this id one a `jumps_from` may anchor on, and is it available yet?
    // ⚑ A CONSTELLATION MEMBER IS ALWAYS AVAILABLE, WHATEVER ORDER THE FILE
    // WAS WRITTEN IN, because a constellation cannot fail to be placed - it
    // makes its own nodes - so the generator resolves every member before any
    // rule runs. Def order only constrains anchors that could themselves fail.
    const auto isConstellationMember = [&](const std::string& id) {
        for (const ConstellationDef& group : m_constellations) {
            for (const SystemDef& member : group.members) {
                if (member.id == id) {
                    return true;
                }
            }
        }
        return false;
    };

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const SystemDef& system = *row.system;
        // ⚑ REFUSE rather than warn, per decision 3 and the `validateRoles`
        // precedent: warn-and-fall-back is right where a real thing is still
        // there to stand in, and here there is none. A system whose faction was
        // removed by a mod would silently become lawless, which is a different
        // place from the one the campaign was written against.
        if (system.hasFaction && findFaction(system.factionId.c_str()) == nullptr) {
            return refuse(row, "'faction' names '" + system.factionId + "', which is not a [[faction]]");
        }
        for (const AuthoredStationDef& station : system.stations) {
            if (findStation(station.stationId.c_str()) == nullptr) {
                return refuse(row,
                              "station '" + station.name + "' names archetype '" + station.stationId +
                                  "', which is not a [[station]]");
            }
        }
        // ⚑ `at_system` names a MAJOR, because a clan template claims nothing
        // and so is never handed a capital by `claimTerritory`. Refused rather
        // than warned for decision 3's reason: a system meant to be somebody's
        // home placed at random instead is a different place from the one the
        // campaign was written against.
        if (system.placement == "at_system") {
            const FactionDef* faction = findFaction(system.atSystemFactionId.c_str());
            if (faction == nullptr) {
                return refuse(
                    row, "'at_system' names '" + system.atSystemFactionId + "', which is not a [[faction]]");
            }
            if (faction->kind != FactionKind::Major) {
                return refuse(row,
                              "'at_system' names '" + system.atSystemFactionId +
                                  "', which is a clan template and holds no capital");
            }
        }
        // ⚑⚑ AN ANCHOR MUST BE DECLARED EARLIER, AND THAT IS CHECKED HERE
        // RATHER THAN LEFT TO THE GENERATOR, because def order is a fact about
        // the FILES and this is the only layer that can see all of them. The
        // generator's own "not placed before this one" refusal still stands
        // behind it and catches the case this cannot: an anchor that parsed
        // fine and then failed its own placement rule.
        if (system.placement == "jumps_from") {
            bool anchorIsAvailable = isConstellationMember(system.jumpsFromSystemId);
            for (std::size_t j = 0; !anchorIsAvailable && j < i; ++j) {
                if (rows[j].system->id == system.jumpsFromSystemId) {
                    anchorIsAvailable = true;
                }
            }
            if (!anchorIsAvailable) {
                bool existsAtAll = false;
                for (const Row& other : rows) {
                    if (other.system->id == system.jumpsFromSystemId) {
                        existsAtAll = true;
                        break;
                    }
                }
                return refuse(row,
                              "'jumps_from' anchors on '" + system.jumpsFromSystemId + "', which is " +
                                  (system.jumpsFromSystemId == system.id ? "this system itself"
                                   : existsAtAll ? "declared after it; an anchor must come first"
                                                 : "not a [[system]]"));
            }
        }
        // ⚑ TWO SYSTEMS CANNOT SHARE AN ID, AND ONLY THIS LAYER CAN SAY SO.
        // `mergeDef` keeps ids unique WITHIN a list by letting a later layer
        // replace an earlier one - but a constellation's members are merged with
        // their group rather than one at a time, so two members of one group, or
        // a member and a `[[system]]`, can both carry the same id and both
        // survive. `sol.system_by_id` would then answer with whichever the
        // generator reached first, which is not an answer.
        for (std::size_t j = 0; j < i; ++j) {
            if (rows[j].system->id == system.id) {
                return refuse(row,
                              "'id' is already used by another authored system" +
                                  std::string(rows[j].group != nullptr
                                                  ? " in constellation '" + rows[j].group->id + "'"
                                                  : ""));
            }
        }
        // Two authored systems fighting over one node is not resolvable in a
        // way either author would recognise as theirs, so it is a refusal here
        // rather than a rule about who wins.
        for (const Row& other : rows) {
            if (other.system != &system && other.system->hasName && system.hasName &&
                other.system->name == system.name && other.system->id != system.id) {
                return refuse(row, "'name' collides with system '" + other.system->id + "'");
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

const CharacterDef* DefDatabase::findCharacter(const char* id) const
{
    const auto it = std::find_if(
        m_characters.begin(), m_characters.end(), [&](const CharacterDef& d) { return d.id == id; });
    return it != m_characters.end() ? &*it : nullptr;
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

const SystemDef* DefDatabase::findSystem(const char* id) const
{
    const auto it =
        std::find_if(m_systems.begin(), m_systems.end(), [&](const SystemDef& d) { return d.id == id; });
    return it != m_systems.end() ? &*it : nullptr;
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

const ComponentDef* DefDatabase::findComponent(const char* id) const
{
    const auto it = std::find_if(
        m_components.begin(), m_components.end(), [&](const ComponentDef& d) { return d.id == id; });
    return it != m_components.end() ? &*it : nullptr;
}

const CrewDef* DefDatabase::findCrew(const char* id) const
{
    const auto it = std::find_if(m_crew.begin(), m_crew.end(), [&](const CrewDef& d) { return d.id == id; });
    return it != m_crew.end() ? &*it : nullptr;
}

} // namespace sol::assets
