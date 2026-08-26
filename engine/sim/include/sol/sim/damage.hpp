#pragma once

// Damage model (engine plan Phase 6, GDD §5): regenerating directional
// shields (docs/decisions/002-shield-facings.md - fore/aft baseline) over
// ablative armor over hull. Armor is a single pool for now; the GDD's
// locational armor needs hit zones tied to real geometry, which spheres
// can't provide - revisit with the model-based collision pass.

#include "sol/core/math/math.hpp"

namespace sol::sim {

enum class ShieldFacing
{
    Fore,
    Aft,
};

struct DefenseTuning
{
    float shieldStrength = 100.0f; // hp per facing
    float shieldRegenRate = 8.0f;  // hp/s at regen scale 1 (SYS pips scale it)
    float shieldRegenDelay = 4.0f; // seconds a facing stays down after a hit
    float armor = 50.0f;           // ablative, does not regenerate
    float hull = 100.0f;
};

struct DefenseState
{
    float shieldFore = 100.0f;
    float shieldAft = 100.0f;
    float regenDelayFore = 0.0f; // seconds until the facing may regen
    float regenDelayAft = 0.0f;
    float armor = 50.0f;
    float hull = 100.0f;

    [[nodiscard]] bool alive() const { return hull > 0.0f; }
};

// What a hit did, for combat feedback and AI threat logic.
struct DamageResult
{
    float shieldAbsorbed = 0.0f;
    float armorAbsorbed = 0.0f;
    float hullDamage = 0.0f;
    bool shieldCollapsed = false; // this hit took the facing to zero
    bool destroyed = false;       // hull reached zero
};

// Facing for a hit arriving from toSource (unit-ish vector, ship position
// toward the damage source, sim space), given the ship's orientation.
[[nodiscard]] ShieldFacing facingForHit(const core::Quat& orientation, const core::DVec3& toSource);

void resetDefense(DefenseState& state, const DefenseTuning& tuning);

// Shield facing -> armor -> hull. The hit facing's regen delay restarts even
// when shields absorb everything.
DamageResult applyDamage(DefenseState& state, const DefenseTuning& tuning, ShieldFacing facing, float amount);

// Regenerates shields (per facing, after its delay) by regenScale (SYS pips).
void stepDefense(DefenseState& state, const DefenseTuning& tuning, float regenScale, double dt);

} // namespace sol::sim
