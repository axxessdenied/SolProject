#include "Sol/Proto/Frames/TimeScales.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace sol::proto::frames {
namespace {

constexpr std::array<const char*, 12> kMonthAbbreviations{
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

/// Parses a SPICE `@YYYY-MON-D` epoch token from the leap-second table.
///
/// Only the exact shape the pinned kernel uses is accepted. SPICE's full epoch grammar is
/// far larger, and quietly accepting a form the table does not contain would mean this
/// parser is not validating the file it claims to read.
[[nodiscard]] UtcDateTime parseDeltaAtEpoch(const std::string& token)
{
    const auto reject = [&token]() -> UtcDateTime {
        throw std::runtime_error("TimeScales: '" + token
                                 + "' is not a supported DELTA_AT epoch token");
    };

    if (token.size() < 2 || token.front() != '@') {
        return reject();
    }

    const std::string body = token.substr(1);
    const std::size_t firstDash = body.find('-');
    const std::size_t secondDash = body.find('-', firstDash + 1);
    if (firstDash == std::string::npos || secondDash == std::string::npos) {
        return reject();
    }

    UtcDateTime utc{};
    try {
        utc.year = std::stoi(body.substr(0, firstDash));
        utc.day = std::stoi(body.substr(secondDash + 1));
    } catch (const std::exception&) {
        return reject();
    }

    std::string month = body.substr(firstDash + 1, secondDash - firstDash - 1);
    for (char& c : month) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const auto found = std::find_if(kMonthAbbreviations.begin(), kMonthAbbreviations.end(),
                                    [&month](const char* name) { return month == name; });
    if (found == kMonthAbbreviations.end()) {
        return reject();
    }
    utc.month = static_cast<int>(std::distance(kMonthAbbreviations.begin(), found)) + 1;

    utc.hour = 0;
    utc.minute = 0;
    utc.second = 0.0;
    return utc;
}

} // namespace

double julianDateAtMidnightUtc(int year, int month, int day)
{
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        throw std::runtime_error("julianDateAtMidnightUtc: date out of range");
    }

    // Proleptic Gregorian day number. Valid for years after -4800, which covers every date
    // this project will ever hold; the formula's behaviour before that is not relied upon.
    const long long a = (14 - month) / 12;
    const long long y = static_cast<long long>(year) + 4800 - a;
    const long long m = static_cast<long long>(month) + 12 * a - 3;
    const long long jdn = static_cast<long long>(day) + (153 * m + 2) / 5 + 365 * y + y / 4
                        - y / 100 + y / 400 - 32045;
    return static_cast<double>(jdn) - 0.5;
}

void civilDateFromJulianDayNumber(long long julianDayNumber, int& year, int& month, int& day)
{
    long long a = julianDayNumber + 32044;
    long long b = (4 * a + 3) / 146097;
    long long c = a - (146097 * b) / 4;
    long long d = (4 * c + 3) / 1461;
    long long e = c - (1461 * d) / 4;
    long long m = (5 * e + 2) / 153;

    day = static_cast<int>(e - (153 * m + 2) / 5 + 1);
    month = static_cast<int>(m + 3 - 12 * (m / 10));
    year = static_cast<int>(100 * b + d - 4800 + m / 10);
}

double nominalJulianDateUtc(const UtcDateTime& utc)
{
    const double dayFraction = (static_cast<double>(utc.hour) * 3600.0
                              + static_cast<double>(utc.minute) * 60.0
                              + utc.second)
                             / Seconds::kSecondsPerDay;
    return julianDateAtMidnightUtc(utc.year, utc.month, utc.day) + dayFraction;
}

std::string formatUtc(const UtcDateTime& utc)
{
    char buffer[64];
    const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%09.6fZ",
                                      utc.year, utc.month, utc.day, utc.hour, utc.minute,
                                      utc.second);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
        throw std::runtime_error("formatUtc: formatting failed");
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}

TimeScales TimeScales::fromLeapSecondKernel(const TextKernel& kernel)
{
    TimeScales scales;

    scales.m_deltaTa = kernel.number("DELTET/DELTA_T_A");
    scales.m_k = kernel.number("DELTET/K");
    scales.m_eb = kernel.number("DELTET/EB");

    const std::vector<double> m = kernel.numbers("DELTET/M", 2);
    scales.m_m0 = m[0];
    scales.m_m1 = m[1];

    const std::vector<std::string>& table = kernel.tokens("DELTET/DELTA_AT");
    if (table.empty() || table.size() % 2 != 0) {
        throw std::runtime_error("TimeScales: DELTET/DELTA_AT must hold offset/epoch pairs");
    }
    scales.m_leapSeconds.reserve(table.size() / 2);
    for (std::size_t i = 0; i < table.size(); i += 2) {
        LeapSecondEntry entry;
        entry.taiMinusUtcSeconds = parseKernelNumber(table[i]);
        entry.effectiveFrom = parseDeltaAtEpoch(table[i + 1]);
        entry.sourceToken = table[i + 1];
        scales.m_leapSeconds.push_back(std::move(entry));
    }

    // The table must be ascending for the lookup below to be a simple scan. NAIF publishes it
    // that way; checking costs nothing and turns a silently wrong offset into a load failure.
    for (std::size_t i = 1; i < scales.m_leapSeconds.size(); ++i) {
        const double previous = nominalJulianDateUtc(scales.m_leapSeconds[i - 1].effectiveFrom);
        const double current = nominalJulianDateUtc(scales.m_leapSeconds[i].effectiveFrom);
        if (!(current > previous)) {
            throw std::runtime_error("TimeScales: DELTET/DELTA_AT is not in ascending order");
        }
    }

    return scales;
}

double TimeScales::taiMinusUtcSeconds(const UtcDateTime& utc) const
{
    // Look the table up at the start of the minute containing @p utc. Leap seconds take
    // effect only at 00:00:00 UTC, so this instant is always on the same side of every step
    // as the caller's instant -- including 23:59:60, which must keep the pre-step value.
    UtcDateTime lookup = utc;
    lookup.second = 0.0;
    const double query = nominalJulianDateUtc(lookup);

    double result = 0.0;
    bool found = false;
    for (const LeapSecondEntry& entry : m_leapSeconds) {
        if (query >= nominalJulianDateUtc(entry.effectiveFrom)) {
            result = entry.taiMinusUtcSeconds;
            found = true;
        } else {
            break;
        }
    }

    if (!found) {
        throw std::runtime_error(
            "TimeScales: " + formatUtc(utc)
            + " precedes the first DELTA_AT entry; TAI-UTC is undefined before 1972 and this "
              "project must not invent a value for it");
    }
    return result;
}

double TimeScales::tdbMinusTtSeconds(TdbEpoch tdb) const noexcept
{
    const double meanAnomaly = m_m0 + m_m1 * tdb.secondsPastJ2000();
    const double eccentricAnomaly = meanAnomaly + m_eb * std::sin(meanAnomaly);
    return m_k * std::sin(eccentricAnomaly);
}

TdbEpoch TimeScales::utcToTdb(const UtcDateTime& utc) const
{
    const double nominalDaysPastJ2000 = nominalJulianDateUtc(utc) - TdbEpoch::kJ2000JulianDate;
    const double ttPastJ2000 = nominalDaysPastJ2000 * Seconds::kSecondsPerDay
                             + taiMinusUtcSeconds(utc) + m_deltaTa;

    // TDB = TT + K sin(E(TDB)). The correction is under 2 ms and the iteration is a
    // contraction with ratio ~1e-10, so two passes are already exact in double; a third is
    // taken for free rather than reasoned about at every call site.
    TdbEpoch tdb = TdbEpoch::fromSecondsPastJ2000(ttPastJ2000);
    for (int iteration = 0; iteration < 3; ++iteration) {
        tdb = TdbEpoch::fromSecondsPastJ2000(ttPastJ2000 + tdbMinusTtSeconds(tdb));
    }
    return tdb;
}

UtcDateTime TimeScales::tdbToUtc(TdbEpoch tdb, bool* leapSecondAmbiguous) const
{
    if (leapSecondAmbiguous != nullptr) {
        *leapSecondAmbiguous = false;
    }

    const double ttPastJ2000 = tdb.secondsPastJ2000() - tdbMinusTtSeconds(tdb);

    // TAI-UTC depends on the UTC instant being solved for, so the offset and the calendar
    // date are found together. Seeding from the newest table entry converges in one step for
    // every instant after the last leap second, which is where the campaign lives.
    double taiMinusUtc = m_leapSeconds.back().taiMinusUtcSeconds;
    UtcDateTime utc{};

    for (int iteration = 0; iteration < 8; ++iteration) {
        const double utcNominalJd = TdbEpoch::kJ2000JulianDate
                                  + (ttPastJ2000 - m_deltaTa - taiMinusUtc)
                                        / Seconds::kSecondsPerDay;

        // Split into whole day and seconds-into-day. The +0.5 shifts the Julian-date
        // half-day offset so the day number is the civil day.
        const double shifted = utcNominalJd + 0.5;
        const double dayNumber = std::floor(shifted);
        double secondsIntoDay = (shifted - dayNumber) * Seconds::kSecondsPerDay;

        civilDateFromJulianDayNumber(static_cast<long long>(dayNumber), utc.year, utc.month,
                                     utc.day);
        utc.hour = static_cast<int>(secondsIntoDay / 3600.0);
        secondsIntoDay -= static_cast<double>(utc.hour) * 3600.0;
        utc.minute = static_cast<int>(secondsIntoDay / 60.0);
        utc.second = secondsIntoDay - static_cast<double>(utc.minute) * 60.0;

        // Guard the boundary case where rounding pushes the seconds field to exactly 60.
        if (utc.second >= 60.0) {
            utc.second -= 60.0;
            ++utc.minute;
        }
        if (utc.minute >= 60) {
            utc.minute -= 60;
            ++utc.hour;
        }

        const double refined = taiMinusUtcSeconds(utc);
        if (refined == taiMinusUtc) {
            return utc;
        }
        taiMinusUtc = refined;
    }

    // Failure to settle means the instant sits inside a positive leap second, where UTC is
    // genuinely not a function of TDB. Reporting the post-step representation and flagging it
    // is honest; silently returning one of the two answers would not be.
    if (leapSecondAmbiguous != nullptr) {
        *leapSecondAmbiguous = true;
    }
    return utc;
}

} // namespace sol::proto::frames
