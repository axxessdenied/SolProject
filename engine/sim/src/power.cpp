#include "sol/sim/power.hpp"

#include "sol/core/math/math.hpp"

#include <initializer_list>

namespace sol::sim {

namespace {

[[nodiscard]] int& pipsFor(PowerPips& pips, PowerSystem system)
{
    switch (system) {
    case PowerSystem::Weapons:
        return pips.weapons;
    case PowerSystem::Engines:
        return pips.engines;
    case PowerSystem::Shields:
        return pips.shields;
    }
    return pips.weapons;
}

[[nodiscard]] float responseScale(int pips, int maxPerSystem, float atZero, float atMax)
{
    const float t = maxPerSystem > 0 ? static_cast<float>(pips) / static_cast<float>(maxPerSystem) : 0.0f;
    return core::lerp(atZero, atMax, core::clamp(t, 0.0f, 1.0f));
}

} // namespace

void addPip(PowerPips& pips, PowerSystem target, const PowerTuning& tuning)
{
    int& targetPips = pipsFor(pips, target);
    if (targetPips >= tuning.maxPerSystem) {
        return;
    }
    // Donor: fullest of the other two, ties in WEP, ENG, SYS order.
    constexpr PowerSystem kOrder[] = {PowerSystem::Weapons, PowerSystem::Engines, PowerSystem::Shields};
    int* donor = nullptr;
    for (const PowerSystem system : kOrder) {
        if (system == target) {
            continue;
        }
        int& candidate = pipsFor(pips, system);
        if (donor == nullptr || candidate > *donor) {
            donor = &candidate;
        }
    }
    if (donor == nullptr || *donor <= 0) {
        return;
    }
    --*donor;
    ++targetPips;
}

void balancePips(PowerPips& pips, const PowerTuning& tuning)
{
    const int base = tuning.pipCapacity / 3;
    int remainder = tuning.pipCapacity - base * 3;
    pips = {base, base, base};
    for (int* system : {&pips.weapons, &pips.engines, &pips.shields}) {
        if (remainder > 0) {
            ++*system;
            --remainder;
        }
    }
}

float weaponRechargeScale(const PowerPips& pips, const PowerTuning& tuning)
{
    return responseScale(
        pips.weapons, tuning.maxPerSystem, tuning.weaponRechargeAtZero, tuning.weaponRechargeAtMax);
}

float engineScale(const PowerPips& pips, const PowerTuning& tuning)
{
    return responseScale(pips.engines, tuning.maxPerSystem, tuning.engineAtZero, tuning.engineAtMax);
}

float shieldRegenScale(const PowerPips& pips, const PowerTuning& tuning)
{
    return responseScale(
        pips.shields, tuning.maxPerSystem, tuning.shieldRegenAtZero, tuning.shieldRegenAtMax);
}

ShipTuning applyEnginePips(const ShipTuning& tuning, const PowerPips& pips, const PowerTuning& power)
{
    const float scale = engineScale(pips, power);
    ShipTuning scaled = tuning;
    scaled.forwardAccel *= scale;
    scaled.reverseAccel *= scale;
    scaled.lateralAccel *= scale;
    scaled.verticalAccel *= scale;
    scaled.maxSpeed *= scale;
    return scaled;
}

void stepPower(PowerState& state, const PowerTuning& tuning, double dt)
{
    const float recharge =
        tuning.weaponRechargeRate * weaponRechargeScale(state.pips, tuning) * static_cast<float>(dt);
    state.weaponCharge = core::clamp(state.weaponCharge + recharge, 0.0f, tuning.weaponCapacitor);
}

bool drawWeaponCharge(PowerState& state, float amount)
{
    if (state.weaponCharge < amount) {
        return false;
    }
    state.weaponCharge -= amount;
    return true;
}

} // namespace sol::sim
