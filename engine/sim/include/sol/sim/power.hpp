#pragma once

// Elite-style power pips (docs/decisions/003-power-management.md): a fixed
// pip budget split across WEP / ENG / SYS, adjusted live with single
// keystrokes. Allocation scales weapon-capacitor recharge, the thrust/speed
// envelope, and shield regeneration through per-ship response curves.

#include "sol/sim/flight.hpp"

namespace sol::sim {

enum class PowerSystem
{
    Weapons,
    Engines,
    Shields,
};

struct PowerPips
{
    int weapons = 2;
    int engines = 2;
    int shields = 2;

    [[nodiscard]] constexpr bool operator==(const PowerPips&) const = default;
};

struct PowerTuning
{
    int pipCapacity = 6; // total distributed pips (invariant: sum == capacity)
    int maxPerSystem = 4;

    // Linear response between the 0-pip and max-pip scale factors.
    float weaponRechargeAtZero = 0.2f;
    float weaponRechargeAtMax = 1.6f;
    float engineAtZero = 0.8f;
    float engineAtMax = 1.15f;
    float shieldRegenAtZero = 0.25f;
    float shieldRegenAtMax = 1.5f;

    float weaponCapacitor = 100.0f;   // energy units
    float weaponRechargeRate = 15.0f; // units/s at scale 1
};

struct PowerState
{
    PowerPips pips;
    float weaponCharge = 100.0f; // current capacitor energy
};

// Adds one pip to `target`, taking it from the fullest other system (ties
// break in WEP, ENG, SYS order). No-op when target is at maxPerSystem or the
// others are empty.
void addPip(PowerPips& pips, PowerSystem target, const PowerTuning& tuning);

// Resets to the even split of pipCapacity (remainders go WEP, ENG, SYS).
void balancePips(PowerPips& pips, const PowerTuning& tuning);

[[nodiscard]] float weaponRechargeScale(const PowerPips& pips, const PowerTuning& tuning);
[[nodiscard]] float engineScale(const PowerPips& pips, const PowerTuning& tuning);
[[nodiscard]] float shieldRegenScale(const PowerPips& pips, const PowerTuning& tuning);

// Returns tuning with linear acceleration and the assist cap scaled by the
// current ENG allocation; turn rates are deliberately unaffected.
[[nodiscard]] ShipTuning
applyEnginePips(const ShipTuning& tuning, const PowerPips& pips, const PowerTuning& power);

// Recharges the weapon capacitor by WEP allocation.
void stepPower(PowerState& state, const PowerTuning& tuning, double dt);

// Draws energy for a shot; false (and no draw) when not enough charge.
[[nodiscard]] bool drawWeaponCharge(PowerState& state, float amount);

} // namespace sol::sim
