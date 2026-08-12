#pragma once

#include "Sol/Proto/Frames/Vec3.h"

#include <cstdint>

namespace sol::proto::frames {

/// Explicit-unit scalar and vector types for this library's API boundary.
///
/// AGENTS.md requires SI units for simulation truth with conversions confined to input,
/// presentation, and data boundaries. These types make that rule mechanical: a value cannot
/// be constructed without naming the unit it came from, and cannot be read without naming
/// the unit you want.
///
/// The wrappers are applied at the *boundary* only -- StateVector fields, fixture parsing,
/// geodetic input, reported results. Interior arithmetic uses the raw Vec3 and double, and
/// each such interior is documented with the unit it works in. Wrapping every intermediate
/// was tried and abandoned: it made the rotation and chain-walk code unreadable without
/// catching anything the boundary types do not already catch, and unreadable numerical code
/// is its own correctness risk.
///
/// The unit errors actually at stake in A2 are real, not hypothetical. The Horizons fixtures
/// are km and km/s, the SPICE kernels are km and degrees, ADR 0008's anchor is in degrees
/// and metres, and the thresholds are millimetres and micrometres per second. Every one of
/// those crossings is a named conversion below.

/// Radians. The unit every trigonometric interior in this library works in.
class Radians {
public:
    constexpr Radians() noexcept = default;

    [[nodiscard]] static constexpr Radians fromRadians(double value) noexcept { return Radians{value}; }
    [[nodiscard]] static constexpr Radians fromDegrees(double value) noexcept
    {
        return Radians{value * kRadiansPerDegree};
    }

    [[nodiscard]] constexpr double radians() const noexcept { return m_value; }
    [[nodiscard]] constexpr double degrees() const noexcept { return m_value / kRadiansPerDegree; }

private:
    constexpr explicit Radians(double value) noexcept : m_value{value} {}

    /// pi / 180, written to full double precision.
    static constexpr double kRadiansPerDegree = 0.017453292519943295769236907684886;

    double m_value{0.0};
};

/// SI seconds.
class Seconds {
public:
    constexpr Seconds() noexcept = default;

    [[nodiscard]] static constexpr Seconds fromSeconds(double value) noexcept { return Seconds{value}; }
    [[nodiscard]] static constexpr Seconds fromDays(double value) noexcept
    {
        return Seconds{value * kSecondsPerDay};
    }

    [[nodiscard]] constexpr double seconds() const noexcept { return m_value; }
    [[nodiscard]] constexpr double days() const noexcept { return m_value / kSecondsPerDay; }
    [[nodiscard]] constexpr double julianCenturies() const noexcept
    {
        return m_value / (kSecondsPerDay * 36525.0);
    }

    /// One SPICE/astronomical day is exactly 86400 SI seconds. This is a definition, not a
    /// measurement of Earth's rotation, and the two must not be conflated.
    static constexpr double kSecondsPerDay = 86400.0;

private:
    constexpr explicit Seconds(double value) noexcept : m_value{value} {}

    double m_value{0.0};
};

/// A position vector in metres.
class PositionMetres {
public:
    constexpr PositionMetres() noexcept = default;

    [[nodiscard]] static constexpr PositionMetres fromMetres(const Vec3& value) noexcept
    {
        return PositionMetres{value};
    }

    /// The single conversion point for the km-valued NAIF and Horizons data boundary.
    [[nodiscard]] static constexpr PositionMetres fromKilometres(const Vec3& value) noexcept
    {
        return PositionMetres{value * 1000.0};
    }

    [[nodiscard]] constexpr const Vec3& metres() const noexcept { return m_value; }

private:
    constexpr explicit PositionMetres(const Vec3& value) noexcept : m_value{value} {}

    Vec3 m_value{};
};

/// A velocity vector in metres per second.
class VelocityMetresPerSecond {
public:
    constexpr VelocityMetresPerSecond() noexcept = default;

    [[nodiscard]] static constexpr VelocityMetresPerSecond fromMetresPerSecond(const Vec3& value) noexcept
    {
        return VelocityMetresPerSecond{value};
    }

    [[nodiscard]] static constexpr VelocityMetresPerSecond fromKilometresPerSecond(const Vec3& value) noexcept
    {
        return VelocityMetresPerSecond{value * 1000.0};
    }

    [[nodiscard]] constexpr const Vec3& metresPerSecond() const noexcept { return m_value; }

private:
    constexpr explicit VelocityMetresPerSecond(const Vec3& value) noexcept : m_value{value} {}

    Vec3 m_value{};
};

/// Campaign time as an exact integer count of nanoseconds past the campaign epoch.
///
/// ADR 0010 requires campaign time to accumulate as an integer with an explicit tick rate.
/// The tick rate itself is still open (docs/architecture.md), so A2 uses nanoseconds: it is
/// exact for every interval this increment samples, and an int64 nanosecond counter spans
/// +/- 292 years, which comfortably covers a campaign.
///
/// This type exists in A2 to demonstrate and exercise the boundary, not to settle the tick
/// rate. What A2 does settle is that the conversion to the ephemeris time scale happens once,
/// explicitly, at a named function -- never by treating a campaign tick as if it were a TDB
/// second.
class CampaignTime {
public:
    constexpr CampaignTime() noexcept = default;

    [[nodiscard]] static constexpr CampaignTime fromNanoseconds(std::int64_t value) noexcept
    {
        return CampaignTime{value};
    }

    /// Rounds to the nearest nanosecond. Only for constructing scenario inputs; never use it
    /// to accumulate, which is the practice ADR 0010 forbids.
    [[nodiscard]] static CampaignTime fromSecondsRounded(double value) noexcept;

    [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept { return m_value; }
    [[nodiscard]] constexpr double seconds() const noexcept { return static_cast<double>(m_value) * 1e-9; }

    [[nodiscard]] constexpr CampaignTime operator+(CampaignTime other) const noexcept
    {
        return CampaignTime{m_value + other.m_value};
    }

private:
    constexpr explicit CampaignTime(std::int64_t value) noexcept : m_value{value} {}

    std::int64_t m_value{0};
};

} // namespace sol::proto::frames
