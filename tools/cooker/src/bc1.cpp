#include "bc1.hpp"

#include <algorithm>

namespace sol::cooker {

namespace {

std::uint16_t toRgb565(int r, int g, int b)
{
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void fromRgb565(std::uint16_t c, int& r, int& g, int& b)
{
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b = (c & 31) * 255 / 31;
}

int distanceSquared(int r0, int g0, int b0, int r1, int g1, int b1)
{
    const int dr = r0 - r1;
    const int dg = g0 - g1;
    const int db = b0 - b1;
    return dr * dr + dg * dg + db * db;
}

} // namespace

std::vector<std::uint8_t> encodeBc1(const ImageRgba& image)
{
    const std::uint32_t blocksX = (image.width + 3) / 4;
    const std::uint32_t blocksY = (image.height + 3) / 4;
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(blocksX) * blocksY * 8);

    for (std::uint32_t by = 0; by < blocksY; ++by) {
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            // Gather the 4x4 block (edge blocks clamp to the last row/column).
            int r[16];
            int g[16];
            int b[16];
            for (int i = 0; i < 16; ++i) {
                const std::uint32_t px = std::min(bx * 4 + static_cast<std::uint32_t>(i % 4),
                                                  image.width - 1);
                const std::uint32_t py = std::min(by * 4 + static_cast<std::uint32_t>(i / 4),
                                                  image.height - 1);
                const std::uint8_t* pixel = image.pixels.data() +
                                            (static_cast<std::size_t>(py) * image.width + px) * 4;
                r[i] = pixel[0];
                g[i] = pixel[1];
                b[i] = pixel[2];
            }

            // Endpoints: bounding-box corners along each channel, inset slightly
            // to reduce ringing (standard fast-BC1 heuristic).
            int minR = 255, minG = 255, minB = 255;
            int maxR = 0, maxG = 0, maxB = 0;
            for (int i = 0; i < 16; ++i) {
                minR = std::min(minR, r[i]);
                minG = std::min(minG, g[i]);
                minB = std::min(minB, b[i]);
                maxR = std::max(maxR, r[i]);
                maxG = std::max(maxG, g[i]);
                maxB = std::max(maxB, b[i]);
            }
            const int insetR = (maxR - minR) >> 4;
            const int insetG = (maxG - minG) >> 4;
            const int insetB = (maxB - minB) >> 4;
            minR += insetR;
            minG += insetG;
            minB += insetB;
            maxR -= insetR;
            maxG -= insetG;
            maxB -= insetB;

            std::uint16_t c0 = toRgb565(maxR, maxG, maxB);
            std::uint16_t c1 = toRgb565(minR, minG, minB);
            if (c0 < c1) {
                std::swap(c0, c1);
            } else if (c0 == c1) {
                // Degenerate (solid) block: any indices work with palette[0].
                out.push_back(static_cast<std::uint8_t>(c0 & 0xFF));
                out.push_back(static_cast<std::uint8_t>(c0 >> 8));
                out.push_back(static_cast<std::uint8_t>(c1 & 0xFF));
                out.push_back(static_cast<std::uint8_t>(c1 >> 8));
                out.insert(out.end(), 4, 0x00);
                continue;
            }

            // 4-color palette (c0 > c1 mode).
            int pr[4];
            int pg[4];
            int pb[4];
            fromRgb565(c0, pr[0], pg[0], pb[0]);
            fromRgb565(c1, pr[1], pg[1], pb[1]);
            pr[2] = (2 * pr[0] + pr[1]) / 3;
            pg[2] = (2 * pg[0] + pg[1]) / 3;
            pb[2] = (2 * pb[0] + pb[1]) / 3;
            pr[3] = (pr[0] + 2 * pr[1]) / 3;
            pg[3] = (pg[0] + 2 * pg[1]) / 3;
            pb[3] = (pb[0] + 2 * pb[1]) / 3;

            std::uint32_t indices = 0;
            for (int i = 0; i < 16; ++i) {
                int best = 0;
                int bestDistance = distanceSquared(r[i], g[i], b[i], pr[0], pg[0], pb[0]);
                for (int p = 1; p < 4; ++p) {
                    const int d = distanceSquared(r[i], g[i], b[i], pr[p], pg[p], pb[p]);
                    if (d < bestDistance) {
                        bestDistance = d;
                        best = p;
                    }
                }
                indices |= static_cast<std::uint32_t>(best) << (i * 2);
            }

            out.push_back(static_cast<std::uint8_t>(c0 & 0xFF));
            out.push_back(static_cast<std::uint8_t>(c0 >> 8));
            out.push_back(static_cast<std::uint8_t>(c1 & 0xFF));
            out.push_back(static_cast<std::uint8_t>(c1 >> 8));
            out.push_back(static_cast<std::uint8_t>(indices & 0xFF));
            out.push_back(static_cast<std::uint8_t>((indices >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((indices >> 16) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((indices >> 24) & 0xFF));
        }
    }
    return out;
}

ImageRgba downsampleHalf(const ImageRgba& image)
{
    ImageRgba result;
    result.width = std::max(1u, image.width / 2);
    result.height = std::max(1u, image.height / 2);
    result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);

    for (std::uint32_t y = 0; y < result.height; ++y) {
        for (std::uint32_t x = 0; x < result.width; ++x) {
            const std::uint32_t sx = std::min(x * 2, image.width - 1);
            const std::uint32_t sy = std::min(y * 2, image.height - 1);
            const std::uint32_t sx1 = std::min(sx + 1, image.width - 1);
            const std::uint32_t sy1 = std::min(sy + 1, image.height - 1);

            std::uint8_t* dst = result.pixels.data() +
                                (static_cast<std::size_t>(y) * result.width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const int sum =
                    image.pixels[(static_cast<std::size_t>(sy) * image.width + sx) * 4 + c] +
                    image.pixels[(static_cast<std::size_t>(sy) * image.width + sx1) * 4 + c] +
                    image.pixels[(static_cast<std::size_t>(sy1) * image.width + sx) * 4 + c] +
                    image.pixels[(static_cast<std::size_t>(sy1) * image.width + sx1) * 4 + c];
                dst[c] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
    return result;
}

} // namespace sol::cooker
