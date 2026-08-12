#pragma once

#include "Sol/Proto/Harness/MetricSeries.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sol::proto {

class JsonWriter;

/// Identity and inputs of one measurement scenario.
///
/// Every field here is required by the P1a shared measurement rules. Leaving one empty is a
/// reporting defect, not a stylistic choice: a measurement whose seed or sample count is
/// unrecorded cannot be reproduced and therefore is not evidence.
struct ScenarioMetadata {
    /// Stable identifier, for example "timing.doubleTransformLoop".
    std::string name;
    /// Bumped whenever the scenario's computation changes, so old results are not silently
    /// compared against new ones.
    std::string version;
    /// Human-readable description of the input set, its size, and its origin.
    std::string inputDescription;
    /// Seed for every generator the scenario uses. ADR 0010 forbids unseeded generators.
    std::uint64_t seed{0};
    std::uint64_t warmupIterations{0};
    std::uint64_t sampleCount{0};
};

/// Builds one scenario report file.
///
/// The report has exactly two top-level sections, and the split is load-bearing:
///
///   "environment" - provenance that legitimately varies between runs: timestamp, host,
///                   output path, peak memory. Never compared for determinism.
///   "results"     - everything the scenario computed. Byte-identical across runs of the
///                   same build, which is what the ADR 0010 same-machine bit-exactness
///                   guarantee is asserted against.
///
/// Without this split a timestamp alone would defeat every determinism comparison, and the
/// obvious workaround -- diffing while ignoring "some" lines -- would quietly also ignore a
/// real numerical change.
///
/// Determinism callers should use RunDeterminismCheck.cmake, which extracts and compares the
/// "results" section of two runs.
class ScenarioReport {
public:
    explicit ScenarioReport(ScenarioMetadata metadata);

    /// Adds a metric series to the results section. Series are emitted in insertion order.
    void addMetric(MetricSeries series);

    /// Registers a callback that writes additional scenario-specific keys into the results
    /// object. Callbacks run in registration order, after the metric series.
    void addResultWriter(std::function<void(JsonWriter&)> writer);

    /// Controls whether metric series emit their full ordered sample arrays.
    /// Off by default: raw sample arrays belong in the gitignored raw evidence directory.
    void setIncludeRawSamples(bool include) noexcept { m_includeRawSamples = include; }

    /// Writes the report to @p path, creating parent directories as needed.
    ///
    /// Output uses "\n" line endings regardless of platform text-mode conventions, so a
    /// file written here is byte-comparable with one written anywhere else.
    /// Throws std::runtime_error when the file cannot be opened.
    void writeToFile(const std::filesystem::path& path) const;

private:
    void writeEnvironment(JsonWriter& writer, const std::filesystem::path& path) const;
    void writeResults(JsonWriter& writer) const;

    ScenarioMetadata m_metadata;
    std::vector<MetricSeries> m_metrics;
    std::vector<std::function<void(JsonWriter&)>> m_resultWriters;
    bool m_includeRawSamples{false};
};

/// Parses `--out <path>` from the command line.
///
/// Returns the path following `--out`, or @p fallback when the flag is absent.
/// Throws std::invalid_argument when `--out` is given without a value, since silently
/// writing to a default path would scatter evidence files unpredictably.
[[nodiscard]] std::filesystem::path parseOutputPath(int argc, char** argv,
                                                    const std::filesystem::path& fallback);

} // namespace sol::proto
