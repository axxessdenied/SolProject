#pragma once

#include <cstdint>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace sol::proto {

/// Minimal streaming JSON writer whose output is byte-reproducible for identical input.
///
/// Reproducibility is the whole point: P1a compares measurement files across runs, so the
/// emitter must not introduce variation of its own. It therefore guarantees:
///   - key order is call order, never a container's iteration order;
///   - doubles are written with std::to_chars shortest round-trip, so a value survives a
///     write/read cycle exactly and no locale or precision setting can perturb it;
///   - indentation is fixed at two spaces and line endings are always "\n".
///
/// The writer performs no schema validation. Misuse (ending an object that was never
/// begun, writing a keyed value inside an array) is a programming error and is reported by
/// throwing std::logic_error rather than by producing malformed JSON.
///
/// Not thread-safe. One writer belongs to one thread and one output stream.
class JsonWriter {
public:
    /// Constructs a writer over @p out. The stream must outlive the writer.
    explicit JsonWriter(std::ostream& out);

    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

    /// Verifies that every opened object and array was closed.
    /// Throws std::logic_error on an unbalanced document, because an unbalanced evidence
    /// file that merely looks truncated is worse than a loud failure.
    ~JsonWriter() noexcept(false);

    /// Opens the root object, or an element object inside an array.
    void beginObject();
    /// Opens a nested object under @p key.
    void beginObject(std::string_view key);
    void endObject();

    /// Opens an array under @p key.
    void beginArray(std::string_view key);
    void endArray();

    void write(std::string_view key, std::string_view value);

    /// Exact-match overload for C strings.
    ///
    /// Without it, `const char*` binds to the bool overload -- a standard pointer-to-bool
    /// conversion outranks the user-defined conversion to string_view -- and every C-string
    /// value is silently written as `true`. That is a data-corrupting overload trap, not a
    /// convenience gap, so this overload must not be removed.
    void write(std::string_view key, const char* value);

    void write(std::string_view key, bool value);
    void write(std::string_view key, std::int64_t value);
    void write(std::string_view key, std::uint64_t value);

    /// Writes a double in shortest round-trip form. Non-finite values are written as the
    /// strings "nan", "inf", or "-inf", since JSON has no encoding for them; a reader must
    /// treat those as failures rather than as numbers.
    void write(std::string_view key, double value);

    /// Writes the raw IEEE-754 bit pattern of @p value as a 16-digit lowercase hex string.
    ///
    /// Determinism comparisons use this rather than the decimal form. Shortest round-trip
    /// decimal is lossless, but hex makes a one-ULP difference visible on inspection
    /// instead of hiding it in a digit a reader is inclined to skim past.
    void writeBits(std::string_view key, double value);

    /// Appends a value to the currently open array.
    void writeValue(std::string_view value);
    /// Exact-match overload for C strings; see the note on write(key, const char*).
    void writeValue(const char* value);
    void writeValue(std::uint64_t value);
    void writeValue(double value);

private:
    enum class Scope : std::uint8_t { Object, Array };

    void writeIndent();
    void writeSeparator();
    void writeKey(std::string_view key);
    void writeEscaped(std::string_view value);
    void requireScope(Scope scope, const char* operation) const;

    std::ostream& m_out;
    std::vector<Scope> m_scopes;
    std::vector<bool> m_scopeHasEntry;
};

} // namespace sol::proto
