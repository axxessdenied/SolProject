/// @file
/// The smallest assertion helper that reports usefully on failure.
///
/// Deliberately not a test framework. See tests/CMakeLists.txt for why one has not been
/// selected. This header stays small enough that replacing it later is a mechanical edit.

#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace sol::test {

/// Accumulates failures so one run reports every problem rather than the first.
class CheckContext {
public:
    explicit CheckContext(std::string_view name)
        : m_name(name)
    {
    }

    void check(bool condition, std::string_view what)
    {
        ++m_total;
        if (!condition) {
            ++m_failures;
            std::printf("  FAIL  %s\n", std::string(what).c_str());
        }
    }

    /// Reports detail only on failure; a passing run stays quiet enough to read.
    void checkEqual(const auto& actual, const auto& expected, std::string_view what)
    {
        ++m_total;
        if (!(actual == expected)) {
            ++m_failures;
            std::printf("  FAIL  %s\n", std::string(what).c_str());
        }
    }

    /// Prints the summary and returns a process exit code.
    [[nodiscard]] int finish() const
    {
        std::printf("%s: %d checks, %d failed\n",
                    std::string(m_name).c_str(),
                    m_total,
                    m_failures);
        return m_failures == 0 ? 0 : 1;
    }

private:
    std::string m_name;
    int m_total = 0;
    int m_failures = 0;
};

} // namespace sol::test
