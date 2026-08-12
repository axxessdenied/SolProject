/// ADR 0001 conformance gate for the C++23 facilities SolProject intends to rely on.
///
/// ADR 0001 requires "a small conformance target [that] compiles using each required C++23
/// facility" and warns that a language mode existing does not mean every library facility is
/// complete. This file is that target.
///
/// Each facility is gated twice: a static_assert on its feature-test macro, which produces a
/// precise diagnostic naming the missing facility, and a real use site, which proves the
/// implementation actually works rather than merely advertising itself. Without the macro
/// check a missing facility surfaces as a wall of template errors; without the use site a
/// broken-but-advertised facility passes.
///
/// This list is the project's *required* C++23 set. Adding to it raises the effective MSVC
/// floor, so a new entry needs a stated reason. Removing one is a finding to record, not a
/// silent edit.
///
/// The executable runs as a CTest test and returns EXIT_SUCCESS; the real gate is that it
/// compiled at all.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// --------------------------------------------------------------------------------------
// Feature-test macros. Values are the standard's minimums for the C++23 form of each.
// --------------------------------------------------------------------------------------

// Error propagation for parsers, loaders, and fixture readers without exceptions on the
// hot path or an out-parameter convention.
static_assert(__cpp_lib_expected >= 202202L, "std::expected is required");

// Diagnostic and report output. Formatting correctness matters because measurement values
// are written by hand-rolled code elsewhere and printf-family formatting is locale-sensitive.
static_assert(__cpp_lib_print >= 202207L, "std::print/std::println are required");

// Scoped-enum-to-underlying conversion at persistence and JSON boundaries, where ADR 0004
// requires fixed-width types and an explicit conversion is clearer than a cast.
static_assert(__cpp_lib_to_underlying >= 202102L, "std::to_underlying is required");

// Byte-order handling at the persistence boundary (ADR 0009 chunked binary containers).
static_assert(__cpp_lib_byteswap >= 202110L, "std::byteswap is required");

// Range pipelines over parallel sample/fixture arrays without index bookkeeping, which is a
// common source of off-by-one errors in measurement code.
static_assert(__cpp_lib_ranges_zip >= 202110L, "views::zip is required");
static_assert(__cpp_lib_ranges_enumerate >= 202302L, "views::enumerate is required");

// Materializing a range into an owning container, needed wherever a deterministic ordered
// copy is required before summarizing.
static_assert(__cpp_lib_ranges_to_container >= 202202L, "ranges::to is required");

// Explicit object parameter ("deducing this"): removes const/non-const accessor duplication
// in the value types that units-and-frames work in increment A2 will need.
static_assert(__cpp_explicit_this_parameter >= 202110L, "deducing this is required");

// Compile-time-context detection, for constants that must be computed differently in
// constant evaluation than at runtime.
static_assert(__cpp_if_consteval >= 202106L, "if consteval is required");

// Multidimensional subscript, for fixture and matrix-like tables.
static_assert(__cpp_multidimensional_subscript >= 202110L, "multidimensional operator[] is required");

// Decay-copy in an expression, used to force a value copy out of a reference-returning
// accessor without naming the type.
//
// A1 finding (MSVC 19.51.36252): the feature is implemented and the use site in main()
// compiles, but the compiler does not define __cpp_auto_cast. Asserting on the macro would
// reject a working toolset, so the use site alone is the gate for this one. Revisit if a
// later toolset starts advertising it.
#if defined(__cpp_auto_cast)
static_assert(__cpp_auto_cast >= 202110L, "auto(x) decay-copy is required");
#endif

// Static operator(), for stateless comparators and projections with no implicit this cost.
static_assert(__cpp_static_call_operator >= 202207L, "static operator() is required");

namespace {

// --- std::expected --------------------------------------------------------------------
std::expected<int, std::string> parsePositive(int value) {
    if (value <= 0) {
        return std::unexpected(std::string("not positive"));
    }
    return value;
}

// --- deducing this ---------------------------------------------------------------------
class SampleBuffer {
public:
    void push(double value) { m_values.push_back(value); }

    /// One definition serving both const and non-const access.
    template <typename Self>
    auto&& values(this Self&& self) {
        return std::forward<Self>(self).m_values;
    }

private:
    std::vector<double> m_values;
};

// --- multidimensional subscript ---------------------------------------------------------
class FixtureTable {
public:
    constexpr double operator[](std::size_t row, std::size_t column) const {
        return m_cells[row * kColumns + column];
    }

private:
    static constexpr std::size_t kColumns = 3;
    std::array<double, 6> m_cells{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
};

// --- static operator() -------------------------------------------------------------------
struct AbsoluteValueLess {
    static constexpr bool operator()(double left, double right) {
        const double leftMagnitude = left < 0.0 ? -left : left;
        const double rightMagnitude = right < 0.0 ? -right : right;
        return leftMagnitude < rightMagnitude;
    }
};

// --- if consteval ---------------------------------------------------------------------
constexpr int scaleFactor() {
    if consteval {
        return 2;
    } else {
        return 2;
    }
}

enum class ArtifactKind : std::uint8_t { Campaign = 1, Blueprint = 2 };

} // namespace

int main() {
    // std::expected
    const auto parsed = parsePositive(7);
    const auto rejected = parsePositive(-1);
    if (!parsed.has_value() || parsed.value() != 7 || rejected.has_value()) {
        return EXIT_FAILURE;
    }

    // std::to_underlying
    if (std::to_underlying(ArtifactKind::Blueprint) != std::uint8_t{2}) {
        return EXIT_FAILURE;
    }

    // std::byteswap
    if (std::byteswap(std::uint32_t{0x01020304u}) != std::uint32_t{0x04030201u}) {
        return EXIT_FAILURE;
    }

    // deducing this
    SampleBuffer buffer;
    buffer.push(3.5);
    const SampleBuffer& constBuffer = buffer;
    if (buffer.values().size() != 1 || constBuffer.values().front() != 3.5) {
        return EXIT_FAILURE;
    }

    // views::zip and views::enumerate
    const std::array<int, 3> left{1, 2, 3};
    const std::array<int, 3> right{10, 20, 30};
    int zipTotal = 0;
    for (const auto& [a, b] : std::views::zip(left, right)) {
        zipTotal += a * b;
    }
    if (zipTotal != 140) {
        return EXIT_FAILURE;
    }

    std::size_t indexTotal = 0;
    for (const auto& [index, value] : std::views::enumerate(left)) {
        indexTotal += static_cast<std::size_t>(index) * static_cast<std::size_t>(value);
    }
    if (indexTotal != 8) {
        return EXIT_FAILURE;
    }

    // ranges::to
    auto doubled = left | std::views::transform([](int value) { return value * scaleFactor(); })
                        | std::ranges::to<std::vector<int>>();
    if (doubled != std::vector<int>{2, 4, 6}) {
        return EXIT_FAILURE;
    }

    // multidimensional subscript
    constexpr FixtureTable table;
    if (table[1, 2] != 6.0) {
        return EXIT_FAILURE;
    }

    // static operator()
    std::array<double, 3> magnitudes{-5.0, 2.0, -1.0};
    std::ranges::sort(magnitudes, AbsoluteValueLess{});
    if (magnitudes.front() != -1.0) {
        return EXIT_FAILURE;
    }

    // auto(x) decay-copy
    auto copied = auto(buffer.values());
    copied.clear();
    if (buffer.values().size() != 1) {
        return EXIT_FAILURE;
    }

    // std::print / std::println
    std::println("Cpp23Conformance: all required C++23 facilities compiled and behaved");
    return EXIT_SUCCESS;
}
