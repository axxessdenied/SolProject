#include "texture.hpp"

#include "bc1.hpp"

#include "sol/assets/formats.hpp"

#include <cstring>
#include <utility>

namespace sol::cooker {

assets::TextureData encodeTexture(const ImageRgba& image)
{
    assets::TextureData data;
    data.width = image.width;
    data.height = image.height;
    data.format = assets::TextureFormat::BC1;
    if (image.width == 0 || image.height == 0) {
        return data;
    }

    ImageRgba level = image;
    while (true) {
        data.mips.push_back(encodeBc1(level));
        if (level.width == 1 && level.height == 1) {
            break;
        }
        level = downsampleHalf(level);
    }
    return data;
}

std::vector<std::uint8_t> serializeTexture(const assets::TextureData& data)
{
    assets::TextureFileHeader header = {};
    header.width = data.width;
    header.height = data.height;
    header.format = data.format;
    header.mipCount = static_cast<std::uint32_t>(data.mips.size());

    std::vector<std::uint8_t> bytes(sizeof(header));
    std::memcpy(bytes.data(), &header, sizeof(header));
    for (const std::vector<std::uint8_t>& mip : data.mips) {
        bytes.insert(bytes.end(), mip.begin(), mip.end());
    }
    return bytes;
}

} // namespace sol::cooker
