#pragma once

#include "sol/ui/screens.hpp"

#include <cmath>

namespace sol::ui {

// Radar projection math for the Phase 8h contact disc, kept out of the HUD so
// it can be asserted on headlessly — the same arrangement map_projection.hpp
// has, data contract in screens.hpp and maths here. This is a *radar*, not a
// map: it is drawn relative to the ship, it rotates with the ship, and its
// job is to answer "which way do I turn".

// The disc compresses radius on a log curve because the playfield spans six
// orders of magnitude — a fighter at 800 m and a station at 1e8 m have to
// both be on it, and a linear disc shows one or the other, never both.
//
// `kRadarDecade` is the distance that lands one decade along the curve, and
// it is deliberately tiny: it sets where the curve spends its resolution.
// At 1 km, dogfight ranges (100 m - 3 km) all pile into the innermost few
// percent of the disc and the radar is useless in the one situation it is
// most needed. At 10 m they spread across 13-31% of the radius while a
// station at 1e8 m still sits at 87%, which is the spread that makes one
// disc serve both a knife fight and a cross-system approach.
inline constexpr double kRadarDecade = 10.0;

// What the outer ring stands for, in meters. Fixed rather than fitted to the
// furthest contact: the star is 1e11 m out and always present, so an
// auto-range would peg to it every frame and crush everything else into the
// middle. Anything past this clamps hollow to the rim, which still carries
// the one thing that matters about it - the direction it lies in.
inline constexpr double kRadarRangeMeters = 1.0e9;

// Compressed radius for a distance in meters. Monotonic and zero at zero, so
// nearer is always nearer on the disc.
[[nodiscard]] inline float radarRadius(double meters)
{
    return static_cast<float>(std::log10(1.0 + std::abs(meters) / kRadarDecade));
}

// The disc's outer ring, as a real distance. Everything past this clamps to
// the rim (and is drawn hollow, so "beyond the radar" never reads as "at the
// edge of the radar").
[[nodiscard]] inline float radarRange(double outerMeters)
{
    const float extent = radarRadius(outerMeters);
    return extent > 0.0f ? extent : 1.0f;
}

// Where a contact lands on a disc of radius `discRadius` about `center`.
//
// Bearing is exact and measured about the ship's forward axis, so the disc
// turns with the ship: a contact drawn at the top of the disc is dead ahead,
// and one at the left is off the port bow. Screen y grows downward, so
// forward (-z in ship space) maps to -y on screen.
[[nodiscard]] inline core::Vec2 radarPoint(core::Vec3 offsetMeters, core::Vec2 center,
                                           float discRadius, float range)
{
    const double x = offsetMeters.x;
    const double z = offsetMeters.z;
    const double planar = std::sqrt(x * x + z * z);
    if (planar <= 0.0 || range <= 0.0f || discRadius <= 0.0f) {
        return center;
    }
    // Clamped so a contact past the outer ring sits ON the rim rather than
    // outside the disc; the caller draws those hollow.
    float scaled = discRadius * (radarRadius(planar) / range);
    if (scaled > discRadius) {
        scaled = discRadius;
    }
    // Unit bearing in the ship's horizontal plane: right is +x, forward is
    // -z. Screen y grows downward, so the forward component subtracts.
    const double inverse = 1.0 / planar;
    const float right = static_cast<float>(x * inverse);
    const float forward = static_cast<float>(-z * inverse);
    return {center.x + right * scaled, center.y - forward * scaled};
}

// True when the contact is past the outer ring, i.e. its dot was clamped to
// the rim. The disc draws these hollow.
[[nodiscard]] inline bool radarBeyondRange(core::Vec3 offsetMeters, float range)
{
    const double x = offsetMeters.x;
    const double z = offsetMeters.z;
    return radarRadius(std::sqrt(x * x + z * z)) > range;
}

// The vertical stalk: how far above (negative) or below (positive) the disc
// plane a contact's dot is drawn, in screen pixels. A flat disc lies about a
// 3D space unless this is right, so it gets the same log compression as the
// radius and is clamped to `limit` pixels either way.
[[nodiscard]] inline float radarStalk(core::Vec3 offsetMeters, float limit)
{
    const float magnitude = radarRadius(std::abs(static_cast<double>(offsetMeters.y)));
    // The stalk saturates at the same decade count the disc's rim uses, so a
    // contact far above reads as "far above" rather than pinning instantly.
    constexpr float kStalkDecades = 5.0f;
    float pixels = limit * (magnitude / kStalkDecades);
    if (pixels > limit) {
        pixels = limit;
    }
    // Screen y grows downward: something above the ship draws upward.
    return offsetMeters.y >= 0.0f ? -pixels : pixels;
}

} // namespace sol::ui
