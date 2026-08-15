#pragma once

#include "png.hpp"

#include <cstdint>
#include <vector>

namespace sol::cooker {

// Encodes an RGBA image as BC1 (opaque, alpha ignored). Output is
// ceil(w/4)*ceil(h/4) blocks of 8 bytes.
[[nodiscard]] std::vector<std::uint8_t> encodeBc1(const ImageRgba& image);

// Box-filter downsample by 2 in each dimension (floor to 1).
[[nodiscard]] ImageRgba downsampleHalf(const ImageRgba& image);

} // namespace sol::cooker
