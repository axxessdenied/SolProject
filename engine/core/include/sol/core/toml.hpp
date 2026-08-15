#pragma once

// TOML parser for game data definitions (ships, weapons, factions - engine
// plan Phase 5 content pipeline). Supports the subset those files need:
//   - comments, bare/quoted/dotted keys, [table] and [[array-of-table]]
//     headers, inline tables, arrays (multi-line, heterogeneous)
//   - strings (basic with escapes incl. \uXXXX, literal), integers (decimal/
//     hex/octal/binary with underscores), floats (incl. inf/nan), booleans
// Deliberately unsupported (parse error): dates/times, multi-line strings.
// Lenient where it helps data authoring: duplicate [table] headers reopen
// the table; trailing commas are accepted in arrays and inline tables.
// Duplicate key assignment is always an error.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sol::core {

enum class TomlType
{
    Bool,
    Integer,
    Float,
    String,
    Array,
    Table,
};

// Immutable-after-parse TOML DOM; the parse root is a Table.
class TomlValue
{
public:
    // Parses a complete document; on failure returns false and sets outError
    // to "line N: message".
    [[nodiscard]] static bool parse(const char* text, std::size_t length, TomlValue& out,
                                    std::string* outError = nullptr);

    [[nodiscard]] TomlType type() const { return m_type; }
    [[nodiscard]] bool isBool() const { return m_type == TomlType::Bool; }
    [[nodiscard]] bool isInteger() const { return m_type == TomlType::Integer; }
    [[nodiscard]] bool isFloat() const { return m_type == TomlType::Float; }
    [[nodiscard]] bool isString() const { return m_type == TomlType::String; }
    [[nodiscard]] bool isArray() const { return m_type == TomlType::Array; }
    [[nodiscard]] bool isTable() const { return m_type == TomlType::Table; }

    [[nodiscard]] bool asBool(bool fallback = false) const
    {
        return isBool() ? m_bool : fallback;
    }
    [[nodiscard]] std::int64_t asInteger(std::int64_t fallback = 0) const
    {
        return isInteger() ? m_integer : fallback;
    }
    // Floats and integers both read as double (data files rarely care).
    [[nodiscard]] double asFloat(double fallback = 0.0) const
    {
        if (isFloat()) {
            return m_float;
        }
        return isInteger() ? static_cast<double>(m_integer) : fallback;
    }
    [[nodiscard]] const std::string& asString() const { return m_string; }

    // Array/table element count; 0 for other types.
    [[nodiscard]] std::size_t size() const;

    // Array element access; asserts type and bounds in dev builds.
    [[nodiscard]] const TomlValue& operator[](std::size_t index) const;

    // Table member lookup (single key segment); nullptr when absent or not a
    // table.
    [[nodiscard]] const TomlValue* find(const char* key) const;

    // Dotted-path lookup across nested tables, e.g. "ship.hull.mass".
    [[nodiscard]] const TomlValue* findPath(const char* path) const;

    // Table member iteration, in file order.
    [[nodiscard]] const std::vector<std::pair<std::string, TomlValue>>& members() const
    {
        return m_table;
    }

private:
    friend class TomlParser;

    TomlType m_type = TomlType::Table;
    bool m_bool = false;
    std::int64_t m_integer = 0;
    double m_float = 0.0;
    bool m_tableArray = false; // array created by [[header]], appendable only by [[header]]
    std::string m_string;
    std::vector<TomlValue> m_array;
    std::vector<std::pair<std::string, TomlValue>> m_table;
};

} // namespace sol::core
