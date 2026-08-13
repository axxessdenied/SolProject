#pragma once

#include "Sol/Proto/Frames/TimeScales.h"
#include "Sol/Proto/Frames/Units.h"

#include <compare>
#include <cstdint>

namespace sol::proto::orbit {

/// An exact interval of campaign time, counted in integer nanoseconds.
///
/// ADR 0010 requires campaign time to accumulate as an integer with an explicit tick rate, and
/// A2 established nanoseconds as the working rate while leaving the production tick rate open.
/// A3 is where that requirement earns its cost: the warp-equivalence gate compares a run that
/// advances in one-second increments against one that advances in thousand-second increments,
/// and if the clock accumulated in doubles those two would not reach the same instant. The
/// comparison would then be measuring the clock rather than the propagation.
///
/// Conversion to seconds is exact for every interval A3 uses. A double holds every integer up
/// to 2^53, which is about 104 days of nanoseconds, and the longest span this increment
/// propagates is measured in days.
class CampaignDuration {
public:
    constexpr CampaignDuration() noexcept = default;

    [[nodiscard]] static constexpr CampaignDuration fromNanoseconds(std::int64_t value) noexcept
    {
        return CampaignDuration{value};
    }

    /// Rounds to the nearest nanosecond.
    ///
    /// For constructing scenario inputs only. Accumulating through this function is the
    /// practice ADR 0010 forbids, and A2's CampaignTime carries the same warning for the same
    /// reason.
    [[nodiscard]] static CampaignDuration fromSecondsRounded(double seconds) noexcept;

    [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept { return m_value; }
    [[nodiscard]] constexpr double seconds() const noexcept
    {
        return static_cast<double>(m_value) * 1.0e-9;
    }

    [[nodiscard]] constexpr CampaignDuration operator+(CampaignDuration other) const noexcept
    {
        return CampaignDuration{m_value + other.m_value};
    }
    [[nodiscard]] constexpr CampaignDuration operator-(CampaignDuration other) const noexcept
    {
        return CampaignDuration{m_value - other.m_value};
    }
    /// Scales by an integer factor. Integer rather than double because a warp factor that
    /// multiplied the clock by 1.5 would reintroduce exactly the inexactness this type exists
    /// to remove.
    [[nodiscard]] constexpr CampaignDuration operator*(std::int64_t factor) const noexcept
    {
        return CampaignDuration{m_value * factor};
    }

    [[nodiscard]] constexpr auto operator<=>(const CampaignDuration&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const CampaignDuration&) const noexcept = default;

private:
    constexpr explicit CampaignDuration(std::int64_t value) noexcept : m_value{value} {}

    std::int64_t m_value{0};
};

/// An instant on the campaign clock, as an exact nanosecond count past the campaign epoch.
///
/// The campaign epoch is 2026-01-01 00:00:00 UTC per ADR 0008. This type never carries a
/// calendar and never carries a time scale: conversion to the TDB ephemeris boundary happens
/// once, explicitly, at a named function, exactly as A2 established.
class CampaignInstant {
public:
    constexpr CampaignInstant() noexcept = default;

    [[nodiscard]] static constexpr CampaignInstant fromNanoseconds(std::int64_t value) noexcept
    {
        return CampaignInstant{value};
    }

    [[nodiscard]] static CampaignInstant fromSecondsRounded(double seconds) noexcept;

    [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept { return m_value; }
    [[nodiscard]] constexpr double secondsPastCampaignEpoch() const noexcept
    {
        return static_cast<double>(m_value) * 1.0e-9;
    }

    [[nodiscard]] constexpr CampaignInstant operator+(CampaignDuration delta) const noexcept
    {
        return CampaignInstant{m_value + delta.nanoseconds()};
    }
    [[nodiscard]] constexpr CampaignInstant operator-(CampaignDuration delta) const noexcept
    {
        return CampaignInstant{m_value - delta.nanoseconds()};
    }
    [[nodiscard]] constexpr CampaignDuration operator-(CampaignInstant other) const noexcept
    {
        return CampaignDuration::fromNanoseconds(m_value - other.m_value);
    }

    [[nodiscard]] constexpr auto operator<=>(const CampaignInstant&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const CampaignInstant&) const noexcept = default;

    /// The A2 boundary type, for reports that already speak it.
    [[nodiscard]] constexpr frames::CampaignTime toCampaignTime() const noexcept
    {
        return frames::CampaignTime::fromNanoseconds(m_value);
    }

private:
    constexpr explicit CampaignInstant(std::int64_t value) noexcept : m_value{value} {}

    std::int64_t m_value{0};
};

/// Converts a campaign instant to the TDB ephemeris boundary.
///
/// The single named crossing between campaign time and ephemeris time. @p campaignEpochTdb is
/// the TDB instant the campaign clock's zero corresponds to, which a caller obtains once from
/// the pinned leap-second kernel rather than assuming.
[[nodiscard]] frames::TdbEpoch toTdb(CampaignInstant instant,
                                     frames::TdbEpoch campaignEpochTdb) noexcept;

} // namespace sol::proto::orbit
