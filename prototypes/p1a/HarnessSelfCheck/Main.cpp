/// Self-check for the measurement harness.
///
/// The harness is the durable half of P1a: every number increments A2 and A3 report passes
/// through JsonWriter and MetricSeries. A silent defect here does not crash anything, it
/// quietly corrupts evidence -- which is the one failure mode the milestone plan cannot
/// tolerate, because the corrupted output still looks like a valid measurement.
///
/// The const-char* case below is not hypothetical. Before this check existed, `const char*`
/// bound to the bool overload of JsonWriter::write and every provenance string in every
/// report was emitted as `true`. The build succeeded and the tests passed.

#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/MetricSeries.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::MetricSeries;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void checkStringOverloadsAreNotBool(CheckContext& checks) {
    const char* const provenance = "abc123";

    std::ostringstream out;
    {
        JsonWriter writer(out);
        writer.beginObject();
        writer.write("cString", provenance);
        writer.write("stringView", std::string_view("def456"));
        writer.write("realBool", true);
        writer.endObject();
    }
    const std::string text = out.str();

    checks.check(contains(text, "\"cString\": \"abc123\""),
                 "a const char* value is written as a string, not as a bool");
    checks.check(contains(text, "\"stringView\": \"def456\""),
                 "a string_view value is written as a string");
    checks.check(contains(text, "\"realBool\": true"),
                 "a genuine bool is still written as a bool");
}

void checkDoubleRoundTrip(CheckContext& checks) {
    // Values chosen to break naive formatting: a value needing all 17 significant digits,
    // a denormal, and an integral value that must not lose its floating-point identity.
    const double awkward = 0.1 + 0.2;
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double integral = 42.0;

    std::ostringstream out;
    {
        JsonWriter writer(out);
        writer.beginObject();
        writer.write("awkward", awkward);
        writer.write("tiny", tiny);
        writer.write("integral", integral);
        writer.writeBits("awkwardBits", awkward);
        writer.endObject();
    }
    const std::string text = out.str();

    checks.check(contains(text, "0.30000000000000004"),
                 "a double is written with shortest round-trip precision, not truncated");
    checks.check(contains(text, "\"integral\": 42.0"),
                 "an integral-valued double keeps a fractional part so it reads as a measurement");
    checks.check(contains(text, "5e-324"), "a denormal survives formatting");
    checks.check(contains(text, "\"awkwardBits\": \"3fd3333333333334\""),
                 "bit patterns are emitted as 16 lowercase hex digits");
}

void checkWriterRejectsMisuse(CheckContext& checks) {
    bool threw = false;
    try {
        std::ostringstream out;
        JsonWriter writer(out);
        writer.beginObject();
        writer.beginArray("values");
        writer.write("key", std::string_view("value")); // keyed write inside an array
        writer.endArray();
        writer.endObject();
    } catch (const std::logic_error&) {
        threw = true;
    }
    checks.check(threw, "a keyed write inside an array is rejected rather than silently malformed");

    bool unbalancedThrew = false;
    try {
        std::ostringstream out;
        JsonWriter writer(out);
        writer.beginObject();
        // Destroyed without endObject().
    } catch (const std::logic_error&) {
        unbalancedThrew = true;
    }
    checks.check(unbalancedThrew, "an unclosed document is reported rather than written truncated");
}

void checkMetricStatistics(CheckContext& checks) {
    MetricSeries series("probe", "ns");
    for (int value = 1; value <= 100; ++value) {
        series.addSample(static_cast<double>(value));
    }

    const auto summary = series.summarize();
    checks.check(summary.count == 100, "every sample is retained");
    checks.check(summary.min == 1.0 && summary.max == 100.0, "min and max are exact");
    checks.check(summary.mean == 50.5, "mean over 1..100 is 50.5");
    checks.check(summary.median == 50.5, "median of an even-sized series averages the middle pair");
    // Nearest rank, 1-indexed: p95 is element ceil(0.95 * 100) = 95.
    checks.check(summary.p95 == 95.0, "p95 uses nearest rank and returns a measured value");
    checks.check(summary.p99 == 99.0, "p99 uses nearest rank and returns a measured value");

    bool rejectedEmpty = false;
    try {
        MetricSeries empty("empty", "ns");
        (void)empty.summarize();
    } catch (const std::logic_error&) {
        rejectedEmpty = true;
    }
    checks.check(rejectedEmpty, "an empty series refuses to report zeros as if they were measurements");

    bool rejectedNonFinite = false;
    try {
        MetricSeries poisoned("poisoned", "ns");
        poisoned.addSample(std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument&) {
        rejectedNonFinite = true;
    }
    checks.check(rejectedNonFinite, "a non-finite sample is rejected at the point of entry");

    bool rejectedUnitless = false;
    try {
        MetricSeries unitless("unitless", "");
    } catch (const std::invalid_argument&) {
        rejectedUnitless = true;
    }
    checks.check(rejectedUnitless, "a series without an explicit unit is rejected");
}

} // namespace

int main() {
    try {
        CheckContext checks;
        checkStringOverloadsAreNotBool(checks);
        checkDoubleRoundTrip(checks);
        checkWriterRejectsMisuse(checks);
        checkMetricStatistics(checks);
        return checks.summarize("HarnessSelfCheck");
    } catch (const std::exception& error) {
        std::cerr << "HarnessSelfCheck: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
