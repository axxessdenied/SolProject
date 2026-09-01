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
    case FitStat::ForwardAccel:
        return def.flight.forwardAccel;
    case FitStat::ReverseAccel:
        return def.flight.reverseAccel;
    case FitStat::LateralAccel:
        return def.flight.lateralAccel;
    case FitStat::VerticalAccel:
        return def.flight.verticalAccel;
    case FitStat::MaxSpeed:
        return def.flight.maxSpeed;
    case FitStat::TurnRate:
        return 1.0f; // uniform scale; handled separately
    case FitStat::CruiseSpeedScale:
        return def.flight.cruiseSpeedScale;
    case FitStat::ShieldStrength:
        return def.defense.shieldStrength;
    case FitStat::ShieldRegen:
        return def.defense.shieldRegen;
    case FitStat::Armor:
        return def.defense.armor;
    case FitStat::Hull:
        return def.defense.hull;
    case FitStat::WeaponCapacitor:
        return def.power.weaponCapacitor;
    case FitStat::WeaponRecharge:
        return def.power.weaponRecharge;
    case FitStat::Cargo:
        return def.cargoCapacity;
    case FitStat::ScanRange:
        return def.scanRange;
    case FitStat::ScanSpeed:
        return def.scanSpeed;
    case FitStat::CollectorRange:
        return def.collectorRange;
    case FitStat::Signature:
        return def.signature;
    case FitStat::Count:
        break;
    }
    return 0.0f;
}

void setStatValue(ShipDef& def, FitStat stat, float value)
{
    switch (stat) {
    case FitStat::ForwardAccel:
        def.flight.forwardAccel = value;
        return;
    case FitStat::ReverseAccel:
        def.flight.reverseAccel = value;
        return;
    case FitStat::LateralAccel:
        def.flight.lateralAccel = value;
        return;
    case FitStat::VerticalAccel:
        def.flight.verticalAccel = value;
        return;
    case FitStat::MaxSpeed:
        def.flight.maxSpeed = value;
        return;
    case FitStat::TurnRate:
        return; // handled separately (three axes)
    case FitStat::CruiseSpeedScale:
        def.flight.cruiseSpeedScale = value;
        return;
    case FitStat::ShieldStrength:
        def.defense.shieldStrength = value;
        return;
    case FitStat::ShieldRegen:
        def.defense.shieldRegen = value;
        return;
    case FitStat::Armor:
        def.defense.armor = value;
        return;
    case FitStat::Hull:
        def.defense.hull = value;
        return;
    case FitStat::WeaponCapacitor:
        def.power.weaponCapacitor = value;
        return;
    case FitStat::WeaponRecharge:
        def.power.weaponRecharge = value;
        return;
    case FitStat::Cargo:
        def.cargoCapacity = value;
        return;
    case FitStat::ScanRange:
        def.scanRange = value;
        return;
    case FitStat::ScanSpeed:
        def.scanSpeed = value;
        return;
    case FitStat::CollectorRange:
        def.collectorRange = value;
        return;
    case FitStat::Signature:
        def.signature = value;
        return;
    case FitStat::Count:
        break;
    }
}

struct Accumulated
{
    std::array<float, kFitStatCount> add{};
    std::array<float, kFitStatCount> mul = StatModifiers::ones();
    float componentMass = 0.0f;
};

void accumulate(Accumulated& acc, const StatModifiers& mods)
{
    for (std::size_t i = 0; i < kFitStatCount; ++i) {
        acc.add[i] += mods.add[i];
        acc.mul[i] *= mods.mul[i];
    }
}

[[nodiscard]] std::string describe(MountKind kind, MountSize size)
{
    return std::string(mountSizeName(size)) + " " + mountKindName(kind);
}

} // namespace

const std::string* FittedMount::defId() const
{
    if (component != nullptr) {
        return &component->id;
    }
    return weapon != nullptr ? &weapon->id : nullptr;
}

MountKind FittedMount::kind() const
{
    if (component != nullptr) {
        return component->mount;
    }
    return weapon != nullptr ? weapon->mount : MountKind::Utility;
}

MountSize FittedMount::size() const
{
    if (component != nullptr) {
        return component->size;
    }
    return weapon != nullptr ? weapon->size : MountSize::Small;
}

bool validateLoadout(const ShipDef& ship,
                     std::span<const FittedMount> fittings,
                     std::span<const CrewDef* const> crew,
                     std::string* outError)
{
    float powerDraw = 0.0f;
    for (std::size_t i = 0; i < fittings.size(); ++i) {
        const FittedMount& fitting = fittings[i];
        const std::string mountId(fitting.mountId);
        const ShipMount* mount = ship.findMount(fitting.mountId);
        if (mount == nullptr) {
            if (outError != nullptr) {
                *outError = "'" + ship.name + "' has no mount '" + mountId + "'";
            }
            return false;
        }
        // decisions/014 rule 1: exactly one fitting per mount. The check is
        // against the fittings BEFORE this one, so the message can name the
        // mount that is already taken rather than a count.
        for (std::size_t j = 0; j < i; ++j) {
            if (fittings[j].mountId == fitting.mountId) {
                if (outError != nullptr) {
                    *outError = "mount '" + mountId + "' is already fitted";
                }
                return false;
            }
        }
        if (fitting.empty()) {
            if (outError != nullptr) {
                *outError = "mount '" + mountId + "' holds a fitting whose def is missing";
            }
            return false;
        }
        if (!mountAccepts(*mount, fitting.kind(), fitting.size())) {
            if (outError != nullptr) {
                *outError = "mount '" + mountId + "' is a " + describe(mount->kind, mount->size) +
                            " and takes no " + describe(fitting.kind(), fitting.size());
            }
            return false;
        }
        if (fitting.component != nullptr) {
            powerDraw += fitting.component->powerDraw;
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

ShipDef resolveLoadout(const ShipDef& base,
                       std::span<const FittedMount> fittings,
                       std::span<const CrewDef* const> crew)
{
    Accumulated acc;
    for (const FittedMount& fitting : fittings) {
        if (fitting.component != nullptr) {
            accumulate(acc, fitting.component->modifiers);
            acc.componentMass += fitting.component->mass;
        }
    }
    for (const CrewDef* member : crew) {
        if (member != nullptr) {
            accumulate(acc, member->modifiers);
        }
    }

    ShipDef def = base;
    // The resolved def IS the ship as flown, mounts included: every consumer
    // that used to read `ShipDef::weaponId` now reads a mount's `fit`, and
    // giving the resolved def the player's fit rather than the hull's default
    // is what lets ONE code path serve an NPC hull and the player's own.
    for (ShipMount& mount : def.mounts) {
        mount.fit.clear();
    }
    for (const FittedMount& fitting : fittings) {
        const std::string* id = fitting.defId();
        if (id == nullptr) {
            continue;
        }
        for (ShipMount& mount : def.mounts) {
            if (mount.id == fitting.mountId) {
                mount.fit = *id;
                break;
            }
        }
    }
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
        def.flight.maxTurnRate[axis] = (base.flight.maxTurnRate[axis] + acc.add[turn]) * acc.mul[turn];
    }

    // Mass penalty: component mass dilutes linear and angular acceleration.
    const float massFactor = base.mass / (base.mass + acc.componentMass);
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
