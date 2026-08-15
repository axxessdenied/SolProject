#pragma once

#include <cstdint>

namespace sol::platform {

// Monotonic, high-resolution seconds since an arbitrary fixed epoch.
[[nodiscard]] double timeSeconds();

void sleepMilliseconds(std::uint32_t milliseconds);

} // namespace sol::platform
