#pragma once

#include "sol/assets/formats.hpp"

#include <cstdint>
#include <vector>

namespace sol::assets {

struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct TextureData
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::BC1;
    std::vector<std::vector<std::uint8_t>> mips; // mip 0 first
};

// Synchronous cooked-asset loading (async lands with the job system, Phase 3).
[[nodiscard]] bool loadMesh(const char* path, MeshData& out);
[[nodiscard]] bool loadTexture(const char* path, TextureData& out);

} // namespace sol::assets
