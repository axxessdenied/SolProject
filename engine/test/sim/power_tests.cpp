#include <cmath>

#include <sol/sim/power.hpp>
#include <sol/test/test.hpp>

using sol::sim::addPip;
using sol::sim::balancePips;
using sol::sim::PowerPips;
using sol::sim::PowerState;
using sol::sim::PowerSystem;
using sol::sim::PowerTuning;

SOL_TEST(power_add_pip_steals_from_fullest)
{
    const PowerTuning tuning;
    PowerPips pips; // 2/2/2
    addPip(pips, PowerSystem::Weapons, tuning);
    SOL_CHECK(pips == PowerPips{3, 1, 2}); // tie between ENG/SYS -> ENG donates first
    addPip(pips, PowerSystem::Weapons, tuning);
    SOL_CHECK(pips == PowerPips{4, 1, 1}); // SYS is now fullest donor
    // WEP is at maxPerSystem; further adds are no-ops.
    addPip(pips, PowerSystem::Weapons, tuning);
    SOL_CHECK(pips == PowerPips{4, 1, 1});
    // Budget invariant held throughout.
    SOL_CHECK(pips.weapons + pips.engines + pips.shields == tuning.pipCapacity);
}

SOL_TEST(power_add_pip_with_empty_donors_is_noop)
{
    const PowerTuning tuning;
    PowerPips pips{2, 4, 0};
    addPip(pips, PowerSystem::Engines, tuning); // ENG at max already
    SOL_CHECK(pips == PowerPips{2, 4, 0});
    PowerPips drained{6, 0, 0};
    const PowerTuning bigMax{.maxPerSystem = 6};
    addPip(drained, PowerSystem::Weapons, bigMax); // others empty
    SOL_CHECK(drained == PowerPips{6, 0, 0});
}

SOL_TEST(power_balance_resets_even)
{
    const PowerTuning tuning;
    PowerPips pips{4, 1, 1};
    balancePips(pips, tuning);
    SOL_CHECK(pips == PowerPips{2, 2, 2});

    const PowerTuning odd{.pipCapacity = 7};
    balancePips(pips, odd);
    SOL_CHECK(pips == PowerPips{3, 2, 2}); // remainder goes to WEP first
}

SOL_TEST(power_response_scales_interpolate)
{
    const PowerTuning tuning;
    const PowerPips zero{0, 0, 6};
    const PowerPips full{4, 4, 4};
    SOL_CHECK(std::abs(weaponRechargeScale(zero, tuning) - 0.2f) < 1e-6f);
    SOL_CHECK(std::abs(weaponRechargeScale(full, tuning) - 1.6f) < 1e-6f);
    SOL_CHECK(std::abs(engineScale(PowerPips{2, 2, 2}, tuning) - (0.8f + (1.15f - 0.8f) * 0.5f)) < 1e-6f);
    SOL_CHECK(std::abs(shieldRegenScale(zero, tuning) - 1.5f) < 1e-6f); // 6 clamps to max 4
}

SOL_TEST(power_engine_pips_scale_tuning)
{
    const PowerTuning power;
    sol::sim::ShipTuning tuning; // forward 60, maxSpeed 220
    const sol::sim::ShipTuning boosted = applyEnginePips(tuning, PowerPips{0, 4, 2}, power);
    SOL_CHECK(std::abs(boosted.forwardAccel - 60.0f * 1.15f) < 1e-4f);
    SOL_CHECK(std::abs(boosted.maxSpeed - 220.0f * 1.15f) < 1e-4f);
    SOL_CHECK(boosted.maxTurnRate == tuning.maxTurnRate); // turn rates untouched

    const sol::sim::ShipTuning starved = applyEnginePips(tuning, PowerPips{4, 0, 2}, power);
    SOL_CHECK(std::abs(starved.maxSpeed - 220.0f * 0.8f) < 1e-4f);
}

SOL_TEST(power_capacitor_recharge_and_draw)
{
    const PowerTuning tuning; // 100 cap, 15/s at scale 1
    PowerState state{.pips = {4, 1, 1}, .weaponCharge = 0.0f};

    // 1 second at max WEP: 15 * 1.6 = 24 units.
    for (int i = 0; i < 60; ++i) {
        stepPower(state, tuning, 1.0 / 60.0);
    }
    SOL_CHECK(std::abs(state.weaponCharge - 24.0f) < 1e-3f);

    SOL_CHECK(drawWeaponCharge(state, 20.0f));
    SOL_CHECK(std::abs(state.weaponCharge - 4.0f) < 1e-3f);
    SOL_CHECK(!drawWeaponCharge(state, 5.0f)); // not enough; unchanged
    SOL_CHECK(std::abs(state.weaponCharge - 4.0f) < 1e-3f);

    // Recharge clamps at capacity.
    for (int i = 0; i < 60 * 600; ++i) {
        stepPower(state, tuning, 1.0 / 60.0);
    }
    SOL_CHECK(state.weaponCharge == 100.0f);
}
