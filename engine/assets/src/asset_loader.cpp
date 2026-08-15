#include "sol/assets/asset_loader.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <cstring>

namespace sol::assets {

bool loadMesh(const char* path, MeshData& out)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path, bytes)) {
        SOL_LOG_ERROR("Failed to read mesh: %s", path);
        return false;
    }

    MeshFileHeader header = {};
    if (bytes.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kMeshMagic || header.version != kFormatVersion) {
        SOL_LOG_ERROR("Bad mesh header: %s", path);
        return false;
    }

    const std::size_t vertexBytes = static_cast<std::size_t>(header.vertexCount) * sizeof(MeshVertex);
    const std::size_t indexBytes = static_cast<std::size_t>(header.indexCount) * sizeof(std::uint32_t);
    if (bytes.size() != sizeof(header) + vertexBytes + indexBytes) {
        SOL_LOG_ERROR("Mesh size mismatch: %s", path);
        return false;
    }

    out.vertices.resize(header.vertexCount);
    out.indices.resize(header.indexCount);
    std::memcpy(out.vertices.data(), bytes.data() + sizeof(header), vertexBytes);
    std::memcpy(out.indices.data(), bytes.data() + sizeof(header) + vertexBytes, indexBytes);
    return true;
}

bool loadTexture(const char* path, TextureData& out)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path, bytes)) {
        SOL_LOG_ERROR("Failed to read texture: %s", path);
        return false;
    }

    TextureFileHeader header = {};
    if (bytes.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kTextureMagic || header.version != kFormatVersion ||
        header.format != TextureFormat::BC1 || header.mipCount == 0) {
        SOL_LOG_ERROR("Bad texture header: %s", path);
        return false;
    }

    out.width = header.width;
    out.height = header.height;
    out.format = header.format;
    out.mips.clear();

    std::size_t offset = sizeof(header);
    for (std::uint32_t mip = 0; mip < header.mipCount; ++mip) {
        const std::uint32_t mipBytes =
            bc1MipByteSize(mipDimension(header.width, mip), mipDimension(header.height, mip));
        if (offset + mipBytes > bytes.size()) {
            SOL_LOG_ERROR("Texture truncated at mip %u: %s", mip, path);
            return false;
        }
        out.mips.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + mipBytes));
        offset += mipBytes;
    }
    return offset == bytes.size();
}

} // namespace sol::assets
