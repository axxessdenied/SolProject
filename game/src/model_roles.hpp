#pragma once

// The model slots this game draws into (engine plan Phase 19).
//
// ⚑ THIS FILE IS THE ONE PLACE MODEL VOCABULARY IS STILL ALLOWED IN C++, AND
// THE DISTINCTION IS THE WHOLE POINT OF THE PHASE. A role id names a slot the
// engine HAS - it either draws a cockpit or it does not, and no data file can
// add a new kind of thing to the world. What a slot is FILLED with is content,
// and lives in `[[role]]` rows in `game/data/models.toml`.
//
// Before Phase 19 the answers lived here too, as string literals scattered
// through `space_world.cpp` and `main.cpp`: Phase 9 stage A replaced the
// `ModelId` enum with a def lookup and only a SHIP def ever went through it,
// so a mesh could be authored, cooked, given a `[[model]]` row - and still
// have no way of becoming a gate, a rock, an ore chunk or a bolt.
//
// Adding a role means adding it here AND in `models.toml`; `validateRoles`
// refuses to load a database that fills one and not the other, in either
// direction.

#include <array>
#include <span>

namespace game {

// Statics authored at their real size and drawn at scale 1.
inline constexpr const char* kRoleGate = "gate";
inline constexpr const char* kRoleGateMembrane = "gate_membrane";

// ⚑ THE THREE BELOW CARRY A RADIUS CONTRACT: their instance scale MEANS
// something, so the model filling one must be authored at radius 1.0 or the
// scale silently changes meaning. A rock's scale is its radius in metres, a
// chunk is 6 m, a bolt is 0.3 x 0.3 x 4 m. Getting this wrong moves collision
// and the mining/salvage beam sweep - `modelBaseRadius() * scale` is what all
// of them read - with no visual tell that anything is off, which is why
// `assets.unit` pins it rather than trusting the comment.
inline constexpr const char* kRoleRock = "rock";
inline constexpr const char* kRoleOreChunk = "ore_chunk";
inline constexpr const char* kRoleBolt = "bolt";

// Pushed by the game layer rather than read off a RenderShape, so it is the
// one drawable with no entity behind it.
inline constexpr const char* kRoleCockpit = "cockpit";

// ⚑ THE THREE BELOW ARE FALLBACKS, AND COUNTING THEM IS WHY THE RECORDED
// TALLY OF SIX LITERALS WAS REALLY TEN. A def that names a model which does
// not exist still has to draw SOMETHING, and until Phase 19 that something
// was a string literal at the call site - two of which stage H itself
// introduced, by turning `modelIdFromName`'s hardcoded fallback into an
// argument and passing "station" and "ship" from the outside. Moving a
// literal to the caller is not removing it.
inline constexpr const char* kRoleStation = "station";
inline constexpr const char* kRoleShip = "ship";

// What a wreck is drawn as when the ship that died cannot be identified.
// Phase 19 stage E prefers the victim's own def; this is what it falls back
// to, so the site is data-driven whether or not that stage happens.
inline constexpr const char* kRoleWreck = "wreck";

// The vocabulary `DefDatabase::validateRoles` is checked against, in both
// directions: a missing row is a refusal, and so is a row naming a role that
// is not in here.
//
// ⚑ A real static array rather than a returned `std::initializer_list`, whose
// backing store would not outlive the return statement.
inline constexpr std::array<const char* const, 9> kModelRoles = {
    kRoleGate,    kRoleGateMembrane, kRoleRock, kRoleOreChunk, kRoleBolt,
    kRoleCockpit, kRoleStation,      kRoleShip, kRoleWreck};

// The roles above whose instance scale carries meaning, so the model filling
// them must be authored at radius 1.0. Named here rather than in the test so
// the contract sits beside the roles it constrains.
inline constexpr std::array<const char* const, 3> kUnitRadiusRoles = {kRoleRock, kRoleOreChunk,
                                                                     kRoleBolt};

[[nodiscard]] inline std::span<const char* const> modelRoles()
{
    return {kModelRoles.data(), kModelRoles.size()};
}

[[nodiscard]] inline std::span<const char* const> unitRadiusRoles()
{
    return {kUnitRadiusRoles.data(), kUnitRadiusRoles.size()};
}

} // namespace game
