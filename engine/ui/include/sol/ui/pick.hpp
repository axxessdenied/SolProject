#pragma once

#include "sol/core/math/vec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sol::ui {

// Screen-space picking for the Phase 8j click-to-select, kept out of the HUD so
// it can be asserted on headlessly - the same arrangement map_projection.hpp
// and radar_projection.hpp have.
//
// The whole file is one ruling: a pick is answered by projecting every
// candidate FORWARDS to the point it was drawn at and taking the nearest one
// to the cursor. Inverting a projection is the obvious alternative and it is
// wrong here - the radar's is not invertible where it matters (rim contacts
// clamp, so many distances share one radius; the blip rides the end of a stalk
// that is a second function of the same offset), and the perspective one would
// have to be inverted against a depth nobody has. Projecting forwards is exact
// and it makes the point the player clicks the point they were shown, by
// construction, for the disc and the flight view alike.

// Perspective focal length in pixels for a vertical field of view. The HUD's
// world-space markers and the pick share it, so a marker and its hit box
// cannot disagree about where the world is.
[[nodiscard]] inline float focalLength(float screenHeight, float tanHalfFovY)
{
    const float tangent = tanHalfFovY > 0.0001f ? tanHalfFovY : 1.0f;
    return (screenHeight * 0.5f) / tangent;
}

// Camera-space direction to a screen point. `inFront` is false when the
// direction is behind the near plane, where the projection would mirror it.
struct ScreenPoint
{
    core::Vec2 position;
    bool inFront = false;
};

[[nodiscard]] inline ScreenPoint screenPoint(const core::Vec3& directionCamera, core::Vec2 center,
                                             float focal)
{
    if (directionCamera.z >= -0.01f) { // camera looks down -Z
        return {};
    }
    const float depth = -directionCamera.z;
    return {{center.x + (directionCamera.x / depth) * focal,
             center.y - (directionCamera.y / depth) * focal},
            true};
}

// One thing that can be clicked in the flight view. `directionCamera` is a
// unit direction in camera space; `screenRadius` is how big the thing is on
// screen, so a station is picked anywhere across its disc rather than at the
// one point its centre projects to; `rangeMeters` breaks ties.
struct PickCandidate
{
    core::Vec3 directionCamera;
    float screenRadius = 0.0f;
    double rangeMeters = 0.0;
    std::uint32_t selection = 0; // opaque to the projection: the caller's index
};

// A fighter at four kilometres is three pixels across, and the complaint this
// item exists to fix is that precision selection is expensive - so the grab
// box is generous and objects only ever grow it.
inline constexpr float kPickGrabPixels = 28.0f;

// The winning candidate's position in `candidates`, or `kNoPick`.
inline constexpr std::size_t kNoPick = static_cast<std::size_t>(-1);

// Nearest candidate to `cursor`, in virtual UI pixels. Ties go to the one
// nearest the cursor and then to the nearer one in world, so a fighter
// silhouetted against a planet is picked over the planet.
[[nodiscard]] inline std::size_t pickNearest(std::span<const PickCandidate> candidates,
                                             core::Vec2 cursor, core::Vec2 center, float focal,
                                             float grabPixels = kPickGrabPixels)
{
    std::size_t best = kNoPick;
    float bestDistanceSquared = 0.0f;
    double bestRange = 0.0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const PickCandidate& candidate = candidates[i];
        const ScreenPoint point = screenPoint(candidate.directionCamera, center, focal);
        if (!point.inFront) {
            continue; // behind the camera: never picked, however close it is
        }
        const float dx = point.position.x - cursor.x;
        const float dy = point.position.y - cursor.y;
        const float distanceSquared = dx * dx + dy * dy;
        const float grab = candidate.screenRadius > grabPixels ? candidate.screenRadius : grabPixels;
        if (distanceSquared > grab * grab) {
            continue;
        }
        if (best == kNoPick || distanceSquared < bestDistanceSquared ||
            (distanceSquared == bestDistanceSquared && candidate.rangeMeters < bestRange)) {
            best = i;
            bestDistanceSquared = distanceSquared;
            bestRange = candidate.rangeMeters;
        }
    }
    return best;
}

// One blip's hit test on the radar disc: `points` are the dots as drawn (see
// radarDot), and the pick is the nearest within `grabPixels`. Kept separate
// from pickNearest because a disc has no depth to break ties with - the
// nearest dot to the cursor is the answer, and the fill has already ordered
// contacts nearest-first, so an exact tie resolves to the nearer contact.
inline constexpr float kRadarGrabPixels = 11.0f;

[[nodiscard]] inline std::size_t pickNearestPoint(std::span<const core::Vec2> points,
                                                  core::Vec2 cursor,
                                                  float grabPixels = kRadarGrabPixels)
{
    std::size_t best = kNoPick;
    float bestDistanceSquared = grabPixels * grabPixels;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float dx = points[i].x - cursor.x;
        const float dy = points[i].y - cursor.y;
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= bestDistanceSquared) {
            if (best != kNoPick && distanceSquared == bestDistanceSquared) {
                continue; // first wins, and the fill puts the nearer contact first
            }
            best = i;
            bestDistanceSquared = distanceSquared;
        }
    }
    return best;
}

// True when `point` is inside a disc - used to route a click to the radar
// rather than to the world behind it.
[[nodiscard]] inline bool insideDisc(core::Vec2 point, core::Vec2 center, float radius)
{
    const float dx = point.x - center.x;
    const float dy = point.y - center.y;
    return dx * dx + dy * dy <= radius * radius;
}

} // namespace sol::ui
