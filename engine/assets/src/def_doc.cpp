#include "sol/assets/def_doc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sol::assets {
namespace {

[[nodiscard]] bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

[[nodiscard]] std::string_view trimLeft(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size() && isSpace(s[i])) {
        ++i;
    }
    return s.substr(i);
}

[[nodiscard]] std::string_view trimRight(std::string_view s)
{
    std::size_t n = s.size();
    while (n > 0 && isSpace(s[n - 1])) {
        --n;
    }
    return s.substr(0, n);
}

// A line carrying nothing but a comment, or nothing at all.
[[nodiscard]] bool isTriviaLine(std::string_view line)
{
    const std::string_view body = trimLeft(line);
    return body.empty() || body.front() == '#';
}

[[nodiscard]] bool isKeyStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Walks one line's worth of a value, tracking whether we are inside a string
// and how deep the brackets are, and reports where an unquoted `#` began.
//
// ⚑ The `#` test has to know about strings or `description = "a # b"` would be
// truncated at the hash. No committed def value contains one, which is exactly
// why the case has to be handled here rather than discovered later.
struct ScanState
{
    int depth = 0;
    bool inString = false;
    std::size_t commentAt = std::string_view::npos;
    bool commentInsideValue = false;
};

void scanValueLine(std::string_view line, ScanState& state)
{
    state.commentAt = std::string_view::npos;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (state.inString) {
            if (c == '\\') {
                ++i; // an escaped quote does not end the string
            } else if (c == '"') {
                state.inString = false;
            }
            continue;
        }
        if (c == '"') {
            state.inString = true;
        } else if (c == '[') {
            ++state.depth;
        } else if (c == ']') {
            --state.depth;
        } else if (c == '#') {
            // ⚑ Only a comment at bracket depth 0 ENDS the value. One inside a
            // still-open array runs to the end of its line and the array carries
            // on below it, so treating it as the end would truncate the value at
            // the hash - and recording it as the value's limit would then splice
            // an edit over the array's own tail.
            if (state.depth == 0) {
                state.commentAt = i;
            } else {
                state.commentInsideValue = true;
            }
            return; // either way, the rest of THIS line is a comment
        }
    }
}

// The separator a new row is written behind: whatever the source put there, or
// a single blank line for one the tool created.
//
// ⚑ Suppressed when nothing has been written yet, or a document whose first row
// carries no trivia would gain a blank line on its first line, every save.
[[nodiscard]] std::string separatorFor(const std::string& leading, const std::string& soFar)
{
    if (!leading.empty()) {
        return leading;
    }
    return soFar.empty() ? std::string{} : std::string("\n");
}

} // namespace

std::string_view DefKey::value() const
{
    if (valueEnd > text.size() || valueBegin > valueEnd) {
        return {};
    }
    return std::string_view(text).substr(valueBegin, valueEnd - valueBegin);
}

std::string_view DefKey::unquoted() const
{
    const std::string_view v = value();
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

void DefKey::setValue(std::string_view v)
{
    if (valueEnd > text.size() || valueBegin > valueEnd) {
        return;
    }
    text.replace(valueBegin, valueEnd - valueBegin, v);
    valueEnd = valueBegin + static_cast<std::uint32_t>(v.size());
}

const DefKey* DefRow::find(std::string_view name) const
{
    for (const DefKey& key : keys) {
        if (key.name == name) {
            return &key;
        }
    }
    return nullptr;
}

DefKey* DefRow::find(std::string_view name)
{
    return const_cast<DefKey*>(static_cast<const DefRow*>(this)->find(name));
}

std::string_view DefRow::id() const
{
    const DefKey* key = find("id");
    return key != nullptr ? key->unquoted() : std::string_view{};
}

void DefRow::set(std::string_view name, std::string_view value)
{
    if (DefKey* existing = find(name); existing != nullptr) {
        existing->setValue(value);
        return;
    }
    DefKey key;
    key.name = std::string(name);
    key.text = std::string(name) + " = " + std::string(value);
    key.valueBegin = static_cast<std::uint32_t>(name.size() + 3);
    key.valueEnd = static_cast<std::uint32_t>(key.text.size());
    keys.push_back(std::move(key));
}

void DefRow::remove(std::string_view name)
{
    const auto it = std::find_if(keys.begin(), keys.end(),
                                 [&](const DefKey& k) { return k.name == name; });
    if (it == keys.end()) {
        return;
    }
    // ⚑ The removed line's trivia moves DOWN to the key that inherits its
    // place, because a comment above a key is about the file at that point, not
    // about the key's identity. Dropping it would delete an author's note as a
    // side effect of clearing a value.
    if (!it->leading.empty()) {
        const auto next = it + 1;
        if (next != keys.end()) {
            next->leading = it->leading + next->leading;
        }
    }
    keys.erase(it);
}

const DefRow* DefDoc::find(std::string_view type, std::string_view id) const
{
    for (const DefRow& row : rows) {
        if (row.type == type && row.id() == id) {
            return &row;
        }
    }
    return nullptr;
}

DefRow* DefDoc::find(std::string_view type, std::string_view id)
{
    return const_cast<DefRow*>(static_cast<const DefDoc*>(this)->find(type, id));
}

std::size_t DefDoc::count(std::string_view type) const
{
    std::size_t n = 0;
    for (const DefRow& row : rows) {
        n += static_cast<std::size_t>(row.type == type);
    }
    return n;
}

DefRow& DefDoc::append(std::string_view type)
{
    DefRow row;
    row.type = std::string(type);
    row.header = "[[" + std::string(type) + "]]";
    // ⚑ The trailer becomes this row's leading rather than staying at the end
    // of the document. Trivia attaches downward, so a note the author left under
    // the last row would otherwise end up under the NEW one - which reads as if
    // it were about the row the tool just made.
    row.leading = trailer;
    trailer.clear();
    rows.push_back(std::move(row));
    return rows.back();
}

bool parseDefs(const char* text, std::size_t length, const char* sourceName, DefDoc& out,
               std::string* error)
{
    DefDoc doc;
    const auto fail = [&](const std::string& message, std::size_t line) {
        if (error != nullptr) {
            *error = std::string(sourceName != nullptr ? sourceName : "<defs>") + ":" +
                     std::to_string(line) + ": " + message;
        }
        return false;
    };

    std::string pending; // trivia waiting for whatever comes below it
    std::size_t cursor = 0;
    std::size_t lineNumber = 0;
    const std::string_view all(text, length);

    const auto nextLine = [&](std::string_view& line) {
        if (cursor >= all.size()) {
            return false;
        }
        const std::size_t end = all.find('\n', cursor);
        if (end == std::string_view::npos) {
            line = all.substr(cursor);
            cursor = all.size();
        } else {
            line = all.substr(cursor, end - cursor);
            cursor = end + 1;
        }
        ++lineNumber;
        return true;
    };

    std::string_view line;
    while (nextLine(line)) {
        if (isTriviaLine(line)) {
            pending += std::string(line);
            pending += '\n';
            continue;
        }
        const std::string_view body = trimLeft(line);
        if (body.size() >= 2 && body[0] == '[') {
            if (body.size() < 5 || body[1] != '[') {
                // A plain `[table]` header, which no def file in this game has
                // and which this model cannot place a key under. A hard refusal
                // rather than a flag: parsing it would silently reassign its
                // keys to the previous `[[table]]`.
                return fail("unsupported table header '" + std::string(trimRight(body)) + "'",
                            lineNumber);
            }
            const std::size_t close = body.find("]]");
            if (close == std::string_view::npos) {
                return fail("unterminated table header", lineNumber);
            }
            DefRow row;
            row.leading = std::move(pending);
            pending.clear();
            row.type = std::string(trimRight(trimLeft(body.substr(2, close - 2))));
            // ⚑ The header line is kept RAW, like a key's. Trimming it looks
            // harmless and eats the '\r' of a CRLF file, which turns every row
            // header in the document into a line ending the author did not write.
            row.header = std::string(line);
            if (row.type.empty()) {
                return fail("empty table header", lineNumber);
            }
            doc.rows.push_back(std::move(row));
            continue;
        }
        if (!isKeyStart(body.front())) {
            return fail("expected a comment, a [[table]] or 'key = value'", lineNumber);
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return fail("expected '=' in a key line", lineNumber);
        }
        if (doc.rows.empty()) {
            return fail("a key outside any [[table]]", lineNumber);
        }

        DefKey key;
        key.leading = std::move(pending);
        pending.clear();
        key.name = std::string(trimRight(trimLeft(line.substr(0, equals))));
        if (key.name.empty()) {
            return fail("empty key name", lineNumber);
        }

        // The value starts after the '=' and its spaces, and runs to the last
        // non-space byte before an unquoted '#'. A value whose brackets are
        // still open swallows following lines whole.
        std::string raw(line);
        std::size_t valueStart = equals + 1;
        while (valueStart < raw.size() && isSpace(raw[valueStart])) {
            ++valueStart;
        }
        ScanState state;
        scanValueLine(std::string_view(raw).substr(valueStart), state);
        std::size_t commentAt = state.commentAt == std::string_view::npos
                                    ? std::string_view::npos
                                    : valueStart + state.commentAt;
        while (state.depth > 0 || state.inString) {
            std::string_view continuation;
            if (!nextLine(continuation)) {
                return fail("unterminated value for key '" + key.name + "'", lineNumber);
            }
            const std::size_t base = raw.size() + 1;
            raw += '\n';
            raw += std::string(continuation);
            scanValueLine(continuation, state);
            commentAt = state.commentAt == std::string_view::npos ? std::string_view::npos
                                                                  : base + state.commentAt;
        }

        const std::size_t valueLimit = commentAt == std::string_view::npos ? raw.size() : commentAt;
        const std::string_view valueText =
            trimRight(std::string_view(raw).substr(valueStart, valueLimit - valueStart));
        if (valueText.empty()) {
            return fail("key '" + key.name + "' has no value", lineNumber);
        }
        doc.hasUnplaceableComments =
            doc.hasUnplaceableComments || state.commentInsideValue;
        // ⚑ The line is kept exactly as read, trailing spaces and a CR included.
        // Trimming it would be a change to a file the author did not ask for,
        // and under CRLF it would strip every line ending in the document.
        key.text = std::move(raw);
        key.valueBegin = static_cast<std::uint32_t>(valueStart);
        key.valueEnd = static_cast<std::uint32_t>(valueStart + valueText.size());
        doc.rows.back().keys.push_back(std::move(key));
    }

    doc.trailer = std::move(pending);
    // ⚑ A source whose last line has no newline must not gain one: the writer
    // ends every line, so the fact has to be carried rather than assumed.
    doc.endsWithNewline = length == 0 || text[length - 1] == '\n';
    if (!doc.endsWithNewline && !doc.trailer.empty()) {
        doc.trailer.pop_back(); // the '\n' the trivia loop supplied
    }
    out = std::move(doc);
    return true;
}

std::string writeDefs(const DefDoc& doc)
{
    std::string out;
    for (const DefRow& row : doc.rows) {
        out += separatorFor(row.leading, out);
        out += row.header;
        out += '\n';
        for (const DefKey& key : row.keys) {
            out += key.leading;
            out += key.text;
            out += '\n';
        }
    }
    out += doc.trailer;
    if (!doc.endsWithNewline && !out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

std::string defNumber(float value)
{
    const auto wide = static_cast<double>(value);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", wide);
    for (int precision = 1; precision <= 9; ++precision) {
        char candidate[64];
        std::snprintf(candidate, sizeof(candidate), "%.*g", precision, wide);
        if (std::strpbrk(candidate, "eE") != nullptr) {
            continue; // a plain form is what a person types; keep looking
        }
        if (static_cast<float>(std::strtod(candidate, nullptr)) == value) {
            std::memcpy(buffer, candidate, sizeof(buffer));
            break;
        }
    }
    std::string text(buffer);
    // TOML tells an integer from a float by the dot, and every value formatted
    // here is a float to the schema that will read it back.
    if (text.find_first_of(".eEni") == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string defNumber(float value, int decimals)
{
    double scale = 1.0;
    for (int i = 0; i < decimals; ++i) {
        scale *= 10.0;
    }
    const double rounded = std::round(static_cast<double>(value) * scale) / scale;
    return defNumber(static_cast<float>(rounded));
}

std::string defString(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out += '"';
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

std::string defBool(bool value)
{
    return value ? "true" : "false";
}

} // namespace sol::assets
