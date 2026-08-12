#pragma once

#include <string>
#include <string_view>

namespace sol::proto {

/// Minimal pass/fail accumulator for headless prototype scenarios.
///
/// P1a is deliberately dependency-free (no test framework has been accepted under
/// ADR 0007), so scenarios report through process exit codes and this accumulator rather
/// than through a framework. It is intentionally small: it records failures, prints them to
/// stderr as they happen, and produces the process exit code.
///
/// Failures never throw and never abort. A scenario must be able to report every failure it
/// found in one run, because rerunning to discover the second problem wastes a measurement.
class CheckContext {
public:
    /// Records a passing or failing check. @p description states what was expected.
    void check(bool condition, std::string_view description);

    /// Records a failure unconditionally.
    void fail(std::string_view description);

    /// Compares two doubles bit-for-bit via their IEEE-754 patterns.
    ///
    /// Bitwise rather than tolerance-based: ADR 0010 asserts same-machine bit-exactness, so
    /// a tolerance here would hide exactly the regression this exists to catch. Two NaNs
    /// with the same payload compare equal, which is the intended behavior for a
    /// reproducibility check.
    void checkBitsEqual(double actual, double expected, std::string_view description);

    [[nodiscard]] unsigned passedCount() const noexcept { return m_passed; }
    [[nodiscard]] unsigned failedCount() const noexcept { return m_failed; }

    /// Prints a one-line summary and returns EXIT_SUCCESS when nothing failed.
    [[nodiscard]] int summarize(std::string_view scenarioName) const;

private:
    unsigned m_passed{0};
    unsigned m_failed{0};
};

} // namespace sol::proto
