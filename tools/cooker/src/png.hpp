#pragma once

#include <cstdint>
#include <vector>

namespace sol::cooker {

struct ImageRgba
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8, row-major, no padding
};

// Decodes an 8-bit, non-interlaced PNG (gray, gray+alpha, RGB, RGBA, palette).
[[nodiscard]] bool decodePng(const std::uint8_t* data, std::size_t size, ImageRgba& out);

} // namespace sol::cooker
