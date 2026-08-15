#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sol::core {

// Decompresses a raw DEFLATE (RFC 1951) stream, appending to out.
[[nodiscard]] bool rawInflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

// Decompresses a zlib (RFC 1950) stream: header check + inflate + Adler-32 verify.
[[nodiscard]] bool zlibInflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

[[nodiscard]] std::uint32_t adler32(const std::uint8_t* data, std::size_t size);

} // namespace sol::core
