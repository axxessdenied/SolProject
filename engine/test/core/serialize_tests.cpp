#include "sol/core/serialize.hpp"
#include "sol/test/test.hpp"

#include <cstdint>
#include <string>

namespace {

using sol::core::BinaryReader;
using sol::core::BinaryWriter;

} // namespace

SOL_TEST(serialize_scalarAndStringRoundTrip)
{
    BinaryWriter writer;
    writer.write(std::uint32_t{0xDEAD'BEEFu});
    writer.write(-123);
    writer.write(3.5);
    writer.writeString("sol engine");
    writer.write(std::uint8_t{7});

    BinaryReader reader(writer.data());
    std::uint32_t u = 0;
    int i = 0;
    double d = 0.0;
    std::string s;
    std::uint8_t b = 0;
    SOL_CHECK(reader.read(u) && u == 0xDEAD'BEEFu);
    SOL_CHECK(reader.read(i) && i == -123);
    SOL_CHECK(reader.read(d) && d == 3.5);
    SOL_CHECK(reader.readString(s) && s == "sol engine");
    SOL_CHECK(reader.read(b) && b == 7);
    SOL_CHECK(reader.remaining() == 0);
    SOL_CHECK(!reader.failed());
}

SOL_TEST(serialize_truncatedReadFailsAndSticks)
{
    BinaryWriter writer;
    writer.write(std::uint16_t{99});

    BinaryReader reader(writer.data());
    std::uint64_t tooBig = 0;
    SOL_CHECK(!reader.read(tooBig));
    SOL_CHECK(tooBig == 0); // output untouched on failure
    SOL_CHECK(reader.failed());
    std::uint8_t small = 0;
    SOL_CHECK(!reader.read(small)); // sticky: even a fitting read now fails
}

SOL_TEST(serialize_corruptStringLengthFails)
{
    BinaryWriter writer;
    writer.write(std::uint32_t{1000}); // claims 1000 bytes, provides none

    BinaryReader reader(writer.data());
    std::string s;
    SOL_CHECK(!reader.readString(s));
    SOL_CHECK(reader.failed());
}

SOL_TEST(serialize_skipAndOverwrite)
{
    BinaryWriter writer;
    const std::size_t patchOffset = writer.size();
    writer.write(std::uint32_t{0}); // placeholder
    writer.write(std::uint32_t{42});
    writer.overwriteAt(patchOffset, std::uint32_t{7});

    BinaryReader reader(writer.data());
    SOL_CHECK(reader.skip(sizeof(std::uint32_t)));
    std::uint32_t v = 0;
    SOL_CHECK(reader.read(v) && v == 42);
    SOL_CHECK(!reader.skip(1)); // nothing left

    BinaryReader reader2(writer.data());
    std::uint32_t patched = 0;
    SOL_CHECK(reader2.read(patched) && patched == 7);
}
