#include <sol/assets/data_defs.hpp>
#include <sol/assets/loadout.hpp>

#include <sol/test/test.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using sol::assets::CrewDef;
using sol::assets::DefDatabase;
using sol::assets::ModuleDef;
using sol::assets::ShipDef;

namespace {

bool merge(DefDatabase& db, const char* toml, const char* source, std::string* outError = nullptr)
{
    return db.mergeToml(toml, std::strlen(toml), source, outError);
}

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 1.0e-4f;
}

constexpr const char* kOutfittingDefs = R"(
[[ship]]
id = "sol.testbed"
name = "Testbed"
forward_accel = 100.0
max_speed = 200.0
shield_strength = 100.0
cargo = 50.0
price = 8000.0
mass = 10000.0
power_output = 6.0
slots_shield = 1
slots_engine = 1
slots_cargo = 2
slots_utility = 1
crew_berths = 1

[[module]]
id = "sol.shield_mk1"
name = "Shield Booster Mk1"
slot = "shield"
price = 500.0
mass = 500.0
power_draw = 2.0
shield_strength_mul = 1.5
shield_strength_add = 20.0

[[module]]
id = "sol.cargo_pod"
name = "Cargo Pod"
slot = "cargo"
price = 300.0
mass = 1000.0
power_draw = 0.0
cargo_add = 25.0

[[module]]
id = "sol.hungry_reactor_sink"
name = "Hungry Sink"
slot = "utility"
power_draw = 100.0

[[crew]]
id = "sol.engineer_kim"
name = "Kim"
role = "Engineer"
price = 400.0
shield_regen_mul = 1.1
)";

} // namespace

SOL_TEST(loadout_parse_modules_and_crew)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    SOL_CHECK(db.modules().size() == 3);
    SOL_CHECK(db.crew().size() == 1);

    const ModuleDef* shield = db.findModule("sol.shield_mk1");
    SOL_CHECK(shield != nullptr);
    if (shield == nullptr) {
        return;
    }
    SOL_CHECK(shield->slot == sol::assets::ModuleSlot::Shield);
    SOL_CHECK(shield->price == 500.0f);
    const auto statIdx = static_cast<std::size_t>(sol::assets::FitStat::ShieldStrength);
    SOL_CHECK(shield->modifiers.mul[statIdx] == 1.5f);
    SOL_CHECK(shield->modifiers.add[statIdx] == 20.0f);

    const CrewDef* kim = db.findCrew("sol.engineer_kim");
    SOL_CHECK(kim != nullptr);
    if (kim == nullptr) {
        return;
    }
    SOL_CHECK(kim->role == "Engineer");
    const auto regenIdx = static_cast<std::size_t>(sol::assets::FitStat::ShieldRegen);
    SOL_CHECK(nearlyEqual(kim->modifiers.mul[regenIdx], 1.1f));

    const ShipDef* ship = db.findShip("sol.testbed");
    SOL_CHECK(ship != nullptr);
    if (ship == nullptr) {
        return;
    }
    SOL_CHECK(ship->price == 8000.0f);
    SOL_CHECK(ship->slotsCargo == 2);
    SOL_CHECK(ship->crewBerths == 1);
}

SOL_TEST(loadout_rejects_bad_slot_and_unknown_modifier)
{
    DefDatabase db;
    std::string error;
    const char* badSlot = R"(
[[module]]
id = "sol.bad"
name = "Bad"
slot = "hyperdrive"
)";
    SOL_CHECK(!merge(db, badSlot, "bad.toml", &error));
    SOL_CHECK(error.find("slot") != std::string::npos);

    const char* badKey = R"(
[[module]]
id = "sol.bad2"
name = "Bad2"
slot = "utility"
warp_factor_mul = 2.0
)";
    SOL_CHECK(!merge(db, badKey, "bad2.toml", &error));
    SOL_CHECK(error.find("warp_factor_mul") != std::string::npos);
}

SOL_TEST(loadout_resolve_adds_then_muls)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    const ShipDef* base = db.findShip("sol.testbed");
    const ModuleDef* shield = db.findModule("sol.shield_mk1");
    SOL_CHECK(base != nullptr && shield != nullptr);
    if (base == nullptr || shield == nullptr) {
        return;
    }

    const ModuleDef* modules[] = {shield};
    const ShipDef effective = sol::assets::resolveLoadout(*base, modules, {});
    // (100 + 20) * 1.5 — adds before muls.
    SOL_CHECK(nearlyEqual(effective.defense.shieldStrength, 180.0f));
    // Mass penalty: 10000 / 10500 on accelerations only.
    SOL_CHECK(nearlyEqual(effective.flight.forwardAccel, 100.0f * 10'000.0f / 10'500.0f));
    SOL_CHECK(effective.flight.maxSpeed == 200.0f); // untouched stat
    SOL_CHECK(effective.price == base->price);      // identity preserved
}

SOL_TEST(loadout_resolve_is_order_independent)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    const ShipDef* base = db.findShip("sol.testbed");
    const ModuleDef* shield = db.findModule("sol.shield_mk1");
    const ModuleDef* pod = db.findModule("sol.cargo_pod");
    SOL_CHECK(base != nullptr && shield != nullptr && pod != nullptr);
    if (base == nullptr || shield == nullptr || pod == nullptr) {
        return;
    }

    const ModuleDef* ab[] = {shield, pod};
    const ModuleDef* ba[] = {pod, shield};
    const ShipDef first = sol::assets::resolveLoadout(*base, ab, {});
    const ShipDef second = sol::assets::resolveLoadout(*base, ba, {});
    SOL_CHECK(first.defense.shieldStrength == second.defense.shieldStrength);
    SOL_CHECK(first.cargoCapacity == second.cargoCapacity);
    SOL_CHECK(first.flight.forwardAccel == second.flight.forwardAccel);
    SOL_CHECK(nearlyEqual(first.cargoCapacity, 75.0f));
}

SOL_TEST(loadout_validate_budgets)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    const ShipDef* ship = db.findShip("sol.testbed");
    const ModuleDef* shield = db.findModule("sol.shield_mk1");
    const ModuleDef* pod = db.findModule("sol.cargo_pod");
    const ModuleDef* sink = db.findModule("sol.hungry_reactor_sink");
    const CrewDef* kim = db.findCrew("sol.engineer_kim");
    SOL_CHECK(ship != nullptr && shield != nullptr && pod != nullptr && sink != nullptr &&
              kim != nullptr);
    if (ship == nullptr || shield == nullptr || pod == nullptr || sink == nullptr ||
        kim == nullptr) {
        return;
    }

    // A legal fit: 1 shield + 2 cargo pods, 2.0/6.0 power, 1 crew.
    const ModuleDef* legal[] = {shield, pod, pod};
    const CrewDef* crew[] = {kim};
    SOL_CHECK(sol::assets::validateLoadout(*ship, legal, crew, &error));

    // Slot overflow: two shield modules in one shield slot.
    const ModuleDef* tooManyShields[] = {shield, shield};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, tooManyShields, {}, &error));
    SOL_CHECK(error.find("shield") != std::string::npos);

    // Power overflow.
    const ModuleDef* tooHungry[] = {sink};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, tooHungry, {}, &error));
    SOL_CHECK(error.find("power") != std::string::npos);

    // Berth overflow.
    const CrewDef* tooManyCrew[] = {kim, kim};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, {}, tooManyCrew, &error));
    SOL_CHECK(error.find("berth") != std::string::npos);
}
