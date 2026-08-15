#pragma once

// Binary serialization primitives (engine plan Phase 3). Convention: every
// serialized blob starts with a u32 magic and a u32 format version written
// by its owner, checked on read. Integers are stored little-endian native
// (x86-64 now; a byte-order shim belongs here if a big-endian port ever
// happens).

#include "sol/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sol::core {

class BinaryWriter
{
public:
    template <typename T>
    void write(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "serialize non-POD types field by field");
        writeBytes(&value, sizeof(T));
    }

    // u32 length prefix + bytes, no terminator.
    void writeString(std::string_view text)
    {
        write(static_cast<std::uint32_t>(text.size()));
        writeBytes(text.data(), text.size());
    }

    void writeBytes(const void* data, std::size_t size)
    {
        const std::size_t offset = m_data.size();
        m_data.resize(offset + size);
        std::memcpy(m_data.data() + offset, data, size);
    }

    [[nodiscard]] std::size_t size() const { return m_data.size(); }
    [[nodiscard]] const std::vector<std::byte>& data() const { return m_data; }

    // Patch a previously written value in place (e.g. a chunk size written
    // before the chunk body was known).
    template <typename T>
    void overwriteAt(std::size_t offset, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        SOL_ASSERT(offset + sizeof(T) <= m_data.size());
        std::memcpy(m_data.data() + offset, &value, sizeof(T));
    }

private:
    std::vector<std::byte> m_data;
};

// Reads from a borrowed buffer. Failure (reading past the end) is sticky:
// every subsequent read fails and outputs are left untouched, so a caller
// may batch reads and check failed() once.
class BinaryReader
{
public:
    explicit BinaryReader(std::span<const std::byte> data)
        : m_data(data)
    {
    }

    template <typename T>
    [[nodiscard]] bool read(T& out)
    {
        static_assert(std::is_trivially_copyable_v<T>, "serialize non-POD types field by field");
        return readBytes(&out, sizeof(T));
    }

    [[nodiscard]] bool readString(std::string& out)
    {
        std::uint32_t length = 0;
        if (!read(length) || length > remaining()) {
            m_failed = true;
            return false;
        }
        out.assign(reinterpret_cast<const char*>(m_data.data() + m_offset), length);
        m_offset += length;
        return true;
    }

    [[nodiscard]] bool readBytes(void* out, std::size_t size)
    {
        if (m_failed || size > remaining()) {
            m_failed = true;
            return false;
        }
        std::memcpy(out, m_data.data() + m_offset, size);
        m_offset += size;
        return true;
    }

    [[nodiscard]] bool skip(std::size_t size)
    {
        if (m_failed || size > remaining()) {
            m_failed = true;
            return false;
        }
        m_offset += size;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const { return m_data.size() - m_offset; }
    [[nodiscard]] bool failed() const { return m_failed; }

private:
    std::span<const std::byte> m_data;
    std::size_t m_offset = 0;
    bool m_failed = false;
};

} // namespace sol::core
