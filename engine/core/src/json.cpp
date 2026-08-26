#include "sol/core/json.hpp"

#include "sol/core/assert.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sol::core {

namespace {

constexpr int kMaxDepth = 128;

void appendUtf8(std::string& out, unsigned codepoint)
{
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

} // namespace

class JsonParser
{
public:
    JsonParser(const char* text, std::size_t length) : m_cursor(text), m_end(text + length) {}

    bool parseDocument(JsonValue& out, std::string* outError)
    {
        skipWhitespace();
        if (!parseValue(out, 0)) {
            if (outError != nullptr) {
                *outError = m_error;
            }
            return false;
        }
        skipWhitespace();
        if (m_cursor != m_end) {
            fail("trailing characters after document");
            if (outError != nullptr) {
                *outError = m_error;
            }
            return false;
        }
        return true;
    }

private:
    void fail(const char* message)
    {
        if (m_error.empty()) {
            char buffer[128];
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%s (offset %zu)",
                          message,
                          static_cast<std::size_t>(m_cursor - m_start));
            m_error = buffer;
        }
    }

    void skipWhitespace()
    {
        while (m_cursor != m_end &&
               (*m_cursor == ' ' || *m_cursor == '\t' || *m_cursor == '\n' || *m_cursor == '\r')) {
            ++m_cursor;
        }
    }

    bool consume(char expected)
    {
        if (m_cursor != m_end && *m_cursor == expected) {
            ++m_cursor;
            return true;
        }
        return false;
    }

    bool consumeLiteral(const char* literal)
    {
        const std::size_t length = std::strlen(literal);
        if (static_cast<std::size_t>(m_end - m_cursor) >= length &&
            std::memcmp(m_cursor, literal, length) == 0) {
            m_cursor += length;
            return true;
        }
        return false;
    }

    bool parseValue(JsonValue& out, int depth)
    {
        if (depth > kMaxDepth) {
            fail("nesting too deep");
            return false;
        }
        if (m_cursor == m_end) {
            fail("unexpected end of input");
            return false;
        }

        switch (*m_cursor) {
        case '{':
            return parseObject(out, depth);
        case '[':
            return parseArray(out, depth);
        case '"':
            out.m_type = JsonType::String;
            return parseString(out.m_string);
        case 't':
            if (consumeLiteral("true")) {
                out.m_type = JsonType::Bool;
                out.m_bool = true;
                return true;
            }
            fail("invalid literal");
            return false;
        case 'f':
            if (consumeLiteral("false")) {
                out.m_type = JsonType::Bool;
                out.m_bool = false;
                return true;
            }
            fail("invalid literal");
            return false;
        case 'n':
            if (consumeLiteral("null")) {
                out.m_type = JsonType::Null;
                return true;
            }
            fail("invalid literal");
            return false;
        default:
            return parseNumber(out);
        }
    }

    bool parseNumber(JsonValue& out)
    {
        char* numberEnd = nullptr;
        const double value = std::strtod(m_cursor, &numberEnd);
        if (numberEnd == m_cursor || numberEnd > m_end) {
            fail("invalid number");
            return false;
        }
        m_cursor = numberEnd;
        out.m_type = JsonType::Number;
        out.m_number = value;
        return true;
    }

    bool parseHex4(unsigned& out)
    {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (m_cursor == m_end) {
                fail("truncated \\u escape");
                return false;
            }
            const char c = *m_cursor++;
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail("invalid \\u escape");
                return false;
            }
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        if (!consume('"')) {
            fail("expected string");
            return false;
        }
        out.clear();
        while (true) {
            if (m_cursor == m_end) {
                fail("unterminated string");
                return false;
            }
            const char c = *m_cursor++;
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_cursor == m_end) {
                fail("unterminated escape");
                return false;
            }
            const char escape = *m_cursor++;
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned codepoint = 0;
                if (!parseHex4(codepoint)) {
                    return false;
                }
                // Surrogate pair
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                    if (consumeLiteral("\\u")) {
                        unsigned low = 0;
                        if (!parseHex4(low)) {
                            return false;
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            fail("invalid low surrogate");
                            return false;
                        }
                    } else {
                        fail("unpaired surrogate");
                        return false;
                    }
                }
                appendUtf8(out, codepoint);
                break;
            }
            default:
                fail("invalid escape");
                return false;
            }
        }
    }

    bool parseArray(JsonValue& out, int depth)
    {
        consume('[');
        out.m_type = JsonType::Array;
        skipWhitespace();
        if (consume(']')) {
            return true;
        }
        while (true) {
            JsonValue element;
            skipWhitespace();
            if (!parseValue(element, depth + 1)) {
                return false;
            }
            out.m_array.push_back(std::move(element));
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                fail("expected ',' or ']'");
                return false;
            }
        }
    }

    bool parseObject(JsonValue& out, int depth)
    {
        consume('{');
        out.m_type = JsonType::Object;
        skipWhitespace();
        if (consume('}')) {
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')) {
                fail("expected ':'");
                return false;
            }
            skipWhitespace();
            JsonValue value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            out.m_object.emplace_back(std::move(key), std::move(value));
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                fail("expected ',' or '}'");
                return false;
            }
        }
    }

    const char* m_start = nullptr;
    const char* m_cursor = nullptr;
    const char* m_end = nullptr;
    std::string m_error;

public:
    void setStart(const char* start) { m_start = start; }
};

bool JsonValue::parse(const char* text, std::size_t length, JsonValue& out, std::string* outError)
{
    JsonParser parser(text, length);
    parser.setStart(text);
    out = JsonValue{};
    return parser.parseDocument(out, outError);
}

std::size_t JsonValue::size() const
{
    if (m_type == JsonType::Array) {
        return m_array.size();
    }
    if (m_type == JsonType::Object) {
        return m_object.size();
    }
    return 0;
}

const JsonValue& JsonValue::operator[](std::size_t index) const
{
    SOL_ASSERT(m_type == JsonType::Array && index < m_array.size());
    return m_array[index];
}

const JsonValue* JsonValue::find(const char* key) const
{
    if (m_type != JsonType::Object) {
        return nullptr;
    }
    for (const auto& [memberKey, memberValue] : m_object) {
        if (memberKey == key) {
            return &memberValue;
        }
    }
    return nullptr;
}

} // namespace sol::core
