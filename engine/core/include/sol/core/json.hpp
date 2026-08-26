#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sol::core {

enum class JsonType
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

// Immutable-after-parse JSON DOM. Sized for asset metadata (glTF), not huge documents.
class JsonValue
{
public:
    // Parses a complete JSON document; on failure returns false and sets outError.
    [[nodiscard]] static bool
    parse(const char* text, std::size_t length, JsonValue& out, std::string* outError = nullptr);

    [[nodiscard]] JsonType type() const { return m_type; }

    [[nodiscard]] bool isNull() const { return m_type == JsonType::Null; }

    [[nodiscard]] bool isBool() const { return m_type == JsonType::Bool; }

    [[nodiscard]] bool isNumber() const { return m_type == JsonType::Number; }

    [[nodiscard]] bool isString() const { return m_type == JsonType::String; }

    [[nodiscard]] bool isArray() const { return m_type == JsonType::Array; }

    [[nodiscard]] bool isObject() const { return m_type == JsonType::Object; }

    [[nodiscard]] bool asBool(bool fallback = false) const { return isBool() ? m_bool : fallback; }

    [[nodiscard]] double asNumber(double fallback = 0.0) const { return isNumber() ? m_number : fallback; }

    [[nodiscard]] const std::string& asString() const { return m_string; }

    // Array/object element count; 0 for other types.
    [[nodiscard]] std::size_t size() const;

    // Array element access; asserts type and bounds in dev builds.
    [[nodiscard]] const JsonValue& operator[](std::size_t index) const;

    // Object member lookup; nullptr when absent or not an object.
    [[nodiscard]] const JsonValue* find(const char* key) const;

    // Object member iteration.
    [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const { return m_object; }

private:
    friend class JsonParser;

    JsonType m_type = JsonType::Null;
    bool m_bool = false;
    double m_number = 0.0;
    std::string m_string;
    std::vector<JsonValue> m_array;
    std::vector<std::pair<std::string, JsonValue>> m_object;
};

} // namespace sol::core
