#include "Sol/Proto/Harness/JsonWriter.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace sol::proto {
namespace {

constexpr std::string_view kIndentUnit = "  ";

/// Formats @p value as shortest round-trip decimal.
///
/// std::to_chars with no precision argument is the only formatting path here that is
/// guaranteed lossless and locale-independent; printf-family formatting is neither.
std::string formatDouble(double value) {
    if (std::isnan(value)) {
        return "\"nan\"";
    }
    if (std::isinf(value)) {
        return value > 0.0 ? "\"inf\"" : "\"-inf\"";
    }

    std::array<char, 64> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
        throw std::logic_error("JsonWriter: std::to_chars failed to format a finite double");
    }

    std::string text(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));

    // to_chars emits integral values without a fractional part ("1" rather than "1.0").
    // Keep them numerically identical but visibly floating point, so a reader never
    // mistakes a measured quantity for a count.
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

/// Renders the IEEE-754 bit pattern most-significant nibble first.
std::string formatBits(double value) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    const auto bits = std::bit_cast<std::uint64_t>(value);
    std::string text(16, '0');
    for (std::size_t index = 0; index < 16; ++index) {
        const auto shift = static_cast<unsigned>(60 - 4 * index);
        text[index] = kDigits[static_cast<std::size_t>((bits >> shift) & 0xFu)];
    }
    return text;
}

} // namespace

JsonWriter::JsonWriter(std::ostream& out) : m_out(out) {}

JsonWriter::~JsonWriter() noexcept(false) {
    if (!m_scopes.empty()) {
        // Only complain when we are not already unwinding; throwing during unwinding
        // would terminate and destroy the more informative original exception.
        if (std::uncaught_exceptions() == 0) {
            throw std::logic_error("JsonWriter: document destroyed with unclosed scopes");
        }
    }
}

void JsonWriter::writeIndent() {
    for (std::size_t depth = 0; depth < m_scopes.size(); ++depth) {
        m_out << kIndentUnit;
    }
}

void JsonWriter::writeSeparator() {
    if (!m_scopes.empty()) {
        if (m_scopeHasEntry.back()) {
            m_out << ",\n";
        } else {
            m_out << "\n";
            m_scopeHasEntry.back() = true;
        }
    }
    writeIndent();
}

void JsonWriter::requireScope(Scope scope, const char* operation) const {
    if (m_scopes.empty() || m_scopes.back() != scope) {
        throw std::logic_error(std::string("JsonWriter: ") + operation
                               + " used outside its required scope");
    }
}

void JsonWriter::writeEscaped(std::string_view value) {
    m_out << '"';
    for (const char character : value) {
        switch (character) {
        case '"':  m_out << "\\\""; break;
        case '\\': m_out << "\\\\"; break;
        case '\n': m_out << "\\n"; break;
        case '\r': m_out << "\\r"; break;
        case '\t': m_out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20u) {
                std::array<char, 8> escape{};
                std::snprintf(escape.data(), escape.size(), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(character)));
                m_out << escape.data();
            } else {
                m_out << character;
            }
            break;
        }
    }
    m_out << '"';
}

void JsonWriter::writeKey(std::string_view key) {
    requireScope(Scope::Object, "a keyed write");
    writeSeparator();
    writeEscaped(key);
    m_out << ": ";
}

void JsonWriter::beginObject() {
    if (m_scopes.empty()) {
        m_out << "{";
    } else {
        requireScope(Scope::Array, "beginObject() without a key");
        writeSeparator();
        m_out << "{";
    }
    m_scopes.push_back(Scope::Object);
    m_scopeHasEntry.push_back(false);
}

void JsonWriter::beginObject(std::string_view key) {
    writeKey(key);
    m_out << "{";
    m_scopes.push_back(Scope::Object);
    m_scopeHasEntry.push_back(false);
}

void JsonWriter::endObject() {
    requireScope(Scope::Object, "endObject()");
    const bool hadEntry = m_scopeHasEntry.back();
    m_scopes.pop_back();
    m_scopeHasEntry.pop_back();
    if (hadEntry) {
        m_out << "\n";
        writeIndent();
    }
    m_out << "}";
    if (m_scopes.empty()) {
        m_out << "\n";
    }
}

void JsonWriter::beginArray(std::string_view key) {
    writeKey(key);
    m_out << "[";
    m_scopes.push_back(Scope::Array);
    m_scopeHasEntry.push_back(false);
}

void JsonWriter::endArray() {
    requireScope(Scope::Array, "endArray()");
    const bool hadEntry = m_scopeHasEntry.back();
    m_scopes.pop_back();
    m_scopeHasEntry.pop_back();
    if (hadEntry) {
        m_out << "\n";
        writeIndent();
    }
    m_out << "]";
}

void JsonWriter::write(std::string_view key, std::string_view value) {
    writeKey(key);
    writeEscaped(value);
}

void JsonWriter::write(std::string_view key, const char* value) {
    write(key, value == nullptr ? std::string_view{} : std::string_view(value));
}

void JsonWriter::write(std::string_view key, bool value) {
    writeKey(key);
    m_out << (value ? "true" : "false");
}

void JsonWriter::write(std::string_view key, std::int64_t value) {
    writeKey(key);
    m_out << value;
}

void JsonWriter::write(std::string_view key, std::uint64_t value) {
    writeKey(key);
    m_out << value;
}

void JsonWriter::write(std::string_view key, double value) {
    writeKey(key);
    m_out << formatDouble(value);
}

void JsonWriter::writeBits(std::string_view key, double value) {
    writeKey(key);
    writeEscaped(formatBits(value));
}

void JsonWriter::writeValue(std::string_view value) {
    requireScope(Scope::Array, "writeValue()");
    writeSeparator();
    writeEscaped(value);
}

void JsonWriter::writeValue(const char* value) {
    writeValue(value == nullptr ? std::string_view{} : std::string_view(value));
}

void JsonWriter::writeValue(std::uint64_t value) {
    requireScope(Scope::Array, "writeValue()");
    writeSeparator();
    m_out << value;
}

void JsonWriter::writeValue(double value) {
    requireScope(Scope::Array, "writeValue()");
    writeSeparator();
    m_out << formatDouble(value);
}

} // namespace sol::proto
