#pragma once

// Cooked binary asset formats shared by tools/cooker (writer) and the
// runtime loader (reader). Little-endian, tightly packed.

#include <cstdint>

namespace sol::assets {

inline constexpr std::uint32_t kMeshMagic = 0x48534D53u;    // "SMSH"
inline constexpr std::uint32_t kTextureMagic = 0x58455453u; // "STEX"
inline constexpr std::uint32_t kFormatVersion = 1;

// .smesh layout: MeshFileHeader, vertexCount * MeshVertex, indexCount * uint32.
struct MeshFileHeader
{
    std::uint32_t magic = kMeshMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
};

struct MeshVertex
{
    float position[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(MeshVertex) == 32);

enum class TextureFormat : std::uint32_t
{
    BC1 = 1, // 8 bytes per 4x4 block, opaque RGB
};

// .stex layout: TextureFileHeader, then mip payloads from mip 0 down,
// each ceil(w/4)*ceil(h/4)*8 bytes of BC1 blocks.
struct TextureFileHeader
{
    std::uint32_t magic = kTextureMagic;
    std::uint32_t version = kFormatVersion;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    TextureFormat format = TextureFormat::BC1;
};

[[nodiscard]] constexpr std::uint32_t bc1MipByteSize(std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t blocksX = (width + 3) / 4;
    const std::uint32_t blocksY = (height + 3) / 4;
    return blocksX * blocksY * 8;
}

[[nodiscard]] constexpr std::uint32_t mipDimension(std::uint32_t base, std::uint32_t mip)
{
    const std::uint32_t d = base >> mip;
    return d > 0 ? d : 1;
}

} // namespace sol::assets
