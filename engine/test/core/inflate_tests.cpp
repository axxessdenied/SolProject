#include "sol/core/inflate.hpp"
#include "sol/test/test.hpp"

#include <cstring>
#include <string>
#include <vector>

using sol::core::adler32;
using sol::core::rawInflate;
using sol::core::zlibInflate;

namespace {

std::string toString(const std::vector<std::uint8_t>& bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

SOL_TEST(adler32MatchesKnownValues)
{
    const char* hello = "hello";
    SOL_CHECK(adler32(reinterpret_cast<const std::uint8_t*>(hello), 5) == 0x062C0215u);
    SOL_CHECK(adler32(nullptr, 0) == 1u);
}

SOL_TEST(inflateStoredBlock)
{
    // bfinal=1, btype=00 (stored), LEN=5, NLEN=~5, "hello"
    const std::uint8_t data[] = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'};
    std::vector<std::uint8_t> out;
    SOL_CHECK(rawInflate(data, sizeof(data), out));
    SOL_CHECK(toString(out) == "hello");
}

SOL_TEST(zlibInflateFixedHuffman)
{
    // "hello" compressed by .NET ZLibStream (fixed-Huffman block).
    const std::uint8_t data[] = {
        0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x15};
    std::vector<std::uint8_t> out;
    SOL_CHECK(zlibInflate(data, sizeof(data), out));
    SOL_CHECK(toString(out) == "hello");
}

SOL_TEST(zlibInflateWithBackReferences)
{
    // 360 bytes of repetitive text compressed by .NET ZLibStream: exercises
    // LZ77 matches, including distances spanning the repeated phrase.
    const std::uint8_t data[] = {0x78, 0xda, 0x2b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56,
                                 0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a,
                                 0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a,
                                 0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0xeb, 0x81, 0x79, 0xa3,
                                 0x8a, 0xc9, 0x52, 0x0c, 0x00, 0x2f, 0xc0, 0x82, 0x39};
    std::string expected;
    for (int i = 0; i < 8; ++i) {
        expected += "the quick brown fox jumps over the lazy dog. ";
    }
    std::vector<std::uint8_t> out;
    SOL_CHECK(zlibInflate(data, sizeof(data), out));
    SOL_CHECK(toString(out) == expected);
}

SOL_TEST(inflateRejectsCorruptInput)
{
    // Bad zlib header check
    const std::uint8_t badHeader[] = {0x78, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::vector<std::uint8_t> out;
    SOL_CHECK(!zlibInflate(badHeader, sizeof(badHeader), out));

    // Truncated fixed-huffman stream
    const std::uint8_t truncated[] = {0x78, 0xda, 0xcb, 0x48};
    out.clear();
    SOL_CHECK(!zlibInflate(truncated, sizeof(truncated), out));

    // Stored block with mismatched NLEN
    const std::uint8_t badStored[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
    out.clear();
    SOL_CHECK(!rawInflate(badStored, sizeof(badStored), out));

    // Corrupted Adler-32
    const std::uint8_t badChecksum[] = {
        0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x16};
    out.clear();
    SOL_CHECK(!zlibInflate(badChecksum, sizeof(badChecksum), out));
}
