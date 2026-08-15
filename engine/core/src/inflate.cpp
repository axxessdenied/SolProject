#include "sol/core/inflate.hpp"

#include <cstring>

// RFC 1951 DEFLATE decoder in the style of Mark Adler's puff: canonical
// Huffman decoding via per-length symbol counts. Simple and correct first;
// speed later if profiling ever cares (it is cook-time/load-time code).

namespace sol::core {

namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLitLenSymbols = 288;
constexpr int kMaxDistSymbols = 30;
constexpr int kMaxCodeLenSymbols = 19;

struct BitReader
{
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t bytePos = 0;
    int bitPos = 0; // 0..7, LSB first
    bool overrun = false;

    [[nodiscard]] int bit()
    {
        if (bytePos >= size) {
            overrun = true;
            return 0;
        }
        const int value = (data[bytePos] >> bitPos) & 1;
        if (++bitPos == 8) {
            bitPos = 0;
            ++bytePos;
        }
        return value;
    }

    [[nodiscard]] std::uint32_t bits(int count)
    {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value |= static_cast<std::uint32_t>(bit()) << i;
        }
        return value;
    }

    void alignToByte()
    {
        if (bitPos != 0) {
            bitPos = 0;
            ++bytePos;
        }
    }
};

struct Huffman
{
    // count[n] = number of codes of length n; symbol[] = symbols sorted by code.
    std::uint16_t count[kMaxBits + 1] = {};
    std::uint16_t symbol[kMaxLitLenSymbols] = {};

    // Builds canonical codes from code lengths; false on an over-subscribed set.
    [[nodiscard]] bool build(const std::uint8_t* lengths, int symbolCount)
    {
        for (int i = 0; i <= kMaxBits; ++i) {
            count[i] = 0;
        }
        for (int i = 0; i < symbolCount; ++i) {
            ++count[lengths[i]];
        }
        if (count[0] == symbolCount) {
            return true; // no codes at all: legal for an unused distance table
        }

        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) {
                return false; // over-subscribed
            }
        }

        std::uint16_t offsets[kMaxBits + 1] = {};
        for (int len = 1; len < kMaxBits; ++len) {
            offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + count[len]);
        }
        for (int i = 0; i < symbolCount; ++i) {
            if (lengths[i] != 0) {
                symbol[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
            }
        }
        return true;
    }

    [[nodiscard]] int decode(BitReader& reader) const
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= reader.bit();
            const int lenCount = count[len];
            if (code - first < lenCount) {
                return symbol[index + (code - first)];
            }
            index += lenCount;
            first = (first + lenCount) << 1;
            code <<= 1;
        }
        return -1; // invalid code
    }
};

// Length codes 257..285: base lengths and extra bits.
constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19, 23, 27,
                                           31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

// Distance codes 0..29.
constexpr std::uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                         33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                         1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlockData(BitReader& reader, const Huffman& litLen, const Huffman& dist,
                      std::vector<std::uint8_t>& out)
{
    while (true) {
        const int symbol = litLen.decode(reader);
        if (symbol < 0 || reader.overrun) {
            return false;
        }
        if (symbol < 256) {
            out.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) {
            return true; // end of block
        }

        const int lengthIndex = symbol - 257;
        if (lengthIndex >= 29) {
            return false;
        }
        const std::size_t length = kLengthBase[lengthIndex] + reader.bits(kLengthExtra[lengthIndex]);

        const int distSymbol = dist.decode(reader);
        if (distSymbol < 0 || distSymbol >= 30) {
            return false;
        }
        const std::size_t distance = kDistBase[distSymbol] + reader.bits(kDistExtra[distSymbol]);
        if (reader.overrun || distance > out.size()) {
            return false;
        }

        // Byte-by-byte copy: overlapping matches are the point (RLE-style).
        std::size_t from = out.size() - distance;
        for (std::size_t i = 0; i < length; ++i) {
            out.push_back(out[from + i]);
        }
    }
}

bool inflateFixedBlock(BitReader& reader, std::vector<std::uint8_t>& out)
{
    std::uint8_t litLenLengths[kMaxLitLenSymbols];
    for (int i = 0; i < 144; ++i) litLenLengths[i] = 8;
    for (int i = 144; i < 256; ++i) litLenLengths[i] = 9;
    for (int i = 256; i < 280; ++i) litLenLengths[i] = 7;
    for (int i = 280; i < 288; ++i) litLenLengths[i] = 8;

    std::uint8_t distLengths[kMaxDistSymbols];
    for (int i = 0; i < kMaxDistSymbols; ++i) distLengths[i] = 5;

    Huffman litLen;
    Huffman dist;
    if (!litLen.build(litLenLengths, kMaxLitLenSymbols) || !dist.build(distLengths, kMaxDistSymbols)) {
        return false;
    }
    return inflateBlockData(reader, litLen, dist, out);
}

bool inflateDynamicBlock(BitReader& reader, std::vector<std::uint8_t>& out)
{
    const int hlit = static_cast<int>(reader.bits(5)) + 257;
    const int hdist = static_cast<int>(reader.bits(5)) + 1;
    const int hclen = static_cast<int>(reader.bits(4)) + 4;
    if (hlit > kMaxLitLenSymbols || hdist > kMaxDistSymbols) {
        return false;
    }

    static constexpr std::uint8_t kCodeLenOrder[kMaxCodeLenSymbols] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
    std::uint8_t codeLenLengths[kMaxCodeLenSymbols] = {};
    for (int i = 0; i < hclen; ++i) {
        codeLenLengths[kCodeLenOrder[i]] = static_cast<std::uint8_t>(reader.bits(3));
    }

    Huffman codeLenHuffman;
    if (!codeLenHuffman.build(codeLenLengths, kMaxCodeLenSymbols)) {
        return false;
    }

    std::uint8_t lengths[kMaxLitLenSymbols + kMaxDistSymbols] = {};
    int index = 0;
    while (index < hlit + hdist) {
        const int symbol = codeLenHuffman.decode(reader);
        if (symbol < 0 || reader.overrun) {
            return false;
        }
        if (symbol < 16) {
            lengths[index++] = static_cast<std::uint8_t>(symbol);
        } else if (symbol == 16) {
            if (index == 0) {
                return false;
            }
            const std::uint8_t previous = lengths[index - 1];
            const int repeat = 3 + static_cast<int>(reader.bits(2));
            for (int i = 0; i < repeat && index < hlit + hdist; ++i) {
                lengths[index++] = previous;
            }
        } else if (symbol == 17) {
            const int repeat = 3 + static_cast<int>(reader.bits(3));
            index += repeat;
        } else {
            const int repeat = 11 + static_cast<int>(reader.bits(7));
            index += repeat;
        }
        if (index > hlit + hdist) {
            return false;
        }
    }
    if (lengths[256] == 0) {
        return false; // end-of-block code must exist
    }

    Huffman litLen;
    Huffman dist;
    if (!litLen.build(lengths, hlit) || !dist.build(lengths + hlit, hdist)) {
        return false;
    }
    return inflateBlockData(reader, litLen, dist, out);
}

bool inflateStoredBlock(BitReader& reader, std::vector<std::uint8_t>& out)
{
    reader.alignToByte();
    if (reader.bytePos + 4 > reader.size) {
        return false;
    }
    const std::uint16_t length =
        static_cast<std::uint16_t>(reader.data[reader.bytePos] | (reader.data[reader.bytePos + 1] << 8));
    const std::uint16_t inverted = static_cast<std::uint16_t>(reader.data[reader.bytePos + 2] |
                                                              (reader.data[reader.bytePos + 3] << 8));
    reader.bytePos += 4;
    if (length != static_cast<std::uint16_t>(~inverted)) {
        return false;
    }
    if (reader.bytePos + length > reader.size) {
        return false;
    }
    out.insert(out.end(), reader.data + reader.bytePos, reader.data + reader.bytePos + length);
    reader.bytePos += length;
    return true;
}

} // namespace

bool rawInflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
{
    BitReader reader = {data, size, 0, 0, false};

    while (true) {
        const int final = reader.bit();
        const std::uint32_t type = reader.bits(2);
        if (reader.overrun) {
            return false;
        }

        bool ok = false;
        switch (type) {
        case 0: ok = inflateStoredBlock(reader, out); break;
        case 1: ok = inflateFixedBlock(reader, out); break;
        case 2: ok = inflateDynamicBlock(reader, out); break;
        default: return false;
        }
        if (!ok || reader.overrun) {
            return false;
        }
        if (final != 0) {
            return true;
        }
    }
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size)
{
    constexpr std::uint32_t kModulus = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % kModulus;
        b = (b + a) % kModulus;
    }
    return (b << 16) | a;
}

bool zlibInflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
{
    if (size < 6) {
        return false;
    }
    const std::uint8_t cmf = data[0];
    const std::uint8_t flg = data[1];
    if ((cmf & 0x0F) != 8) {
        return false; // not deflate
    }
    if (((static_cast<unsigned>(cmf) << 8) | flg) % 31 != 0) {
        return false; // header check failed
    }
    if ((flg & 0x20) != 0) {
        return false; // preset dictionaries unsupported
    }

    const std::size_t startSize = out.size();
    if (!rawInflate(data + 2, size - 6, out)) {
        return false;
    }

    const std::uint8_t* checksum = data + size - 4;
    const std::uint32_t expected = (static_cast<std::uint32_t>(checksum[0]) << 24) |
                                   (static_cast<std::uint32_t>(checksum[1]) << 16) |
                                   (static_cast<std::uint32_t>(checksum[2]) << 8) |
                                   static_cast<std::uint32_t>(checksum[3]);
    return adler32(out.data() + startSize, out.size() - startSize) == expected;
}

} // namespace sol::core
