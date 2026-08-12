#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace sol::proto::frames {

/// The subset of the SPICE text-kernel format that ADR 0008's pinned kernels need.
///
/// A text kernel interleaves free-form documentation with data. Only text between a
/// `\begindata` marker and the following `\begintext` marker is data; everything else is
/// commentary. Honouring that is not optional pedantry -- pck00011.tpc contains several
/// worked examples in its preamble that assign `BODY399_RADII` to illustrative values, and a
/// parser that merely greps for the keyword picks up a documentation example instead of the
/// adopted constant.
///
/// Supported, because the pinned kernels use it:
///   - `NAME = ( v1 v2 ... )` and `NAME = value`, with assignments spanning lines;
///   - Fortran `D` exponents (`1.657D-3`), which SPICE kernels use throughout;
///   - `@`-prefixed epoch tokens, returned verbatim as strings for the caller to interpret.
///
/// Not supported, because no pinned kernel uses it: the `+=` append form, `%`-quoted string
/// values, and inline comment characters inside a data block. Encountering `+=` throws
/// rather than being silently treated as `=`.
class TextKernel {
public:
    /// Parses @p path. Throws std::runtime_error on an unreadable file or malformed data
    /// block.
    [[nodiscard]] static TextKernel loadFromFile(const std::filesystem::path& path);

    /// Returns every token assigned to @p name, in file order.
    ///
    /// SPICE semantics let a later kernel override an earlier assignment. Within one file the
    /// pinned kernels never reassign a name inside a data block, and this class throws if one
    /// does, so a future kernel revision that starts reassigning cannot silently change which
    /// value a scenario used.
    [[nodiscard]] const std::vector<std::string>& tokens(const std::string& name) const;

    [[nodiscard]] bool has(const std::string& name) const;

    /// Returns the numeric values assigned to @p name.
    /// Throws std::runtime_error when the name is absent or any token is not a number.
    [[nodiscard]] std::vector<double> numbers(const std::string& name) const;

    /// Returns the single numeric value assigned to @p name.
    /// Throws when the name is absent or carries a different number of values.
    [[nodiscard]] double number(const std::string& name) const;

    /// Returns the numeric values assigned to @p name, requiring exactly @p expectedCount.
    [[nodiscard]] std::vector<double> numbers(const std::string& name, std::size_t expectedCount) const;

private:
    std::map<std::string, std::vector<std::string>> m_assignments;
};

/// Parses a SPICE numeric token, accepting the Fortran `D` exponent form.
/// Throws std::runtime_error when @p token is not a complete number.
[[nodiscard]] double parseKernelNumber(const std::string& token);

} // namespace sol::proto::frames
