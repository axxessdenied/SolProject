#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sol::proto::frames {

/// SHA-256 over raw bytes, returned as 64 lowercase hex characters.
///
/// ADR 0008 requires a checksum with every golden fixture. A checksum recorded only in a
/// document is a claim; this makes it a runtime precondition, so a scenario that silently
/// reads an edited or truncated kernel fails loudly instead of producing plausible numbers
/// from the wrong data.
///
/// Implemented here rather than taken from a library because P1a is dependency-free under
/// ADR 0007 and no package has an accepted need. Roughly a hundred lines of well-specified
/// arithmetic is a smaller commitment than a dependency review, and it is verified against
/// the published test vectors in FramesSelfCheck.
[[nodiscard]] std::string sha256Hex(const void* data, std::size_t byteCount);

[[nodiscard]] std::string sha256Hex(std::string_view data);

/// Hashes a file's exact bytes.
///
/// Opened in binary mode deliberately: the fixtures are checked in with whatever line
/// endings NAIF and Horizons served, and a text-mode read would translate them and produce a
/// digest that never matches the published one. Throws std::runtime_error if the file cannot
/// be read.
[[nodiscard]] std::string sha256HexOfFile(const std::filesystem::path& path);

} // namespace sol::proto::frames
