#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sol::proto::frames {

/// The reference frames A2 measures conversions between.
///
/// This is a *runtime* tag, not a compile-time one. A compile-time frame parameter was
/// considered and rejected for A2: the whole subject under test is a frame *graph* whose
/// shape and depth are data, and whose transforms change every timestep. Encoding the graph
/// in the type system would force every conversion through templates that both candidate
/// models would have to instantiate identically, which is exactly the sort of measurement
/// distortion the P1a rules warn about. Frame mismatches are instead rejected at conversion
/// time, loudly.
///
/// The set is deliberately the minimum chain that reaches from a launch pad to the Solar
/// System barycentre while crossing every kind of boundary the architecture proposes: a
/// floating local origin, a topocentric horizon frame, a rotating body-fixed frame, two
/// nested inertial translations, and the global root.
enum class FrameId : std::uint8_t {
    /// Origin at the Solar System barycentre, ICRF axes. The graph root.
    SsbIcrf = 0,
    /// Origin at the Sun's centre, ICRF axes.
    SunIcrf,
    /// Origin at the Earth-Moon barycentre, ICRF axes.
    EarthMoonBarycentreIcrf,
    /// Origin at Earth's centre, ICRF axes. The frame a geocentric orbit is expressed in.
    EarthIcrf,
    /// Origin at the Moon's centre, ICRF axes.
    MoonIcrf,
    /// Origin at Earth's centre, IAU_EARTH body-fixed axes. Rotating.
    EarthBodyFixed,
    /// Origin at the ADR 0008 launch anchor, east-north-up axes. Rotating with Earth.
    LaunchSiteEnu,
    /// Origin at the vehicle, axes parallel to LaunchSiteEnu. A floating local origin.
    VehicleLocal,
};

inline constexpr std::size_t kFrameCount = 8;

[[nodiscard]] constexpr std::string_view frameName(FrameId frame) noexcept
{
    switch (frame) {
    case FrameId::SsbIcrf:                 return "SsbIcrf";
    case FrameId::SunIcrf:                 return "SunIcrf";
    case FrameId::EarthMoonBarycentreIcrf: return "EarthMoonBarycentreIcrf";
    case FrameId::EarthIcrf:               return "EarthIcrf";
    case FrameId::MoonIcrf:                return "MoonIcrf";
    case FrameId::EarthBodyFixed:          return "EarthBodyFixed";
    case FrameId::LaunchSiteEnu:           return "LaunchSiteEnu";
    case FrameId::VehicleLocal:            return "VehicleLocal";
    }
    return "unknown";
}

/// The frame chain from the launch pad to the graph root, in ascending order.
///
/// Consecutive entries are the boundaries A2 reports per-boundary error for. The Sun and Moon
/// frames are not on this chain; they exist so the graph has branches, which is what makes
/// the hierarchical model's common-ancestor walk a real operation rather than a straight line.
inline constexpr std::array<FrameId, 6> kSurfaceToRootChain{
    FrameId::VehicleLocal,
    FrameId::LaunchSiteEnu,
    FrameId::EarthBodyFixed,
    FrameId::EarthIcrf,
    FrameId::EarthMoonBarycentreIcrf,
    FrameId::SsbIcrf,
};

} // namespace sol::proto::frames
