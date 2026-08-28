#pragma once

// Comment and blank-line preservation for the authored TOML documents, shared
// by `texture_doc.cpp` and `sound_doc.cpp` (engine plan Phase 26 stage A).
//
// ⚑⚑ FACTORED AT THE THIRD INSTANCE AND NOT BEFORE, WHICH IS THE POINT. This
// scanner was written for `.forge` (Phase 9 stage E), copied for `.tex` (stage
// G), and `.snd` would have been the third identical copy of ninety lines whose
// only difference is the element header it looks for. Two copies of a rule is a
// coincidence; three is a rule with nowhere to live, and the failure mode is
// specific rather than aesthetic - a fix to the quote handling below would have
// to be found and made three times, and the first one missed is a document kind
// that silently reformats a committed file.
//
// ⚑ `forge_doc.cpp` KEEPS ITS OWN, DELIBERATELY, AND THAT IS NOT AN OVERSIGHT.
// It tracks a second leading run for its `[build]` table as well as one per
// `[[part]]`, so folding it in means generalising this to N element headers - a
// change worth making on its own evidence rather than riding along inside a
// sound phase, where nothing would exercise it.
//
// The model itself, unchanged from where it was proved: a run of comments and
// blank lines attaches VERBATIM to whatever stands below it, so a tool that
// saves on every edit gives a committed file back the way it found it.

#include <string>
#include <string_view>
#include <vector>

namespace sol::assets::doc {

struct SourceTrivia
{
    std::string header;
    // One entry per element header line, in file order.
    std::vector<std::string> elementLeading;
    std::string trailer;
    // A comment this model cannot place: after a value on the same line, or
    // inside a multi-line array. The caller says so rather than dropping it.
    bool unplaceable = false;
};

[[nodiscard]] inline bool isTriviaLine(std::string_view line)
{
    for (const char c : line) {
        if (c == ' ' || c == '\t' || c == '\r') {
            continue;
        }
        return c == '#';
    }
    return true; // blank
}

[[nodiscard]] inline std::string_view trimLeft(std::string_view line)
{
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return line.substr(i);
}

// ⚑ Quoted spans are skipped so a `#` or `[` inside a string reads as text
// rather than as syntax, and the bracket depth is what keeps a comment sitting
// between two rows of a multi-line array attached to that value instead of to
// the next element.
inline void scanContentLine(std::string_view line, int& depth, bool& sawComment)
{
    char quote = '\0';
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != '\0') {
            if (c == '\\' && quote == '"') {
                ++i;
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            sawComment = true;
            return;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
        }
    }
}

// `elementHeader` is the literal that opens a repeated element - `"[[op]]"` for
// both document kinds using this today.
[[nodiscard]] inline SourceTrivia
scanTrivia(const char* text, std::size_t length, std::string_view elementHeader)
{
    SourceTrivia trivia;
    std::string pending;
    bool sawElement = false;
    int depth = 0;

    std::size_t cursor = 0;
    while (cursor < length) {
        std::size_t end = cursor;
        while (end < length && text[end] != '\n') {
            ++end;
        }
        const std::string_view line(text + cursor, end - cursor);
        const std::size_t next = (end < length) ? end + 1 : end;

        if (depth == 0 && isTriviaLine(line)) {
            pending.append(text + cursor, next - cursor);
            if (next == end) {
                pending += '\n';
            }
            cursor = next;
            continue;
        }

        if (depth == 0) {
            const std::string_view body = trimLeft(line);
            if (body.starts_with(elementHeader)) {
                trivia.elementLeading.push_back(std::move(pending));
            } else if (!sawElement) {
                trivia.header = std::move(pending);
            } else if (!pending.empty()) {
                trivia.unplaceable = true;
            }
            sawElement = true;
            pending.clear();
        }

        bool sawComment = false;
        scanContentLine(line, depth, sawComment);
        if (sawComment) {
            trivia.unplaceable = true;
        }
        cursor = next;
    }

    trivia.trailer = std::move(pending);
    return trivia;
}

// ⚑ A blank line between elements only where the author had no comment there.
// A file that opens straight into its first element must save that way, so the
// separator is suppressed while nothing has been written yet.
[[nodiscard]] inline std::string separatorFor(const std::string& leading, const std::string& soFar)
{
    if (!leading.empty()) {
        return leading;
    }
    return soFar.empty() ? std::string() : std::string("\n");
}

} // namespace sol::assets::doc
