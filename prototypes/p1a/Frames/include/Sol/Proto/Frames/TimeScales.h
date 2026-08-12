#pragma once

#include "Sol/Proto/Frames/TextKernel.h"
#include "Sol/Proto/Frames/Units.h"

#include <string>
#include <vector>

namespace sol::proto::frames {

/// A UTC calendar instant.
///
/// UTC is a calendar, not a uniform count of seconds, so it is represented as one. Storing
/// UTC as "seconds since some epoch" is the single most common way projects lose leap
/// seconds, because two different UTC instants can then map to the same number.
///
/// @c second may reach 60.0 during a positive leap second. That is a legal UTC time, not an
/// input error, and ADR 0008 requires the increment to exercise it.
struct UtcDateTime {
    int year{2000};
    int month{1};
    int day{1};
    int hour{0};
    int minute{0};
    double second{0.0};
};

/// An instant on the TDB scale, as SI seconds past the J2000 epoch (JD 2451545.0 TDB).
///
/// This is the scale ADR 0008 nominates as the ephemeris boundary, and the only scale any
/// ephemeris fixture in this library is evaluated at. It never carries a calendar.
class TdbEpoch {
public:
    constexpr TdbEpoch() noexcept = default;

    [[nodiscard]] static constexpr TdbEpoch fromSecondsPastJ2000(double value) noexcept
    {
        return TdbEpoch{value};
    }

    [[nodiscard]] static constexpr TdbEpoch fromJulianDate(double julianDateTdb) noexcept
    {
        return TdbEpoch{(julianDateTdb - kJ2000JulianDate) * Seconds::kSecondsPerDay};
    }

    [[nodiscard]] constexpr double secondsPastJ2000() const noexcept { return m_value; }
    [[nodiscard]] constexpr double julianDate() const noexcept
    {
        return kJ2000JulianDate + m_value / Seconds::kSecondsPerDay;
    }

    [[nodiscard]] constexpr TdbEpoch advancedBy(Seconds delta) const noexcept
    {
        return TdbEpoch{m_value + delta.seconds()};
    }

    /// Julian date of the J2000 epoch. A definition, not a measurement.
    static constexpr double kJ2000JulianDate = 2451545.0;

private:
    constexpr explicit TdbEpoch(double value) noexcept : m_value{value} {}

    double m_value{0.0};
};

/// One step of the TAI-UTC table.
struct LeapSecondEntry {
    /// TAI - UTC, in SI seconds, applicable from @c effectiveFrom onward.
    double taiMinusUtcSeconds{0.0};
    /// The UTC calendar instant at which the value takes effect.
    UtcDateTime effectiveFrom{};
    /// The token exactly as it appeared in the kernel, retained for provenance reporting.
    std::string sourceToken;
};

/// The UTC/TAI/TT/TDB boundary of ADR 0008, driven entirely by a pinned leap-second kernel.
///
/// Nothing in this class hardcodes a leap-second count or the 32.184 s TAI-TT offset. Both
/// come from the kernel, so the fixtures and the conversions cannot drift apart: replacing
/// the kernel replaces the conversion, and the recorded checksum says which one was used.
///
/// The TDB model is SPICE's own analytic approximation, built from the DELTET/K, DELTET/EB,
/// and DELTET/M constants the same kernel supplies:
///
///     TDB = TAI + DELTA_T_A + K sin(E),  E = M + EB sin(M),  M = M0 + M1 * TDB
///
/// It agrees with the full Fairhead-Bretagnon series to well under a millisecond, which is
/// far inside anything A2 measures. What matters for this increment is that it is exact,
/// reproducible, and sourced from the pinned data rather than from a constant typed in here.
class TimeScales {
public:
    /// Builds the boundary from an already-parsed leap-second kernel.
    /// Throws std::runtime_error when a required assignment is missing or malformed.
    [[nodiscard]] static TimeScales fromLeapSecondKernel(const TextKernel& kernel);

    /// TAI - UTC applicable at @p utc, in seconds.
    ///
    /// During a positive leap second the correct answer is the value in force *before* the
    /// step, because 23:59:60 belongs to the old day. The lookup therefore uses the start of
    /// @p utc's minute, which is the last instant guaranteed to be on the pre-step side of a
    /// table entry -- leap seconds only ever take effect at 00:00:00 UTC.
    [[nodiscard]] double taiMinusUtcSeconds(const UtcDateTime& utc) const;

    /// Converts a UTC calendar instant to the TDB ephemeris boundary.
    [[nodiscard]] TdbEpoch utcToTdb(const UtcDateTime& utc) const;

    /// Converts back to a UTC calendar instant.
    ///
    /// The inverse is iterative because TAI-UTC is a function of the UTC instant being
    /// solved for. It converges in at most a few steps everywhere except inside a leap
    /// second, where UTC is genuinely not a function of TDB; the ambiguous instant resolves
    /// to the post-step representation and @c leapSecondAmbiguous is set so a caller cannot
    /// mistake it for an exact round trip.
    [[nodiscard]] UtcDateTime tdbToUtc(TdbEpoch tdb, bool* leapSecondAmbiguous = nullptr) const;

    /// TT - TAI, from DELTET/DELTA_T_A. Conventionally 32.184 s.
    [[nodiscard]] double ttMinusTaiSeconds() const noexcept { return m_deltaTa; }

    /// TDB - TT at @p tdb, in seconds. The periodic term only; its magnitude is under 2 ms.
    [[nodiscard]] double tdbMinusTtSeconds(TdbEpoch tdb) const noexcept;

    [[nodiscard]] const std::vector<LeapSecondEntry>& leapSeconds() const noexcept
    {
        return m_leapSeconds;
    }

private:
    std::vector<LeapSecondEntry> m_leapSeconds;
    double m_deltaTa{0.0};
    double m_k{0.0};
    double m_eb{0.0};
    double m_m0{0.0};
    double m_m1{0.0};
};

/// Julian date at 00:00 UTC of the given proleptic Gregorian calendar date.
///
/// Every day counts as exactly 86400 seconds, which is what makes this a *nominal* Julian
/// date: it is a calendar-arithmetic result, not a physical duration, and it is only ever
/// used here to order UTC instants and to index the leap-second table.
[[nodiscard]] double julianDateAtMidnightUtc(int year, int month, int day);

/// Inverse of julianDateAtMidnightUtc.
void civilDateFromJulianDayNumber(long long julianDayNumber, int& year, int& month, int& day);

/// The nominal Julian date of a UTC calendar instant, counting every day as 86400 s.
[[nodiscard]] double nominalJulianDateUtc(const UtcDateTime& utc);

/// Formats a UTC instant as `YYYY-MM-DDTHH:MM:SS.ssssssZ`.
[[nodiscard]] std::string formatUtc(const UtcDateTime& utc);

} // namespace sol::proto::frames
