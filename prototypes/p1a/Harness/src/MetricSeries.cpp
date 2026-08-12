#include "Sol/Proto/Harness/MetricSeries.h"

#include "Sol/Proto/Harness/JsonWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace sol::proto {
namespace {

/// Nearest-rank percentile over already-sorted samples. @p fraction is in [0, 1].
double nearestRank(const std::vector<double>& sorted, double fraction) {
    const auto count = sorted.size();
    auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(count)));
    if (rank == 0) {
        rank = 1;
    }
    if (rank > count) {
        rank = count;
    }
    return sorted[rank - 1];
}

} // namespace

MetricSeries::MetricSeries(std::string name, std::string unit)
    : m_name(std::move(name)), m_unit(std::move(unit)) {
    if (m_name.empty()) {
        throw std::invalid_argument("MetricSeries: name must not be empty");
    }
    if (m_unit.empty()) {
        throw std::invalid_argument("MetricSeries: unit must not be empty; P1a requires explicit units");
    }
}

void MetricSeries::addSample(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("MetricSeries: sample for '" + m_name + "' is not finite");
    }
    m_samples.push_back(value);
}

void MetricSeries::reserve(std::size_t sampleCount) {
    m_samples.reserve(sampleCount);
}

MetricSummary MetricSeries::summarize() const {
    if (m_samples.empty()) {
        throw std::logic_error("MetricSeries: cannot summarize empty series '" + m_name + "'");
    }

    std::vector<double> sorted = m_samples;
    std::sort(sorted.begin(), sorted.end());

    MetricSummary summary;
    summary.count = sorted.size();
    summary.min = sorted.front();
    summary.max = sorted.back();

    // Insertion-order summation: reproducible on a given build, per ADR 0010.
    double total = 0.0;
    for (const double sample : m_samples) {
        total += sample;
    }
    const auto count = static_cast<double>(sorted.size());
    summary.mean = total / count;

    const std::size_t middle = sorted.size() / 2;
    summary.median = (sorted.size() % 2 == 0) ? (sorted[middle - 1] + sorted[middle]) * 0.5
                                              : sorted[middle];

    summary.p95 = nearestRank(sorted, 0.95);
    summary.p99 = nearestRank(sorted, 0.99);

    double sumSquaredDeviation = 0.0;
    for (const double sample : m_samples) {
        const double deviation = sample - summary.mean;
        sumSquaredDeviation += deviation * deviation;
    }
    summary.standardDeviation = std::sqrt(sumSquaredDeviation / count);

    return summary;
}

void MetricSeries::writeTo(JsonWriter& writer, bool includeRawSamples) const {
    const MetricSummary summary = summarize();

    writer.beginObject(m_name);
    writer.write("unit", m_unit);
    writer.write("count", static_cast<std::uint64_t>(summary.count));
    writer.write("min", summary.min);
    writer.write("max", summary.max);
    writer.write("mean", summary.mean);
    writer.write("median", summary.median);
    writer.write("p95", summary.p95);
    writer.write("p99", summary.p99);
    writer.write("standardDeviation", summary.standardDeviation);

    if (includeRawSamples) {
        writer.beginArray("samples");
        for (const double sample : m_samples) {
            writer.writeValue(sample);
        }
        writer.endArray();
    }

    writer.endObject();
}

} // namespace sol::proto
