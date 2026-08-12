#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sol::proto {

class JsonWriter;

/// Summary statistics over one metric's samples. All fields carry the series' unit.
struct MetricSummary {
    std::size_t count{0};
    double min{0.0};
    double max{0.0};
    double mean{0.0};
    double median{0.0};
    double p95{0.0};
    double p99{0.0};
    /// Population standard deviation (divisor N), not the sample estimator.
    double standardDeviation{0.0};
};

/// A named, unit-tagged set of measurements.
///
/// Samples are retained in insertion order. Statistics are computed over a sorted copy, so
/// the source order is preserved for the raw record while summaries stay order-independent.
/// Summation for the mean follows insertion order, which makes the result reproducible on a
/// given build under ADR 0010 but not associativity-independent -- do not reorder samples
/// and expect a bit-identical mean.
///
/// Percentiles use the nearest-rank method on the sorted samples: p95 is the value at
/// ceil(0.95 * N), 1-indexed. No interpolation, so every reported percentile is a value
/// that was actually measured.
///
/// Samples must be finite. Passing a NaN or infinity is a programming error and throws,
/// because a silently poisoned statistic is the exact failure P1a cannot afford.
class MetricSeries {
public:
    /// @param name  Identifier used as the JSON key; must be unique within a report.
    /// @param unit  Explicit unit, for example "ns", "bytes", or "m". Never omit it.
    MetricSeries(std::string name, std::string unit);

    /// Throws std::invalid_argument when @p value is not finite.
    void addSample(double value);

    void reserve(std::size_t sampleCount);

    [[nodiscard]] std::string_view name() const noexcept { return m_name; }
    [[nodiscard]] std::string_view unit() const noexcept { return m_unit; }
    [[nodiscard]] const std::vector<double>& samples() const noexcept { return m_samples; }
    [[nodiscard]] bool empty() const noexcept { return m_samples.empty(); }

    /// Computes the summary. Throws std::logic_error when the series is empty, since an
    /// empty summary would report zeros that look like real measurements.
    [[nodiscard]] MetricSummary summarize() const;

    /// Writes `"<name>": { unit, count, min, ... }` into the currently open JSON object.
    /// @param includeRawSamples  When true, also emits the full ordered sample array.
    void writeTo(JsonWriter& writer, bool includeRawSamples) const;

private:
    std::string m_name;
    std::string m_unit;
    std::vector<double> m_samples;
};

} // namespace sol::proto
