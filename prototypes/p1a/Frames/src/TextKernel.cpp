#include "Sol/Proto/Frames/TextKernel.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sol::proto::frames {
namespace {

[[nodiscard]] bool isDataMarker(const std::string& line, const char* marker)
{
    // Markers appear alone on a line, possibly with surrounding whitespace and a trailing
    // carriage return from the CRLF text the NAIF server serves.
    const std::size_t begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return false;
    }
    const std::size_t end = line.find_last_not_of(" \t\r\n");
    return line.compare(begin, end - begin + 1, marker) == 0;
}

} // namespace

double parseKernelNumber(const std::string& token)
{
    std::string normalized = token;
    for (char& c : normalized) {
        if (c == 'D' || c == 'd') {
            c = 'E';
        }
    }

    double value = 0.0;
    const char* first = normalized.data();
    const char* last = first + normalized.size();
    const auto [pointer, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || pointer != last) {
        throw std::runtime_error("TextKernel: '" + token + "' is not a number");
    }
    return value;
}

TextKernel TextKernel::loadFromFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("TextKernel: cannot open " + path.string());
    }

    TextKernel kernel;
    bool inData = false;
    std::string dataText;
    std::string line;
    while (std::getline(file, line)) {
        if (isDataMarker(line, "\\begindata")) {
            if (inData) {
                throw std::runtime_error("TextKernel: nested \\begindata in " + path.string());
            }
            inData = true;
            continue;
        }
        if (isDataMarker(line, "\\begintext")) {
            inData = false;
            continue;
        }
        if (inData) {
            dataText += line;
            dataText += '\n';
        }
    }

    // Tokenise the concatenated data blocks. Parentheses and equals signs are separate
    // tokens so an assignment can span lines, which every pinned kernel's tables do.
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };
    for (const char c : dataText) {
        if (c == '=') {
            // A '+' immediately preceding '=' is SPICE's append operator, not the sign of a
            // number. Recognising it here is what lets the assignment loop below reject it,
            // rather than silently reading an append as a plain assignment.
            if (current == "+") {
                current.clear();
                tokens.emplace_back("+=");
                continue;
            }
            flush();
            tokens.emplace_back(1, c);
        } else if (c == '(' || c == ')') {
            flush();
            tokens.emplace_back(1, c);
        } else if (c == ',' || std::isspace(static_cast<unsigned char>(c)) != 0) {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();

    for (std::size_t i = 0; i < tokens.size();) {
        const std::string& name = tokens[i];
        if (i + 1 < tokens.size() && tokens[i + 1] == "+=") {
            throw std::runtime_error("TextKernel: the '+=' append form is unsupported, used by '"
                                     + name + "' in " + path.string());
        }
        if (i + 1 >= tokens.size() || tokens[i + 1] != "=") {
            throw std::runtime_error("TextKernel: expected '=' after '" + name + "' in "
                                     + path.string());
        }

        std::vector<std::string> values;
        std::size_t cursor = i + 2;
        if (cursor < tokens.size() && tokens[cursor] == "(") {
            ++cursor;
            while (cursor < tokens.size() && tokens[cursor] != ")") {
                values.push_back(tokens[cursor]);
                ++cursor;
            }
            if (cursor >= tokens.size()) {
                throw std::runtime_error("TextKernel: unterminated '(' for '" + name + "' in "
                                         + path.string());
            }
            ++cursor; // consume ')'
        } else if (cursor < tokens.size()) {
            values.push_back(tokens[cursor]);
            ++cursor;
        } else {
            throw std::runtime_error("TextKernel: '" + name + "' has no value in " + path.string());
        }

        const auto [entry, inserted] = kernel.m_assignments.emplace(name, std::move(values));
        if (!inserted) {
            throw std::runtime_error("TextKernel: '" + name + "' is assigned twice in "
                                     + path.string()
                                     + "; A2 requires one unambiguous value per kernel");
        }
        i = cursor;
    }

    return kernel;
}

const std::vector<std::string>& TextKernel::tokens(const std::string& name) const
{
    const auto entry = m_assignments.find(name);
    if (entry == m_assignments.end()) {
        throw std::runtime_error("TextKernel: no assignment named '" + name + "'");
    }
    return entry->second;
}

bool TextKernel::has(const std::string& name) const
{
    return m_assignments.find(name) != m_assignments.end();
}

std::vector<double> TextKernel::numbers(const std::string& name) const
{
    const std::vector<std::string>& raw = tokens(name);
    std::vector<double> values;
    values.reserve(raw.size());
    for (const std::string& token : raw) {
        values.push_back(parseKernelNumber(token));
    }
    return values;
}

std::vector<double> TextKernel::numbers(const std::string& name, std::size_t expectedCount) const
{
    std::vector<double> values = numbers(name);
    if (values.size() != expectedCount) {
        std::ostringstream message;
        message << "TextKernel: '" << name << "' has " << values.size() << " values, expected "
                << expectedCount;
        throw std::runtime_error(message.str());
    }
    return values;
}

double TextKernel::number(const std::string& name) const
{
    return numbers(name, 1).front();
}

} // namespace sol::proto::frames
