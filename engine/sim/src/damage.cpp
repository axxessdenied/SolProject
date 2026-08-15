#include "sol/sim/damage.hpp"

namespace sol::sim {

ShieldFacing facingForHit(const core::Quat& orientation, const core::DVec3& toSource)
{
    const core::Vec3 forward = rotate(orientation, core::Vec3{0.0f, 0.0f, -1.0f});
    const double facing = static_cast<double>(forward.x) * toSource.x +
                          static_cast<double>(forward.y) * toSource.y +
                          static_cast<double>(forward.z) * toSource.z;
    return facing >= 0.0 ? ShieldFacing::Fore : ShieldFacing::Aft;
}

void resetDefense(DefenseState& state, const DefenseTuning& tuning)
{
    state.shieldFore = tuning.shieldStrength;
    state.shieldAft = tuning.shieldStrength;
    state.regenDelayFore = 0.0f;
    state.regenDelayAft = 0.0f;
    state.armor = tuning.armor;
    state.hull = tuning.hull;
}

DamageResult applyDamage(DefenseState& state, const DefenseTuning& tuning, ShieldFacing facing,
                         float amount)
{
    DamageResult result;
    if (amount <= 0.0f || !state.alive()) {
        return result;
    }

    float& shield = facing == ShieldFacing::Fore ? state.shieldFore : state.shieldAft;
    float& delay = facing == ShieldFacing::Fore ? state.regenDelayFore : state.regenDelayAft;
    delay = tuning.shieldRegenDelay;

    if (shield > 0.0f) {
        result.shieldAbsorbed = shield < amount ? shield : amount;
        shield -= result.shieldAbsorbed;
        amount -= result.shieldAbsorbed;
        if (shield <= 0.0f) {
            shield = 0.0f;
            result.shieldCollapsed = true;
        }
    }
    if (amount > 0.0f && state.armor > 0.0f) {
        result.armorAbsorbed = state.armor < amount ? state.armor : amount;
        state.armor -= result.armorAbsorbed;
        amount -= result.armorAbsorbed;
    }
    if (amount > 0.0f) {
        result.hullDamage = state.hull < amount ? state.hull : amount;
        state.hull -= result.hullDamage;
        if (state.hull <= 0.0f) {
            state.hull = 0.0f;
            result.destroyed = true;
        }
    }
    return result;
}

void stepDefense(DefenseState& state, const DefenseTuning& tuning, float regenScale, double dt)
{
    if (!state.alive()) {
        return;
    }
    const float step = static_cast<float>(dt);
    const float regen = tuning.shieldRegenRate * regenScale * step;

    auto regenFacing = [&](float& shield, float& delay) {
        if (delay > 0.0f) {
            delay = delay > step ? delay - step : 0.0f;
            return;
        }
        shield += regen;
        if (shield > tuning.shieldStrength) {
            shield = tuning.shieldStrength;
        }
    };
    regenFacing(state.shieldFore, state.regenDelayFore);
    regenFacing(state.shieldAft, state.regenDelayAft);
}

} // namespace sol::sim
