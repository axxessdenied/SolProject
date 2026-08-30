#pragma once

// Loadout resolution (engine plan Phase 8a): a base ShipDef plus fitted
// components and hired crew produce an effective ShipDef the game applies
// through its normal def path. Pure def math, deliberately below sim.
//
// Resolution is order-independent: every _add modifier sums onto the base
// stat, then every _mul multiplies the result; finally component mass dilutes
// linear and angular accelerations by mass / (mass + total component mass).
//
// ⚑ Phase 31 stage B changed WHERE a fitting sits and nothing about WHAT it
// does. The adds-then-muls math below is untouched by mounts, which is exactly
// the property `decisions/014` predicted it would keep: a mount is a place,
// and a place has no opinion about a shield multiplier.

#include "sol/assets/data_defs.hpp"

#include <span>
#include <string>
#include <string_view>

namespace sol::assets {

// One fitting in one mount (Phase 31 stage B). `mountId` names the place; at
// most one of `component`/`weapon` is set, and WHICH one is decided by the
// mount's kind rather than by searching two tables for an id.
//
// Both null is a fitting whose def has gone missing - a mod uninstalled under
// a save. That is ignored by the math and reported by validation, the same
// null-tolerance the flat component list had before mounts.
struct FittedMount
{
    std::string_view mountId;
    const ComponentDef* component = nullptr;
    const WeaponDef* weapon = nullptr;

    [[nodiscard]] bool empty() const { return component == nullptr && weapon == nullptr; }

    [[nodiscard]] const std::string* defId() const;
    [[nodiscard]] MountKind kind() const;
    [[nodiscard]] MountSize size() const;
};

// Validates a fit against the ship's MOUNTS, crew berths, and power budget:
// every fitting names a mount that exists on this hull, no mount is fitted
// twice, and each mount accepts its fitting's kind and size. On failure writes
// a human-readable reason to outError (if given) and returns false.
//
// A fitting whose def is missing is refused by name rather than skipped: it is
// occupying a mount the player can see, and silently treating it as empty is
// how a fit that will not reload passes validation.
[[nodiscard]] bool validateLoadout(const ShipDef& ship,
                                   std::span<const FittedMount> fittings,
                                   std::span<const CrewDef* const> crew,
                                   std::string* outError = nullptr);

// Applies modifiers and the mass penalty; does not validate. The returned def
// keeps the base's identity (id/name/model), mount list, and prices - only the
// modified stats differ, and each mount's `fit` is rewritten to what is
// actually in it, so the resolved def IS the ship as flown.
//
// Weapons contribute no mass and no power draw today: `WeaponDef` carries
// neither, and inventing them here would be balance written by the resolver.
[[nodiscard]] ShipDef resolveLoadout(const ShipDef& base,
                                     std::span<const FittedMount> fittings,
                                     std::span<const CrewDef* const> crew);

} // namespace sol::assets
