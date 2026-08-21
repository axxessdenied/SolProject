#pragma once

// The cooked texture format, as a pure function of an image.
//
// ⚑ THIS IS THE THIRD TIME THIS EXACT GAP HAS BEEN CLOSED, AND THE FIRST TWO
// ARE WHY IT IS WORTH A COMMENT. `encodeSound` has always been a library
// function with `main.cpp` only writing the bytes, which is why sound has a
// round-trip test; `encodeMesh` lived in `main.cpp` instead, so `cooker.unit`
// could not reach the `.smesh` layout at all and the D checkpoint's gap
// survived three slices of being written down as "still open" - the E1-E3 debt
// slice moved it. The texture path was left in the mesh's shape and nobody
// noticed, because until stage G nothing but the cooker needed it.
//
// Now two callers do: the cooker writes it, and the Forge uploads it so an
// author sees the BC1 the game will actually load rather than the RGBA the
// document built. A second copy of the mip loop in the tool would be two
// answers to what a cooked texture is.

#include "png.hpp"

#include "sol/assets/asset_loader.hpp"

#include <cstdint>
#include <vector>

namespace sol::cooker {

// BC1 with a full box-filtered mip chain down to 1x1. Alpha is discarded, which
// is what BC1 is.
[[nodiscard]] assets::TextureData encodeTexture(const ImageRgba& image);

// The `.stex` byte layout: TextureFileHeader, then mip payloads from mip 0 down.
[[nodiscard]] std::vector<std::uint8_t> serializeTexture(const assets::TextureData& data);

} // namespace sol::cooker
