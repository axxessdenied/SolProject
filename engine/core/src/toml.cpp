#include "sol/core/toml.hpp"

#include "sol/core/assert.hpp"

#include <charconv>
#include <cstring>
#include <limits>

namespace sol::core {

namespace {

[[nodiscard]] bool isBareKeyChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

[[nodiscard]] bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] int hexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void appendUtf8(std::string& out, std::uint32_t codepoint)
{
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
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

class TomlParser
{
public:
    TomlParser(const char* text, std::size_t length)
        : m_cursor(text)
        , m_end(text + length)
    {
    }

    std::string error;

    [[nodiscard]] bool parseDocument(TomlValue& root)
    {
        root = TomlValue{};
        m_current = &root;
        for (;;) {
            skipBlank();
            if (atEnd()) {
                return true;
            }
            if (peek() == '[') {
                if (!parseHeader(root)) {
                    return false;
                }
            } else {
                if (!parseKeyValueInto(*m_current) || !expectLineEnd()) {
                    return false;
                }
            }
        }
    }

private:
    // ---- character stream -------------------------------------------------

    [[nodiscard]] bool atEnd() const { return m_cursor >= m_end; }
    [[nodiscard]] char peek() const { return atEnd() ? '\0' : *m_cursor; }
    [[nodiscard]] char peekAt(std::size_t offset) const
    {
        return m_cursor + offset >= m_end ? '\0' : m_cursor[offset];
    }

    void advance()
    {
        if (*m_cursor == '\n') {
            ++m_line;
        }
        ++m_cursor;
    }

    [[nodiscard]] bool fail(const char* message)
    {
        error = "line " + std::to_string(m_line) + ": " + message;
        return false;
    }

    void skipInline()
    {
        while (!atEnd() && (peek() == ' ' || peek() == '\t')) {
            advance();
        }
    }

    void skipComment()
    {
        if (peek() == '#') {
            while (!atEnd() && peek() != '\n') {
                advance();
            }
        }
    }

    // Whitespace, newlines, and comments.
    void skipBlank()
    {
        for (;;) {
            skipInline();
            skipComment();
            if (!atEnd() && (peek() == '\n' || peek() == '\r')) {
                advance();
                continue;
            }
            return;
        }
    }

    [[nodiscard]] bool expectLineEnd()
    {
        skipInline();
        skipComment();
        if (atEnd() || peek() == '\n') {
            return true;
        }
        if (peek() == '\r' && peekAt(1) == '\n') {
            return true;
        }
        return fail("unexpected characters after value");
    }

    // ---- keys -------------------------------------------------------------

    [[nodiscard]] bool parseKeySegment(std::string& out)
    {
        out.clear();
        if (peek() == '"') {
            return parseBasicString(out);
        }
        if (peek() == '\'') {
            return parseLiteralString(out);
        }
        while (isBareKeyChar(peek())) {
            out.push_back(peek());
            advance();
        }
        if (out.empty()) {
            return fail("expected a key");
        }
        return true;
    }

    [[nodiscard]] bool parseKeyPath(std::vector<std::string>& segments)
    {
        segments.clear();
        for (;;) {
            skipInline();
            std::string segment;
            if (!parseKeySegment(segment)) {
                return false;
            }
            segments.push_back(std::move(segment));
            skipInline();
            if (peek() != '.') {
                return true;
            }
            advance();
        }
    }

    // ---- table navigation -------------------------------------------------

    [[nodiscard]] static TomlValue* findMember(TomlValue& table, const std::string& key)
    {
        for (auto& [name, value] : table.m_table) {
            if (name == key) {
                return &value;
            }
        }
        return nullptr;
    }

    // Header-style descent: [a.b] where a is an array of tables means the
    // last element of a.
    [[nodiscard]] bool descend(TomlValue*& table, const std::string& key)
    {
        TomlValue* member = findMember(*table, key);
        if (member == nullptr) {
            table->m_table.emplace_back(key, TomlValue{});
            table = &table->m_table.back().second;
            return true;
        }
        if (member->m_type == TomlType::Table) {
            table = member;
            return true;
        }
        if (member->m_type == TomlType::Array && member->m_tableArray) {
            table = &member->m_array.back();
            return true;
        }
        return fail("key already used for a non-table value");
    }

    [[nodiscard]] bool parseHeader(TomlValue& root)
    {
        advance(); // '['
        const bool isArrayOfTables = peek() == '[';
        if (isArrayOfTables) {
            advance();
        }

        std::vector<std::string> segments;
        if (!parseKeyPath(segments)) {
            return false;
        }
        if (peek() != ']') {
            return fail("expected ']' to close a table header");
        }
        advance();
        if (isArrayOfTables) {
            if (peek() != ']') {
                return fail("expected ']]' to close an array-of-tables header");
            }
            advance();
        }
        if (!expectLineEnd()) {
            return false;
        }

        TomlValue* table = &root;
        for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
            if (!descend(table, segments[i])) {
                return false;
            }
        }

        const std::string& last = segments.back();
        TomlValue* member = findMember(*table, last);
        if (isArrayOfTables) {
            if (member == nullptr) {
                table->m_table.emplace_back(last, TomlValue{});
                member = &table->m_table.back().second;
                member->m_type = TomlType::Array;
                member->m_tableArray = true;
            } else if (member->m_type != TomlType::Array || !member->m_tableArray) {
                return fail("key already used for a non-array-of-tables value");
            }
            member->m_array.emplace_back();
            m_current = &member->m_array.back();
            return true;
        }
        if (member == nullptr) {
            table->m_table.emplace_back(last, TomlValue{});
            m_current = &table->m_table.back().second;
            return true;
        }
        if (member->m_type == TomlType::Table) {
            m_current = member; // lenient: reopening a table is allowed
            return true;
        }
        return fail("table header conflicts with an existing value");
    }

    // ---- key/value pairs --------------------------------------------------

    [[nodiscard]] bool parseKeyValueInto(TomlValue& table)
    {
        std::vector<std::string> segments;
        if (!parseKeyPath(segments)) {
            return false;
        }
        if (peek() != '=') {
            return fail("expected '=' after key");
        }
        advance();
        skipInline();

        TomlValue value;
        if (!parseValue(value)) {
            return false;
        }

        TomlValue* target = &table;
        for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
            TomlValue* member = findMember(*target, segments[i]);
            if (member == nullptr) {
                target->m_table.emplace_back(segments[i], TomlValue{});
                target = &target->m_table.back().second;
            } else if (member->m_type == TomlType::Table) {
                target = member;
            } else {
                return fail("dotted key crosses a non-table value");
            }
        }
        if (findMember(*target, segments.back()) != nullptr) {
            return fail("duplicate key");
        }
        target->m_table.emplace_back(segments.back(), std::move(value));
        return true;
    }

    // ---- values -----------------------------------------------------------

    [[nodiscard]] bool parseValue(TomlValue& out)
    {
        switch (peek()) {
        case '"':
            out.m_type = TomlType::String;
            return parseBasicString(out.m_string);
        case '\'':
            out.m_type = TomlType::String;
            return parseLiteralString(out.m_string);
        case '[':
            return parseArray(out);
        case '{':
            return parseInlineTable(out);
        default:
            break;
        }
        if (matchKeyword("true")) {
            out.m_type = TomlType::Bool;
            out.m_bool = true;
            return true;
        }
        if (matchKeyword("false")) {
            out.m_type = TomlType::Bool;
            out.m_bool = false;
            return true;
        }
        return parseNumber(out);
    }

    [[nodiscard]] bool matchKeyword(const char* word)
    {
        const std::size_t length = std::strlen(word);
        if (static_cast<std::size_t>(m_end - m_cursor) < length ||
            std::memcmp(m_cursor, word, length) != 0) {
            return false;
        }
        if (isBareKeyChar(peekAt(length))) {
            return false; // prefix of a longer token
        }
        for (std::size_t i = 0; i < length; ++i) {
            advance();
        }
        return true;
    }

    [[nodiscard]] bool parseBasicString(std::string& out)
    {
        advance(); // opening '"'
        if (peek() == '"' && peekAt(1) == '"') {
            return fail("multi-line strings are not supported");
        }
        out.clear();
        for (;;) {
            if (atEnd() || peek() == '\n' || peek() == '\r') {
                return fail("unterminated string");
            }
            const char c = peek();
            advance();
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            const char escape = peek();
            advance();
            switch (escape) {
            case 'b': out.push_back('\b'); break;
            case 't': out.push_back('\t'); break;
            case 'n': out.push_back('\n'); break;
            case 'f': out.push_back('\f'); break;
            case 'r': out.push_back('\r'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'u':
            case 'U': {
                const int digits = escape == 'u' ? 4 : 8;
                std::uint32_t codepoint = 0;
                for (int i = 0; i < digits; ++i) {
                    const int digit = hexDigit(peek());
                    if (digit < 0) {
                        return fail("invalid unicode escape");
                    }
                    codepoint = (codepoint << 4) | static_cast<std::uint32_t>(digit);
                    advance();
                }
                if (codepoint > 0x10'FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
                    return fail("invalid unicode codepoint");
                }
                appendUtf8(out, codepoint);
                break;
            }
            default:
                return fail("unknown string escape");
            }
        }
    }

    [[nodiscard]] bool parseLiteralString(std::string& out)
    {
        advance(); // opening '\''
        if (peek() == '\'' && peekAt(1) == '\'') {
            return fail("multi-line strings are not supported");
        }
        out.clear();
        for (;;) {
            if (atEnd() || peek() == '\n' || peek() == '\r') {
                return fail("unterminated string");
            }
            const char c = peek();
            advance();
            if (c == '\'') {
                return true;
            }
            out.push_back(c);
        }
    }

    [[nodiscard]] bool parseArray(TomlValue& out)
    {
        advance(); // '['
        out.m_type = TomlType::Array;
        for (;;) {
            skipBlank(); // arrays may span lines and contain comments
            if (atEnd()) {
                return fail("unterminated array");
            }
            if (peek() == ']') {
                advance();
                return true;
            }
            TomlValue element;
            if (!parseValue(element)) {
                return false;
            }
            out.m_array.push_back(std::move(element));
            skipBlank();
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == ']') {
                advance();
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    [[nodiscard]] bool parseInlineTable(TomlValue& out)
    {
        advance(); // '{'
        out.m_type = TomlType::Table;
        skipInline();
        if (peek() == '}') {
            advance();
            return true;
        }
        for (;;) {
            skipInline();
            if (!parseKeyValueInto(out)) {
                return false;
            }
            skipInline();
            if (peek() == ',') {
                advance();
                skipInline();
                if (peek() == '}') { // lenient: trailing comma
                    advance();
                    return true;
                }
                continue;
            }
            if (peek() == '}') {
                advance();
                return true;
            }
            return fail("expected ',' or '}' in inline table");
        }
    }

    [[nodiscard]] bool parseNumber(TomlValue& out)
    {
        std::string token;
        while (!atEnd()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ']' ||
                c == '}' || c == '#') {
                break;
            }
            token.push_back(c);
            advance();
        }
        if (token.empty()) {
            return fail("expected a value");
        }

        std::string body = token;
        bool negative = false;
        if (body[0] == '+' || body[0] == '-') {
            negative = body[0] == '-';
            body.erase(0, 1);
        }

        if (body == "inf") {
            out.m_type = TomlType::Float;
            out.m_float = negative ? -std::numeric_limits<double>::infinity()
                                   : std::numeric_limits<double>::infinity();
            return true;
        }
        if (body == "nan") {
            out.m_type = TomlType::Float;
            out.m_float = std::numeric_limits<double>::quiet_NaN();
            return true;
        }

        // A ':' anywhere (times) or a dddd- prefix (dates) - careful not to
        // trip on the '-' of a negative float exponent like 2.5e-3.
        if (body.find(':') != std::string::npos ||
            (body.size() > 4 && isDigit(body[0]) && isDigit(body[1]) && isDigit(body[2]) &&
             isDigit(body[3]) && body[4] == '-')) {
            return fail("dates/times are not supported");
        }

        if (!stripUnderscores(body)) {
            return fail("misplaced underscore in number");
        }
        if (body.empty()) {
            return fail("malformed number");
        }

        int base = 10;
        std::string digits = body;
        if (body.size() > 2 && body[0] == '0') {
            if (body[1] == 'x') {
                base = 16;
            } else if (body[1] == 'o') {
                base = 8;
            } else if (body[1] == 'b') {
                base = 2;
            }
            if (base != 10) {
                if (negative) {
                    return fail("non-decimal integers cannot be signed");
                }
                digits = body.substr(2);
            }
        }
        // 'e'/'E' only means an exponent in decimal; hex digits contain them.
        const bool isFloat = base == 10 && (body.find('.') != std::string::npos ||
                                            body.find('e') != std::string::npos ||
                                            body.find('E') != std::string::npos);

        if (!isFloat) {
            std::uint64_t magnitude = 0;
            const auto [ptr, ec] =
                std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, base);
            if (ec == std::errc{} && ptr == digits.data() + digits.size()) {
                const std::uint64_t limit =
                    negative ? 0x8000'0000'0000'0000ull
                             : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                if (magnitude > limit) {
                    return fail("integer out of range");
                }
                out.m_type = TomlType::Integer;
                out.m_integer = negative ? -static_cast<std::int64_t>(magnitude - 1) - 1
                                         : static_cast<std::int64_t>(magnitude);
                return true;
            }
            if (ec == std::errc::result_out_of_range) {
                return fail("integer out of range");
            }
            if (base != 10) {
                return fail("malformed number");
            }
        }

        double value = 0.0;
        const auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), value);
        if (ec != std::errc{} || ptr != body.data() + body.size()) {
            return fail("malformed number");
        }
        out.m_type = TomlType::Float;
        out.m_float = negative ? -value : value;
        return true;
    }

    // TOML underscores separate digits: reject leading/trailing/doubled.
    [[nodiscard]] static bool stripUnderscores(std::string& body)
    {
        if (body.find('_') == std::string::npos) {
            return true;
        }
        if (body.front() == '_' || body.back() == '_' ||
            body.find("__") != std::string::npos) {
            return false;
        }
        std::string stripped;
        stripped.reserve(body.size());
        for (const char c : body) {
            if (c != '_') {
                stripped.push_back(c);
            }
        }
        body = std::move(stripped);
        return true;
    }

    const char* m_cursor;
    const char* m_end;
    int m_line = 1;
    TomlValue* m_current = nullptr;
};

bool TomlValue::parse(const char* text, std::size_t length, TomlValue& out, std::string* outError)
{
    TomlParser parser(text, length);
    if (parser.parseDocument(out)) {
        return true;
    }
    if (outError != nullptr) {
        *outError = parser.error;
    }
    out = TomlValue{};
    return false;
}

std::size_t TomlValue::size() const
{
    if (m_type == TomlType::Array) {
        return m_array.size();
    }
    if (m_type == TomlType::Table) {
        return m_table.size();
    }
    return 0;
}

const TomlValue& TomlValue::operator[](std::size_t index) const
{
    SOL_ASSERT(m_type == TomlType::Array && index < m_array.size());
    return m_array[index];
}

const TomlValue* TomlValue::find(const char* key) const
{
    if (m_type != TomlType::Table) {
        return nullptr;
    }
    for (const auto& [name, value] : m_table) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

const TomlValue* TomlValue::findPath(const char* path) const
{
    const TomlValue* current = this;
    std::string segment;
    for (const char* c = path;; ++c) {
        if (*c == '.' || *c == '\0') {
            current = current->find(segment.c_str());
            if (current == nullptr || *c == '\0') {
                return current;
            }
            segment.clear();
        } else {
            segment.push_back(*c);
        }
    }
}

} // namespace sol::core
