#include "png.hpp"

#include "sol/core/inflate.hpp"
#include "sol/core/log.hpp"

#include <cstdlib>
#include <cstring>

namespace sol::cooker {

namespace {

std::uint32_t readBigEndian32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

int paethPredictor(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

} // namespace

bool decodePng(const std::uint8_t* data, std::size_t size, ImageRgba& out)
{
    static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8 + 25 || std::memcmp(data, kSignature, 8) != 0) {
        SOL_LOG_ERROR("png: bad signature");
        return false;
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t colorType = 0;
    std::uint8_t interlace = 0;
    std::vector<std::uint8_t> compressed;
    std::vector<std::uint8_t> palette; // RGB triples

    std::size_t offset = 8;
    bool sawEnd = false;
    while (offset + 12 <= size && !sawEnd) {
        const std::uint32_t chunkLength = readBigEndian32(data + offset);
        const std::uint8_t* chunkType = data + offset + 4;
        const std::uint8_t* chunkData = data + offset + 8;
        if (offset + 12 + chunkLength > size) {
            SOL_LOG_ERROR("png: truncated chunk");
            return false;
        }

        if (std::memcmp(chunkType, "IHDR", 4) == 0) {
            if (chunkLength != 13) {
                return false;
            }
            width = readBigEndian32(chunkData);
            height = readBigEndian32(chunkData + 4);
            bitDepth = chunkData[8];
            colorType = chunkData[9];
            interlace = chunkData[12];
        } else if (std::memcmp(chunkType, "PLTE", 4) == 0) {
            palette.assign(chunkData, chunkData + chunkLength);
        } else if (std::memcmp(chunkType, "IDAT", 4) == 0) {
            compressed.insert(compressed.end(), chunkData, chunkData + chunkLength);
        } else if (std::memcmp(chunkType, "IEND", 4) == 0) {
            sawEnd = true;
        }
        // ancillary chunks are skipped

        offset += 12 + chunkLength;
    }

    if (width == 0 || height == 0 || !sawEnd || compressed.empty()) {
        SOL_LOG_ERROR("png: missing required chunks");
        return false;
    }
    if (bitDepth != 8) {
        SOL_LOG_ERROR("png: only 8-bit depth supported (got %u)", bitDepth);
        return false;
    }
    if (interlace != 0) {
        SOL_LOG_ERROR("png: interlaced images not supported");
        return false;
    }

    int channels = 0;
    switch (colorType) {
    case 0: channels = 1; break; // gray
    case 2: channels = 3; break; // RGB
    case 3: channels = 1; break; // palette index
    case 4: channels = 2; break; // gray + alpha
    case 6: channels = 4; break; // RGBA
    default: SOL_LOG_ERROR("png: unsupported color type %u", colorType); return false;
    }
    if (colorType == 3 && palette.empty()) {
        SOL_LOG_ERROR("png: palette image without PLTE");
        return false;
    }

    std::vector<std::uint8_t> raw;
    if (!core::zlibInflate(compressed.data(), compressed.size(), raw)) {
        SOL_LOG_ERROR("png: inflate failed");
        return false;
    }

    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    if (raw.size() != (stride + 1) * height) {
        SOL_LOG_ERROR("png: decompressed size mismatch");
        return false;
    }

    // Undo per-scanline filters in place (into a clean buffer).
    std::vector<std::uint8_t> image(stride * height);
    const int bpp = channels; // bytes per pixel at 8-bit depth
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t filter = raw[(stride + 1) * y];
        const std::uint8_t* src = raw.data() + (stride + 1) * y + 1;
        std::uint8_t* dst = image.data() + stride * y;
        const std::uint8_t* prior = y > 0 ? image.data() + stride * (y - 1) : nullptr;

        for (std::size_t x = 0; x < stride; ++x) {
            const int a = x >= static_cast<std::size_t>(bpp) ? dst[x - bpp] : 0;
            const int b = prior != nullptr ? prior[x] : 0;
            const int c = (prior != nullptr && x >= static_cast<std::size_t>(bpp)) ? prior[x - bpp] : 0;

            int value = src[x];
            switch (filter) {
            case 0: break;
            case 1: value += a; break;
            case 2: value += b; break;
            case 3: value += (a + b) / 2; break;
            case 4: value += paethPredictor(a, b, c); break;
            default: SOL_LOG_ERROR("png: bad filter %u", filter); return false;
            }
            dst[x] = static_cast<std::uint8_t>(value & 0xFF);
        }
    }

    // Expand to RGBA8.
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const std::uint8_t* src = image.data() + i * channels;
        std::uint8_t* dst = out.pixels.data() + i * 4;
        switch (colorType) {
        case 0: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255; break;
        case 2: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; break;
        case 3: {
            const std::size_t p = static_cast<std::size_t>(src[0]) * 3;
            if (p + 2 >= palette.size()) {
                SOL_LOG_ERROR("png: palette index out of range");
                return false;
            }
            dst[0] = palette[p];
            dst[1] = palette[p + 1];
            dst[2] = palette[p + 2];
            dst[3] = 255;
            break;
        }
        case 4: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1]; break;
        case 6: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; break;
        default: return false;
        }
    }
    return true;
}

} // namespace sol::cooker
