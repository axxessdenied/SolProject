#pragma once

#include <cstdint>

namespace sol::core {

struct Version
{
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
};

[[nodiscard]] Version engineVersion();

// "major.minor.patch"; points at storage with static lifetime.
[[nodiscard]] const char* engineVersionString();

} // namespace sol::core
