/// Host and toolchain capability report.
///
/// Increment A1 must deliver "capability reporting for the host toolchain" and record the
/// exact supported MSVC toolset, preset names, and applied floating-point flags. This target
/// produces that record as machine-readable output rather than as prose someone typed.
///
/// It also *verifies* the parts of ADR 0010 that a flag string alone cannot establish:
/// whether the CPU actually provides the AVX2 the binary was compiled for, and whether the
/// C++23 mode the build claims is the one the compiler is in.

#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/HostInfo.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ScenarioReport.h"
#include "Sol/Proto/Harness/SolToolchainFacts.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;

/// Feature-test macro values, with 0 standing for "the compiler does not define this".
///
/// Each is captured through an explicit #if defined() rather than used directly, because a
/// missing macro is a hard compile error at the use site. Distinguishing "absent" from
/// "present but old" is the whole point of this report: MSVC 19.51 implements auto(x) while
/// defining no __cpp_auto_cast, and a report that could not say so would be misleading.
struct FeatureMacro {
    std::string_view name;
    long value;
};

#if defined(__cpp_auto_cast)
constexpr long kAutoCast = __cpp_auto_cast;
#else
constexpr long kAutoCast = 0;
#endif

#if defined(__cpp_lib_ranges_enumerate)
constexpr long kRangesEnumerate = __cpp_lib_ranges_enumerate;
#else
constexpr long kRangesEnumerate = 0;
#endif

#if defined(__cpp_lib_ranges_to_container)
constexpr long kRangesToContainer = __cpp_lib_ranges_to_container;
#else
constexpr long kRangesToContainer = 0;
#endif

/// Writes one feature-test macro's presence and value.
void writeFeature(JsonWriter& writer, const FeatureMacro& feature) {
    writer.beginObject(feature.name);
    writer.write("present", feature.value != 0);
    writer.write("value", static_cast<std::int64_t>(feature.value));
    writer.endObject();
}

} // namespace

int main(int argc, char** argv) {
    try {
        CheckContext checks;

        const sol::proto::CpuInfo cpu = sol::proto::captureCpuInfo();

        // ADR 0010 compiles every project-owned target with /arch:AVX2. If the host does not
        // provide usable AVX2 the binary is not merely slow, it is invalid, and any
        // measurement taken from it is void.
        checks.check(cpu.hasAvx2,
                     "host CPU provides OS-enabled AVX2, as ADR 0010's /arch:AVX2 requires");

        // MSVC only reports a truthful __cplusplus under /Zc:__cplusplus, which
        // sol_apply_project_options sets. C++23 is 202302L.
        checks.check(__cplusplus >= 202302L,
                     "compiler reports a C++23 or later __cplusplus value");

        checks.check(std::string_view(sol::proto::facts::kBuildPreset)
                         .find("unknown") == std::string_view::npos,
                     "build was configured through a checked-in preset");

        checks.check(!sol::proto::facts::kNegativeControlContract,
                     "build is not an ADR 0010 negative control (evidence builds must not be)");

        ScenarioMetadata metadata;
        metadata.name = "toolchain.capabilityReport";
        metadata.version = "1";
        metadata.inputDescription =
            "Static toolchain facts baked in at configure time, plus CPUID and RtlGetVersion "
            "queries of the host at run time. No workload.";
        metadata.seed = 0;
        metadata.warmupIterations = 0;
        metadata.sampleCount = 0;

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            const FeatureMacro features[] = {
                {"__cplusplus", __cplusplus},
                {"__cpp_lib_expected", __cpp_lib_expected},
                {"__cpp_lib_print", __cpp_lib_print},
                {"__cpp_lib_to_underlying", __cpp_lib_to_underlying},
                {"__cpp_lib_byteswap", __cpp_lib_byteswap},
                {"__cpp_lib_ranges_zip", __cpp_lib_ranges_zip},
                {"__cpp_lib_ranges_enumerate", kRangesEnumerate},
                {"__cpp_lib_ranges_to_container", kRangesToContainer},
                {"__cpp_explicit_this_parameter", __cpp_explicit_this_parameter},
                {"__cpp_if_consteval", __cpp_if_consteval},
                {"__cpp_multidimensional_subscript", __cpp_multidimensional_subscript},
                {"__cpp_auto_cast", kAutoCast},
                {"__cpp_static_call_operator", __cpp_static_call_operator},
            };

            writer.beginObject("cxx23Facilities");
            for (const FeatureMacro& feature : features) {
                writeFeature(writer, feature);
            }
            writer.endObject();

            writer.beginObject("msvc");
            writer.write("_MSC_VER", static_cast<std::int64_t>(_MSC_VER));
            writer.write("_MSC_FULL_VER", static_cast<std::int64_t>(_MSC_FULL_VER));
            writer.endObject();

            writer.beginObject("adr0010Checks");
            writer.write("hostAvx2Usable", cpu.hasAvx2);
            writer.write("hostFmaAvailable", cpu.hasFma);
            writer.write("cxx23ModeConfirmed", __cplusplus >= 202302L);
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath =
            sol::proto::parseOutputPath(argc, argv, "toolchain-report.json");
        report.writeToFile(outputPath);
        std::cout << "ToolchainReport: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("ToolchainReport");
    } catch (const std::exception& error) {
        std::cerr << "ToolchainReport: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
