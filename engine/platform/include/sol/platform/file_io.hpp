#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sol::platform {

// Reads an entire binary file; returns false and leaves outBytes empty on failure.
[[nodiscard]] bool readFileBytes(const char* path, std::vector<std::uint8_t>& outBytes);

// Absolute directory containing the running executable, with a trailing separator.
[[nodiscard]] std::string executableDirectory();

} // namespace sol::platform
