#include "sol/assets/loadout.hpp"

#include <array>
#include <cstddef>

namespace sol::assets {

namespace {

constexpr std::size_t statIndex(FitStat stat)
{
    return static_cast<std::size_t>(stat);
}

// Base value of a stat on a def (the write side mirrors this in resolve).
float statValue(const ShipDef& def, FitStat stat)
{
    switch (stat) {
    case FitStat::ForwardAccel: return def.flight.forwardAccel;
    case FitStat::ReverseAccel: return def.flight.reverseAccel;
    case FitStat::LateralAccel: return def.flight.lateralAccel;
    case FitStat::VerticalAccel: return def.flight.verticalAccel;
    case FitStat::MaxSpeed: return def.flight.maxSpeed;
    case FitStat::TurnRate: return 1.0f; // uniform scale; handled separately
    case FitStat::CruiseSpeedScale: return def.flight.cruiseSpeedScale;
    case FitStat::ShieldStrength: return def.defense.shieldStrength;
    case FitStat::ShieldRegen: return def.defense.shieldRegen;
    case FitStat::Armor: return def.defense.armor;
    case FitStat::Hull: return def.defense.hull;
    case FitStat::WeaponCapacitor: return def.power.weaponCapacitor;
    case FitStat::WeaponRecharge: return def.power.weaponRecharge;
    case FitStat::Cargo: return def.cargoCapacity;
    case FitStat::Count: break;
    }
    return 0.0f;
}

void setStatValue(ShipDef& def, FitStat stat, float value)
{
    switch (stat) {
    case FitStat::ForwardAccel: def.flight.forwardAccel = value; return;
    case FitStat::ReverseAccel: def.flight.reverseAccel = value; return;
    case FitStat::LateralAccel: def.flight.lateralAccel = value; return;
    case FitStat::VerticalAccel: def.flight.verticalAccel = value; return;
    case FitStat::MaxSpeed: def.flight.maxSpeed = value; return;
    case FitStat::TurnRate: return; // handled separately (three axes)
    case FitStat::CruiseSpeedScale: def.flight.cruiseSpeedScale = value; return;
    case FitStat::ShieldStrength: def.defense.shieldStrength = value; return;
    case FitStat::ShieldRegen: def.defense.shieldRegen = value; return;
    case FitStat::Armor: def.defense.armor = value; return;
    case FitStat::Hull: def.defense.hull = value; return;
    case FitStat::WeaponCapacitor: def.power.weaponCapacitor = value; return;
    case FitStat::WeaponRecharge: def.power.weaponRecharge = value; return;
    case FitStat::Cargo: def.cargoCapacity = value; return;
    case FitStat::Count: break;
    }
}

struct Accumulated
{
    std::array<float, kFitStatCount> add{};
    std::array<float, kFitStatCount> mul = StatModifiers::ones();
    float moduleMass = 0.0f;
};

void accumulate(Accumulated& acc, const StatModifiers& mods)
{
    for (std::size_t i = 0; i < kFitStatCount; ++i) {
        acc.add[i] += mods.add[i];
        acc.mul[i] *= mods.mul[i];
    }
}

} // namespace

bool validateLoadout(const ShipDef& ship, std::span<const ModuleDef* const> modules,
                     std::span<const CrewDef* const> crew, std::string* outError)
{
    std::array<std::uint32_t, kModuleSlotCount> used{};
    float powerDraw = 0.0f;
    for (const ModuleDef* module : modules) {
        if (module == nullptr) {
            continue;
        }
        ++used[static_cast<std::size_t>(module->slot)];
        powerDraw += module->powerDraw;
    }

    const std::array<std::uint32_t, kModuleSlotCount> limits = {
        ship.slotsShield, ship.slotsEngine, ship.slotsCargo, ship.slotsUtility};
    constexpr const char* kSlotNames[kModuleSlotCount] = {"shield", "engine", "cargo",
                                                          "utility"};
    for (std::size_t i = 0; i < kModuleSlotCount; ++i) {
        if (used[i] > limits[i]) {
            if (outError != nullptr) {
                *outError = std::string("too many ") + kSlotNames[i] + " modules (" +
                            std::to_string(used[i]) + "/" + std::to_string(limits[i]) + ")";
            }
            return false;
        }
    }

    if (powerDraw > ship.powerOutput) {
        if (outError != nullptr) {
            *outError = "power budget exceeded (" + std::to_string(powerDraw) + "/" +
                        std::to_string(ship.powerOutput) + ")";
        }
        return false;
    }

    std::uint32_t crewCount = 0;
    for (const CrewDef* member : crew) {
        if (member != nullptr) {
            ++crewCount;
        }
    }
    if (crewCount > ship.crewBerths) {
        if (outError != nullptr) {
            *outError = "not enough crew berths (" + std::to_string(crewCount) + "/" +
                        std::to_string(ship.crewBerths) + ")";
        }
        return false;
    }
    return true;
}

ShipDef resolveLoadout(const ShipDef& base, std::span<const ModuleDef* const> modules,
                       std::span<const CrewDef* const> crew)
{
    Accumulated acc;
    for (const ModuleDef* module : modules) {
        if (module != nullptr) {
            accumulate(acc, module->modifiers);
            acc.moduleMass += module->mass;
        }
    }
    for (const CrewDef* member : crew) {
        if (member != nullptr) {
            accumulate(acc, member->modifiers);
        }
    }

    ShipDef def = base;
    for (std::size_t i = 0; i < kFitStatCount; ++i) {
        const auto stat = static_cast<FitStat>(i);
        if (stat == FitStat::TurnRate) {
            continue;
        }
        setStatValue(def, stat, (statValue(base, stat) + acc.add[i]) * acc.mul[i]);
    }

    // TurnRate applies uniformly across the three axes: add is rad/s on each,
    // mul scales each.
    const std::size_t turn = statIndex(FitStat::TurnRate);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        def.flight.maxTurnRate[axis] =
            (base.flight.maxTurnRate[axis] + acc.add[turn]) * acc.mul[turn];
    }

    // Mass penalty: module mass dilutes linear and angular acceleration.
    const float massFactor = base.mass / (base.mass + acc.moduleMass);
    def.flight.forwardAccel *= massFactor;
    def.flight.reverseAccel *= massFactor;
    def.flight.lateralAccel *= massFactor;
    def.flight.verticalAccel *= massFactor;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        def.flight.angularAccel[axis] *= massFactor;
    }
    return def;
}

} // namespace sol::assets
