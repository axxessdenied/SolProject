/// ADR 0010 determinism smoke scenario.
///
/// Asserts the same-machine, same-build guarantee: identical inputs reproduce identical
/// event ordering and bit-identical floating-point output.
///
/// The kernels below are chosen to be *fragile* in the specific ways the floating-point
/// policy cares about. A kernel that survives a flag change would make the whole check
/// worthless, so each one is sensitive to a documented hazard:
///
///   accumulationChain    - long dependent add/multiply chain. Sensitive to reassociation,
///                          which /fp:fast permits and /fp:precise forbids.
///   contractionProbe     - a*b + c*d shaped so that fusing the multiply-add changes the
///                          rounding. This is the kernel the /fp:contract negative control
///                          is aimed at, and the reason contraction policy is testable at
///                          all on a toolset that offers no disabling flag.
///   transcendentalChain  - dependent sqrt/sin/cos/exp chain. Sensitive to a changed math
///                          library implementation or vectorized variant selection.
///   orderedReduction     - the same values summed forwards and backwards. The two results
///                          are *expected to differ*; what must be reproducible is that they
///                          differ by exactly the same amount every run. This is what makes
///                          "deterministic iteration order" a testable requirement rather
///                          than an aspiration.
///
/// Output goes through ScenarioReport, so the nondeterministic environment section is
/// separated from the results section that RunDeterminismCheck.cmake compares byte-for-byte.
/// Values are emitted as raw IEEE-754 bit patterns, never as decimal alone.

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ScenarioReport.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <vector>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;

constexpr std::uint64_t kSeed = 0x5010'2026'0101'0000ull;
constexpr int kChainLength = 4096;
constexpr std::size_t kReductionSize = 1024;

/// Deterministic 64-bit generator (SplitMix64).
///
/// ADR 0010 forbids rand() and unseeded or standard-library-dependent engines, because their
/// implementations vary between standard library versions. This one is project-owned, its
/// algorithm is recorded here, and it produces the same stream everywhere.
class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : m_state(seed) {}

    constexpr std::uint64_t nextBits() noexcept {
        m_state += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    /// Returns a value in [1.0, 2.0), built from the mantissa bits so no division is
    /// involved and the mapping is exact.
    double nextUnitDouble() noexcept {
        const std::uint64_t bits = (nextBits() >> 12) | 0x3FF0'0000'0000'0000ull;
        double value = 0.0;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    std::uint64_t m_state;
};

/// Long dependent chain; reassociation changes the result.
double accumulationChain() {
    double accumulator = 1.0;
    for (int step = 1; step <= kChainLength; ++step) {
        const double term = static_cast<double>(step);
        accumulator = accumulator * 1.0000001 + 1.0 / term;
        accumulator -= accumulator * 1e-9;
    }
    return accumulator;
}

/// Returns @p value through a volatile round trip.
///
/// The result is identical to the input on every run, so determinism is unaffected, but the
/// compiler cannot see through it and constant-fold the arithmetic below. Without this the
/// probe would be evaluated at compile time in the compiler's own arithmetic and would
/// report nothing about the generated code.
double opaque(double value) {
    volatile double sink = value;
    return sink;
}

/// Detects whether the compiler fused a multiply into an add.
///
/// For a = 1 + e and b = 1 - e, the exact product is 1 - e^2. With e = 2^-27, that needs 54
/// significant bits, one more than a double has:
///
///   without contraction  a*b rounds to exactly 1.0, so a*b - 1.0 is exactly 0.0;
///   with contraction     the fused multiply-add keeps the full-width product, so the
///                        result is exactly -e^2.
///
/// The two outcomes differ by a whole term rather than by a low-order bit, so the probe
/// cannot be defeated by a value that happens to have zeros in its trailing mantissa. The
/// A1 negative control asserts this kernel changes when /fp:contract is added.
double contractionProbe() {
    double total = 0.0;
    for (int step = 0; step < 512; ++step) {
        const double epsilon = std::ldexp(1.0, -27 - (step % 8));
        const double a = opaque(1.0 + epsilon);
        const double b = opaque(1.0 - epsilon);
        total += a * b - 1.0;
    }
    return total;
}

double transcendentalChain() {
    double value = 0.5;
    for (int step = 0; step < 512; ++step) {
        value = std::sqrt(value + 1.0);
        value = std::sin(value) * std::cos(value) + std::exp(value * 0.01);
    }
    return value;
}

/// Sums the same values in both directions. The gap between them must be reproducible.
struct OrderedReduction {
    double forward{0.0};
    double backward{0.0};
    double difference{0.0};
};

OrderedReduction orderedReduction() {
    SplitMix64 generator(kSeed);
    std::vector<double> values;
    values.reserve(kReductionSize);
    for (std::size_t index = 0; index < kReductionSize; ++index) {
        // Spread magnitudes across many exponents so summation order genuinely matters.
        const double magnitude = std::ldexp(generator.nextUnitDouble(),
                                            static_cast<int>(index % 64) - 32);
        values.push_back(magnitude);
    }

    OrderedReduction reduction;
    for (const double value : values) {
        reduction.forward += value;
    }
    for (std::size_t index = values.size(); index > 0; --index) {
        reduction.backward += values[index - 1];
    }
    reduction.difference = reduction.forward - reduction.backward;
    return reduction;
}

} // namespace

int main(int argc, char** argv) {
    try {
        sol::proto::allocations::reset();

        const double accumulation = accumulationChain();
        const double contraction = contractionProbe();
        const double transcendental = transcendentalChain();
        const OrderedReduction reduction = orderedReduction();

        // Repeat every kernel in-process. A kernel that varies between two calls in one
        // process is broken far more seriously than one that varies between runs.
        CheckContext checks;
        checks.checkBitsEqual(accumulationChain(), accumulation,
                              "accumulationChain is stable within a single process");
        checks.checkBitsEqual(contractionProbe(), contraction,
                              "contractionProbe is stable within a single process");
        checks.checkBitsEqual(transcendentalChain(), transcendental,
                              "transcendentalChain is stable within a single process");
        checks.checkBitsEqual(orderedReduction().difference, reduction.difference,
                              "orderedReduction difference is stable within a single process");

        ScenarioMetadata metadata;
        metadata.name = "determinism.smoke";
        metadata.version = "1";
        metadata.inputDescription =
            "Four floating-point kernels seeded with a fixed SplitMix64 constant: a 4096-step "
            "dependent accumulation chain, a 512-step fused-multiply-add probe, a 512-step "
            "transcendental chain, and a 1024-value forward/backward ordered reduction.";
        metadata.seed = kSeed;
        metadata.warmupIterations = 0;
        metadata.sampleCount = 1;

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("kernels");

            writer.beginObject("accumulationChain");
            writer.writeBits("bits", accumulation);
            writer.write("value", accumulation);
            writer.endObject();

            writer.beginObject("contractionProbe");
            writer.writeBits("bits", contraction);
            writer.write("value", contraction);
            writer.endObject();

            writer.beginObject("transcendentalChain");
            writer.writeBits("bits", transcendental);
            writer.write("value", transcendental);
            writer.endObject();

            writer.beginObject("orderedReduction");
            writer.writeBits("forwardBits", reduction.forward);
            writer.writeBits("backwardBits", reduction.backward);
            writer.writeBits("differenceBits", reduction.difference);
            writer.write("difference", reduction.difference);
            writer.endObject();

            writer.endObject();

            writer.beginObject("inProcessRepeat");
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath =
            sol::proto::parseOutputPath(argc, argv, "determinism-smoke.json");
        report.writeToFile(outputPath);
        std::cout << "DeterminismSmoke: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("DeterminismSmoke");
    } catch (const std::exception& error) {
        std::cerr << "DeterminismSmoke: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
