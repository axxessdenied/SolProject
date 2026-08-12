#pragma once

#include "Sol/Proto/Frames/FrameId.h"
#include "Sol/Proto/Frames/FrameTransform.h"
#include "Sol/Proto/Frames/ReferenceData.h"

#include <array>

namespace sol::proto::frames {

/// The parent of each frame in the graph, indexed by FrameId.
///
/// SsbIcrf is its own parent, which is how the root is recognised. Both candidate models are
/// built from this one table, so any difference between them is numerical rather than
/// structural -- which is the only way the comparison means anything.
[[nodiscard]] constexpr FrameId parentFrame(FrameId frame) noexcept
{
    switch (frame) {
    case FrameId::SsbIcrf:                 return FrameId::SsbIcrf;
    case FrameId::SunIcrf:                 return FrameId::SsbIcrf;
    case FrameId::EarthMoonBarycentreIcrf: return FrameId::SsbIcrf;
    case FrameId::EarthIcrf:               return FrameId::EarthMoonBarycentreIcrf;
    case FrameId::MoonIcrf:                return FrameId::EarthMoonBarycentreIcrf;
    case FrameId::EarthBodyFixed:          return FrameId::EarthIcrf;
    case FrameId::LaunchSiteEnu:           return FrameId::EarthBodyFixed;
    case FrameId::VehicleLocal:            return FrameId::LaunchSiteEnu;
    }
    return FrameId::SsbIcrf;
}

/// Distance from @p frame to the root.
[[nodiscard]] constexpr int frameDepth(FrameId frame) noexcept
{
    int depth = 0;
    FrameId current = frame;
    while (current != FrameId::SsbIcrf) {
        current = parentFrame(current);
        ++depth;
    }
    return depth;
}

/// The parent-relative transform of every frame at one instant.
///
/// This is the shared input both models are constructed from. It is deliberately a plain
/// value: building it is not part of what A2 measures, and sharing it removes construction
/// cost from the comparison entirely.
struct FrameGraphSnapshot {
    TdbEpoch epoch{};
    std::array<FrameTransform, kFrameCount> parentToFrame{};
};

/// How the origins of the celestial frames move between fixture epochs.
///
/// A2 has exactly one ephemeris fixture epoch, and its scenarios sample a several-minute
/// window around it. Origins therefore move linearly from their fixture state:
///
///     r(t) = r0 + v0 (t - t0)
///
/// This is a *definition of the frames' kinematics for this increment*, not an ephemeris and
/// not a propagator. It is exactly self-consistent -- position and velocity agree by
/// construction -- which is all a frame-conversion round trip can be sensitive to, and it
/// keeps propagation entirely inside increment A3 where ADR 0011 owns it.
///
/// It is not astronomically accurate. Over a 500 s ascent the neglected curvature displaces
/// Earth's barycentric position by several hundred metres. Nothing A2 concludes depends on
/// that, and nothing A2 concludes may be quoted as an ephemeris result.
struct OriginMotionModel {
    static constexpr const char* kDescription =
        "linear extrapolation from the single fixture epoch; frame kinematics only, not an "
        "ephemeris";
};

/// Where the vehicle is, in the launch-site topocentric frame.
struct VehicleState {
    Vec3 positionInEnu{};
    Vec3 velocityInEnu{};
};

/// Builds the parent-relative transforms for @p epoch.
///
/// @param vehicle  Defines the VehicleLocal floating origin. Its axes stay parallel to the
///                 launch site's, so the local frame translates with the craft without
///                 rotating under it -- the "floating local origin" of the proposed seamless
///                 scale strategy in docs/architecture.md.
[[nodiscard]] FrameGraphSnapshot buildSnapshot(const ReferenceData& data, TdbEpoch epoch,
                                               const VehicleState& vehicle);

/// The launch anchor's body-fixed position, in metres, on the reference data's own ellipsoid.
[[nodiscard]] Vec3 launchAnchorBodyFixed(const ReferenceData& data);

/// Rotation taking Earth body-fixed coordinates into launch-site east-north-up coordinates.
[[nodiscard]] Mat3 launchSiteEnuRotation(const ReferenceData& data);

} // namespace sol::proto::frames
