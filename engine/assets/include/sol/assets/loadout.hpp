#pragma once

// Loadout resolution (engine plan Phase 8a): a base ShipDef plus fitted
// modules and hired crew produce an effective ShipDef the game applies
// through its normal def path. Pure def math, deliberately below sim.
//
// Resolution is order-independent: every _add modifier sums onto the base
// stat, then every _mul multiplies the result; finally module mass dilutes
// linear and angular accelerations by mass / (mass + total module mass).

#include "sol/assets/data_defs.hpp"

#include <span>
#include <string>

namespace sol::assets {

// Validates a fit against the ship's typed slot counts, crew berths, and
// power budget. Null entries are ignored (empty slots). On failure writes a
// human-readable reason to outError (if given) and returns false.
[[nodiscard]] bool validateLoadout(const ShipDef& ship,
                                   std::span<const ModuleDef* const> modules,
                                   std::span<const CrewDef* const> crew,
                                   std::string* outError = nullptr);

// Applies modifiers and the mass penalty; does not validate. Null entries
// are ignored. The returned def keeps the base's identity (id/name/model),
// slot counts, and prices — only the modified stats differ.
[[nodiscard]] ShipDef resolveLoadout(const ShipDef& base,
                                     std::span<const ModuleDef* const> modules,
                                     std::span<const CrewDef* const> crew);

} // namespace sol::assets
