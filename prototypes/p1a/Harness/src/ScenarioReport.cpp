#include "Sol/Proto/Harness/ScenarioReport.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/HostInfo.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ProcessMetrics.h"
#include "Sol/Proto/Harness/SolToolchainFacts.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace sol::proto {

ScenarioReport::ScenarioReport(ScenarioMetadata metadata) : m_metadata(std::move(metadata)) {
    if (m_metadata.name.empty() || m_metadata.version.empty()) {
        throw std::invalid_argument("ScenarioReport: scenario name and version are required");
    }
    if (m_metadata.inputDescription.empty()) {
        throw std::invalid_argument("ScenarioReport: inputDescription is required by the P1a measurement rules");
    }
}

void ScenarioReport::addMetric(MetricSeries series) {
    m_metrics.push_back(std::move(series));
}

void ScenarioReport::addResultWriter(std::function<void(JsonWriter&)> writer) {
    m_resultWriters.push_back(std::move(writer));
}

void ScenarioReport::writeEnvironment(JsonWriter& writer, const std::filesystem::path& path) const {
    const CpuInfo cpu = captureCpuInfo();
    const OsInfo os = captureOsInfo();
    const ProcessMetrics memory = captureProcessMetrics();
    const AllocationCounts allocationCounts = allocations::snapshot();

    writer.beginObject("environment");
    writer.write("timestampUtc", captureUtcTimestamp());
    writer.write("rawOutputPath", path.generic_string());

    writer.beginObject("build");
    writer.write("gitCommit", facts::kGitCommit);
    writer.write("gitDirty", facts::kGitDirty);
    writer.write("preset", facts::kBuildPreset);
    writer.write("buildType", facts::kBuildType);
    writer.write("compilerId", facts::kCompilerId);
    writer.write("compilerVersion", facts::kCompilerVersion);
    writer.write("cxxStandardFlag", facts::kCxxStandardFlag);
    writer.write("cxxFlags", facts::kCxxFlags);
    writer.write("fpContractionMode", facts::kFpContractionMode);
    writer.write("fpContractionFlag", facts::kFpContractionFlag);
    writer.write("negativeControlContract", facts::kNegativeControlContract);
    writer.endObject();

    writer.beginObject("host");
    writer.write("osVersion", os.version);
    writer.write("cpuBrand", cpu.brand);
    writer.write("cpuVendor", cpu.vendor);
    writer.write("logicalProcessors", static_cast<std::uint64_t>(cpu.logicalProcessorCount));
    writer.write("hasAvx", cpu.hasAvx);
    writer.write("hasAvx2", cpu.hasAvx2);
    writer.write("hasFma", cpu.hasFma);
    writer.endObject();

    writer.beginObject("process");
    writer.write("peakWorkingSetBytes", memory.peakWorkingSetBytes);
    writer.write("peakPagefileBytes", memory.peakPagefileBytes);
    writer.write("allocationCount", allocationCounts.allocationCount);
    writer.write("deallocationCount", allocationCounts.deallocationCount);
    writer.write("totalAllocatedBytes", allocationCounts.totalAllocatedBytes);
    writer.write("liveBytes", allocationCounts.liveBytes);
    writer.endObject();

    writer.endObject();
}

void ScenarioReport::writeResults(JsonWriter& writer) const {
    writer.beginObject("results");

    writer.beginObject("scenario");
    writer.write("name", m_metadata.name);
    writer.write("version", m_metadata.version);
    writer.write("inputDescription", m_metadata.inputDescription);
    writer.write("seed", m_metadata.seed);
    writer.write("warmupIterations", m_metadata.warmupIterations);
    writer.write("sampleCount", m_metadata.sampleCount);
    writer.endObject();

    if (!m_metrics.empty()) {
        writer.beginObject("metrics");
        for (const MetricSeries& series : m_metrics) {
            series.writeTo(writer, m_includeRawSamples);
        }
        writer.endObject();
    }

    for (const auto& resultWriter : m_resultWriters) {
        resultWriter(writer);
    }

    writer.endObject();
}

void ScenarioReport::writeToFile(const std::filesystem::path& path) const {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code errorCode;
        std::filesystem::create_directories(parent, errorCode);
        if (errorCode) {
            throw std::runtime_error("ScenarioReport: cannot create directory '"
                                     + parent.string() + "': " + errorCode.message());
        }
    }

    // Binary mode keeps "\n" from being translated to "\r\n", so a report written on
    // Windows is byte-identical to the same report written anywhere else.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("ScenarioReport: cannot open '" + path.string() + "' for writing");
    }

    {
        JsonWriter writer(out);
        writer.beginObject();
        writer.write("reportFormatVersion", std::uint64_t{1});
        writeEnvironment(writer, path);
        writeResults(writer);
        writer.endObject();
    }

    out.flush();
    if (!out) {
        throw std::runtime_error("ScenarioReport: failed while writing '" + path.string() + "'");
    }
}

std::filesystem::path parseOutputPath(int argc, char** argv,
                                      const std::filesystem::path& fallback) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--out") {
            if (index + 1 >= argc) {
                throw std::invalid_argument("--out requires a path argument");
            }
            return std::filesystem::path(argv[index + 1]);
        }
    }
    return fallback;
}

} // namespace sol::proto
