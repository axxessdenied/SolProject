#include "Sol/Proto/Frames/Sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace sol::proto::frames {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotateRight(std::uint32_t value, unsigned bits) noexcept
{
    return (value >> bits) | (value << (32u - bits));
}

/// Streaming SHA-256 state. Kept private to this translation unit: the public surface is the
/// three one-shot functions, because every caller in this increment hashes a complete
/// fixture and a partial-update API would only invite misuse.
class Sha256State {
public:
    void update(const unsigned char* data, std::size_t byteCount)
    {
        m_totalBytes += byteCount;
        while (byteCount > 0) {
            const std::size_t take = std::min(byteCount, sizeof(m_block) - m_blockFill);
            std::memcpy(m_block + m_blockFill, data, take);
            m_blockFill += take;
            data += take;
            byteCount -= take;
            if (m_blockFill == sizeof(m_block)) {
                compress(m_block);
                m_blockFill = 0;
            }
        }
    }

    [[nodiscard]] std::string finish()
    {
        const std::uint64_t bitCount = m_totalBytes * 8u;

        const unsigned char padStart = 0x80u;
        update(&padStart, 1);

        const unsigned char zero = 0x00u;
        while (m_blockFill != 56) {
            update(&zero, 1);
        }

        // The length field is appended directly rather than through update(), which would
        // corrupt m_totalBytes and therefore the very value being written.
        for (int shift = 56; shift >= 0; shift -= 8) {
            m_block[m_blockFill++] = static_cast<unsigned char>((bitCount >> shift) & 0xffu);
        }
        compress(m_block);
        m_blockFill = 0;

        std::string hex;
        hex.reserve(64);
        constexpr char kDigits[] = "0123456789abcdef";
        for (const std::uint32_t word : m_hash) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                hex.push_back(kDigits[(word >> shift) & 0xfu]);
            }
        }
        return hex;
    }

private:
    void compress(const unsigned char* block) noexcept
    {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t i = 0; i < 16; ++i) {
            schedule[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24)
                        | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                        | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                        | (static_cast<std::uint32_t>(block[i * 4 + 3]));
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotateRight(schedule[i - 15], 7)
                                   ^ rotateRight(schedule[i - 15], 18)
                                   ^ (schedule[i - 15] >> 3);
            const std::uint32_t s1 = rotateRight(schedule[i - 2], 17)
                                   ^ rotateRight(schedule[i - 2], 19)
                                   ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }

        std::uint32_t a = m_hash[0];
        std::uint32_t b = m_hash[1];
        std::uint32_t c = m_hash[2];
        std::uint32_t d = m_hash[3];
        std::uint32_t e = m_hash[4];
        std::uint32_t f = m_hash[5];
        std::uint32_t g = m_hash[6];
        std::uint32_t h = m_hash[7];

        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t sigma1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[i] + schedule[i];
            const std::uint32_t sigma0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sigma0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        m_hash[0] += a;
        m_hash[1] += b;
        m_hash[2] += c;
        m_hash[3] += d;
        m_hash[4] += e;
        m_hash[5] += f;
        m_hash[6] += g;
        m_hash[7] += h;
    }

    std::array<std::uint32_t, 8> m_hash{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    unsigned char m_block[64]{};
    std::size_t m_blockFill{0};
    std::uint64_t m_totalBytes{0};
};

} // namespace

std::string sha256Hex(const void* data, std::size_t byteCount)
{
    Sha256State state;
    state.update(static_cast<const unsigned char*>(data), byteCount);
    return state.finish();
}

std::string sha256Hex(std::string_view data)
{
    return sha256Hex(data.data(), data.size());
}

std::string sha256HexOfFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("sha256HexOfFile: cannot open " + path.string());
    }

    Sha256State state;
    std::vector<char> buffer(64 * 1024);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) {
            state.update(reinterpret_cast<const unsigned char*>(buffer.data()),
                         static_cast<std::size_t>(got));
        }
    }
    if (file.bad()) {
        throw std::runtime_error("sha256HexOfFile: read error on " + path.string());
    }
    return state.finish();
}

} // namespace sol::proto::frames
