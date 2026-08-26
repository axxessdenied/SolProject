#include <cmath>

#include <sol/sim/damage.hpp>
#include <sol/test/test.hpp>

using sol::core::DVec3;
using sol::core::Quat;
using sol::sim::applyDamage;
using sol::sim::DamageResult;
using sol::sim::DefenseState;
using sol::sim::DefenseTuning;
using sol::sim::facingForHit;
using sol::sim::resetDefense;
using sol::sim::ShieldFacing;
using sol::sim::stepDefense;

SOL_TEST(damage_facing_resolution)
{
    // Identity orientation: ship nose is -Z.
    const Quat identity = Quat::identity();
    SOL_CHECK(facingForHit(identity, {0.0, 0.0, -1.0}) == ShieldFacing::Fore); // ahead
    SOL_CHECK(facingForHit(identity, {0.0, 0.0, 1.0}) == ShieldFacing::Aft);   // behind
    // Hits from the side bucket by the forward component's sign.
    SOL_CHECK(facingForHit(identity, {1.0, 0.0, -0.01}) == ShieldFacing::Fore);
    SOL_CHECK(facingForHit(identity, {1.0, 0.0, 0.01}) == ShieldFacing::Aft);
}

SOL_TEST(damage_layers_absorb_in_order)
{
    const DefenseTuning tuning; // 100 shield/facing, 50 armor, 100 hull
    DefenseState state;
    resetDefense(state, tuning);

    // 30 into the fore shield only.
    DamageResult hit = applyDamage(state, tuning, ShieldFacing::Fore, 30.0f);
    SOL_CHECK(hit.shieldAbsorbed == 30.0f && hit.armorAbsorbed == 0.0f && hit.hullDamage == 0.0f);
    SOL_CHECK(!hit.shieldCollapsed && state.shieldFore == 70.0f && state.shieldAft == 100.0f);

    // 120 more: collapses the facing (70), eats armor (50), hull untouched.
    hit = applyDamage(state, tuning, ShieldFacing::Fore, 120.0f);
    SOL_CHECK(hit.shieldCollapsed);
    SOL_CHECK(hit.shieldAbsorbed == 70.0f && hit.armorAbsorbed == 50.0f && hit.hullDamage == 0.0f);
    SOL_CHECK(state.armor == 0.0f && state.hull == 100.0f);

    // The aft facing is unaffected; a hit from behind hits full shield.
    hit = applyDamage(state, tuning, ShieldFacing::Aft, 10.0f);
    SOL_CHECK(hit.shieldAbsorbed == 10.0f && state.shieldAft == 90.0f);

    // Fore is naked now: straight to hull, and enough kills.
    hit = applyDamage(state, tuning, ShieldFacing::Fore, 150.0f);
    SOL_CHECK(hit.hullDamage == 100.0f && hit.destroyed);
    SOL_CHECK(!state.alive());

    // Dead ships take no further damage.
    hit = applyDamage(state, tuning, ShieldFacing::Fore, 50.0f);
    SOL_CHECK(hit.hullDamage == 0.0f && !hit.destroyed);
}

SOL_TEST(damage_regen_per_facing_after_delay)
{
    const DefenseTuning tuning; // regen 8/s, delay 4 s
    DefenseState state;
    resetDefense(state, tuning);
    (void)applyDamage(state, tuning, ShieldFacing::Fore, 40.0f); // fore 60, delay armed

    // During the delay nothing regenerates on fore; aft stays full.
    for (int i = 0; i < 60 * 4; ++i) {
        stepDefense(state, tuning, 1.0f, 1.0 / 60.0);
    }
    SOL_CHECK(std::abs(state.shieldFore - 60.0f) < 0.5f);

    // After the delay, 1 s at scale 2 regenerates 16.
    for (int i = 0; i < 60; ++i) {
        stepDefense(state, tuning, 2.0f, 1.0 / 60.0);
    }
    SOL_CHECK(std::abs(state.shieldFore - 76.0f) < 0.5f);
    SOL_CHECK(state.shieldAft == 100.0f); // clamped at strength all along

    // Long regen clamps at full.
    for (int i = 0; i < 60 * 60; ++i) {
        stepDefense(state, tuning, 1.0f, 1.0 / 60.0);
    }
    SOL_CHECK(state.shieldFore == 100.0f);
}

SOL_TEST(damage_absorbing_hit_still_restarts_delay)
{
    const DefenseTuning tuning;
    DefenseState state;
    resetDefense(state, tuning);
    (void)applyDamage(state, tuning, ShieldFacing::Aft, 10.0f);
    SOL_CHECK(state.regenDelayAft == tuning.shieldRegenDelay);
    SOL_CHECK(state.regenDelayFore == 0.0f);
}
