#pragma once

#include "sol/assets/formats.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::cooker {

// A baked font: one shared R8 atlas plus the style and glyph tables that index
// it. Mirrors the .sfont file layout so writing it out is a memcpy.
struct BakedFont
{
    std::uint32_t atlasWidth = 0;
    std::uint32_t atlasHeight = 0;
    std::vector<std::uint8_t> atlas; // R8 coverage, row-major
    std::vector<assets::FontStyleRecord> styles;
    std::vector<assets::FontGlyphRecord> glyphs;
};

// Bakes the font described by a `.font` manifest (TOML). `sourceDirectory` is
// where the manifest lives; style `source` paths resolve against it.
//
// Deterministic for a given manifest and set of TTFs: styles bake in manifest
// order, glyphs in codepoint order, and the shelf packer never consults
// anything but the glyph sizes it is handed. Cooked output is therefore
// byte-identical across runs and machines.
[[nodiscard]] bool bakeFont(const char* manifestText,
                            std::size_t manifestLength,
                            const std::string& sourceDirectory,
                            BakedFont& out,
                            std::string* outError = nullptr);

// Serializes a baked font into .sfont bytes.
[[nodiscard]] std::vector<std::uint8_t> encodeFont(const BakedFont& font);

} // namespace sol::cooker
