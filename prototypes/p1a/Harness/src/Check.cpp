#include "Sol/Proto/Harness/Check.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace sol::proto {

void CheckContext::check(bool condition, std::string_view description) {
    if (condition) {
        ++m_passed;
        return;
    }
    ++m_failed;
    std::cerr << "FAIL: " << description << '\n';
}

void CheckContext::fail(std::string_view description) {
    check(false, description);
}

void CheckContext::checkBitsEqual(double actual, double expected, std::string_view description) {
    const auto actualBits = std::bit_cast<std::uint64_t>(actual);
    const auto expectedBits = std::bit_cast<std::uint64_t>(expected);
    if (actualBits == expectedBits) {
        ++m_passed;
        return;
    }
    ++m_failed;
    std::cerr << "FAIL: " << description << "\n      actual bits   0x" << std::hex << actualBits
              << "\n      expected bits 0x" << expectedBits << std::dec << '\n';
}

int CheckContext::summarize(std::string_view scenarioName) const {
    std::cout << scenarioName << ": " << m_passed << " passed, " << m_failed << " failed\n";
    return m_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace sol::proto
