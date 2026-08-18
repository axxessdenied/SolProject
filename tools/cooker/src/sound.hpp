#pragma once

#include "sol/assets/asset_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sol::cooker {

// RIFF/WAVE, uncompressed PCM: 8-bit unsigned, 16/24/32-bit signed, and
// 32-bit float all normalize to the int16 the cooked format stores. Written
// in-repo, like the PNG and glTF importers beside it (AGENTS.md section 5).
[[nodiscard]] bool importWav(const std::uint8_t* data, std::size_t size, assets::SoundData& out);

// Ogg Vorbis, through the vendored stb_vorbis. The only place in the project
// that decodes ogg - see docs/decisions/009-audio-decoder.md.
[[nodiscard]] bool importOgg(const std::uint8_t* data, std::size_t size, assets::SoundData& out);

// Serializes to the .saud layout in sol/assets/formats.hpp.
[[nodiscard]] std::vector<std::uint8_t> encodeSound(const assets::SoundData& sound);

} // namespace sol::cooker
