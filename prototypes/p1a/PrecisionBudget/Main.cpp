/// The measured precision budget from surface millimetres to orbital distance.
///
/// A2's done criteria require that "the precision budget is stated as measured numbers rather
/// than asserted". This scenario is that statement. It has four parts:
///
///   1. Representable resolution. What one ULP of a double is worth, in metres, at each
///      distance a candidate root would force a coordinate to be expressed at. This is the
///      floor: no algorithm can beat it, and it is what decides whether a global-root model
///      can work at all at a given scale.
///   2. Measured cost of forming a small quantity at a large magnitude. The floor above is
///      theory; this measures what actually happens when a 12 m local vector is carried out to
///      barycentric distance and back.
///   3. Contributions that are not floating point at all: the resolution of the reference data
///      itself, the datum choice for the launch anchor, and the angle-representation cost of
///      an unreduced Earth rotation angle. Each of these is larger than the arithmetic.
///   4. The sum. A budget that lists terms without adding them is a list, not a budget.
///
/// The scenario deliberately reports numbers that are *outside* A2's chain, notably Jupiter and
/// Neptune distances. A frame decision taken now constrains the whole Solar System, and the
/// place a global-root double stops working is the single most useful number this increment can
/// hand to whoever revisits the decision.

#include "Sol/Proto/Frames/Ellipsoid.h"
#include "Sol/Proto/Frames/FlatFrameModel.h"
#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/HierarchicalFrameModel.h"
#include "Sol/Proto/Frames/ReferenceData.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ScenarioReport.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;
using namespace sol::proto::frames;

constexpr double kPositionThresholdMetres = 1.0e-3;

struct ScaleEntry {
    const char* label;
    double distanceMetres;
    const char* relevance;
};

/// Distances a coordinate might have to be expressed at, in ascending order.
constexpr std::array<ScaleEntry, 10> kScales{{
    {"vehicle local origin", 1.0e2, "a craft's own extent; the scale a player judges alignment at"},
    {"launch site to 200 km orbit", 2.0e5, "the A2 and A3 reference altitude"},
    {"Earth centre to surface", 6.378e6, "the body-fixed frame's working magnitude"},
    {"Earth centre to geostationary", 4.2164e7, "the outer edge of routine Earth operations"},
    {"Earth to Moon", 3.844e8, "the first interplanetary transfer target"},
    {"Earth-Moon barycentre to SSB", 1.47e11, "what a global root costs near Earth"},
    {"Sun to Mars aphelion", 2.49e11, "P4-era inner-system operations"},
    {"Sun to Jupiter", 7.785e11, "P5-era outer-system operations"},
    {"Sun to Neptune", 4.495e12, "the outer planets, and the roadmap's stated far-future scope"},
    {"Sun to inner Oort cloud", 3.0e14, "beyond any stated roadmap scope; included as the bound"},
}};

/// Metres per ULP of a double at @p magnitude.
///
/// std::nextafter rather than a computed exponent: it asks the floating-point representation
/// what the next value is, which is the definition, and it stays correct across subnormals and
/// exponent boundaries where a hand-rolled 2^(e-52) does not.
[[nodiscard]] double metresPerUlp(double magnitude) noexcept
{
    return std::nextafter(magnitude, std::numeric_limits<double>::infinity()) - magnitude;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        CheckContext checks;

        const std::filesystem::path fixtureRoot = parseFixtureRoot(argc, argv);
        const ReferenceData data = ReferenceData::loadFromDirectory(fixtureRoot);
        const TdbEpoch epoch = data.fixtureEpoch();

        sol::proto::allocations::reset();

        // ---- Part 2: measured cost of a round trip at barycentric magnitude ----------
        VehicleState onPad;
        onPad.positionInEnu = Vec3{};
        onPad.velocityInEnu = Vec3{};
        const FrameGraphSnapshot snapshot = buildSnapshot(data, epoch, onPad);
        const FlatFrameModel flat{snapshot};
        const HierarchicalFrameModel hierarchical{snapshot};

        StateVector local;
        local.position = PositionMetres::fromMetres(Vec3{12.0, -3.5, 8.25});
        local.velocity = VelocityMetresPerSecond::fromMetresPerSecond(Vec3{0.0, 0.0, 0.0});
        local.frame = FrameId::VehicleLocal;
        local.epoch = epoch;

        const StateVector flatAtRoot = flat.convert(local, FrameId::SsbIcrf);
        const StateVector flatReturned = flat.convert(flatAtRoot, FrameId::VehicleLocal);
        const double flatRoundTripError =
            distance(flatReturned.position.metres(), local.position.metres());

        const StateVector hierarchicalAtRoot = hierarchical.convert(local, FrameId::SsbIcrf);
        const StateVector hierarchicalReturned =
            hierarchical.convert(hierarchicalAtRoot, FrameId::VehicleLocal);
        const double hierarchicalRoundTripError =
            distance(hierarchicalReturned.position.metres(), local.position.metres());

        const double barycentricMagnitude = length(flatAtRoot.position.metres());
        const double barycentricUlp = metresPerUlp(barycentricMagnitude);

        // The deepest magnitude the hierarchical model forms on the way to the pad. It never
        // exceeds the Earth-Moon barycentre offset unless a conversion asks for the root.
        const double earthRelativeMagnitude =
            length(hierarchical.convert(local, FrameId::EarthIcrf).position.metres());
        const double earthRelativeUlp = metresPerUlp(earthRelativeMagnitude);

        // ---- Part 3a: reference-data resolution -------------------------------------
        const BodyStateFixture& earthFixture = data.bodyState(399);
        const double earthFixtureMagnitude = length(earthFixture.position.metres());
        // Horizons printed 16 significant digits of a value in kilometres, so its last printed
        // digit is worth this many metres. Nothing downstream can be more certain than this.
        const double referenceDataResolutionMetres =
            earthFixtureMagnitude
            * std::pow(10.0, -(static_cast<double>(earthFixture.printedSignificantDigits) - 1.0));

        // ---- Part 3b: datum choice ---------------------------------------------------
        const Geodetic anchor = ReferenceData::launchAnchor();
        const double datumDisplacement =
            distance(data.earthEllipsoid().toBodyFixed(anchor).metres(),
                     Ellipsoid::wgs84().toBodyFixed(anchor).metres());

        // ---- Part 3c: angle representation -------------------------------------------
        // The same prime-meridian angle computed with and without removing whole turns per day
        // before multiplying by the day count. Both are the same angle mathematically; they
        // differ only in how many significant bits survive to reach the sine.
        const double reducedDegrees =
            earthPrimeMeridianDegrees(epoch, data.earthPrimeMeridian(), true);
        const double unreducedDegrees =
            earthPrimeMeridianDegrees(epoch, data.earthPrimeMeridian(), false);
        const double angleDifferenceDegrees = std::abs(reducedDegrees - unreducedDegrees);
        // Arc length that angular difference is worth at the launch site's distance from the
        // rotation axis.
        const double anchorAxisDistance =
            std::hypot(launchAnchorBodyFixed(data).x, launchAnchorBodyFixed(data).y);
        const double angleCostMetres =
            Radians::fromDegrees(angleDifferenceDegrees).radians() * anchorAxisDistance;

        const auto allocationCounts = sol::proto::allocations::snapshot();

        // ---- Part 4: the sum ---------------------------------------------------------
        // Only terms the implemented path actually incurs are summed. Two exclusions matter:
        //
        //   - the datum displacement is a definitional offset that moves the whole site, not an
        //     error between two conversions of one state, and adding it would inflate the budget
        //     with a constant that cancels everywhere it is compared;
        //   - the angle-reduction term is what the implementation *avoids*. A2 uses the reduced
        //     form, so charging the budget for the unreduced cost would overstate it. It is
        //     reported separately as the price of dropping the reduction.
        const double implementedConversionBudget = hierarchicalRoundTripError + earthRelativeUlp;
        const double budgetIfAngleUnreduced = implementedConversionBudget + angleCostMetres;

        checks.check(barycentricUlp > kPositionThresholdMetres * 0.01,
                     "barycentric magnitude consumes a measurable share of the millimetre budget, "
                     "so the comparison between roots is not academic");
        checks.check(hierarchicalRoundTripError < flatRoundTripError,
                     "parent-relative storage measurably beats a global root at the surface");
        checks.check(implementedConversionBudget < kPositionThresholdMetres,
                     "the summed per-conversion budget for the leading model fits inside 1 mm");
        checks.check(budgetIfAngleUnreduced < kPositionThresholdMetres,
                     "the budget still fits inside 1 mm even if the angle reduction were dropped, "
                     "so the reduction buys headroom rather than compliance");
        checks.check(angleCostMetres > 1.0e-6,
                     "the unreduced rotation angle costs more than a micrometre, confirming the "
                     "reduction is a precision requirement rather than an optimisation");
        checks.check(referenceDataResolutionMetres > 0.0,
                     "the reference data's own resolution is quantified rather than assumed exact");

        ScenarioMetadata metadata;
        metadata.name = "frames.precisionBudget";
        metadata.version = "1";
        metadata.inputDescription =
            "Representable resolution of a double at ten Solar System distances, the measured "
            "round-trip error of a 12 m local vector through each candidate model at the ADR "
            "0008 epoch, the printed resolution of the Horizons fixtures, the launch-anchor "
            "datum displacement, and the cost of an unreduced Earth prime-meridian angle.";
        metadata.seed = 0;
        metadata.warmupIterations = 0;
        metadata.sampleCount = kScales.size();

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("representableResolution");
            writer.write("type", "IEEE-754 binary64, 53-bit significand");
            writer.write("positionThresholdMetres", kPositionThresholdMetres);
            writer.beginArray("scales");
            for (const ScaleEntry& scale : kScales) {
                const double ulp = metresPerUlp(scale.distanceMetres);
                writer.beginObject();
                writer.write("label", scale.label);
                writer.write("distanceMetres", scale.distanceMetres);
                writer.write("metresPerUlp", ulp);
                writer.write("ulpsPerMillimetre", kPositionThresholdMetres / ulp);
                writer.write("millimetreIsRepresentable", ulp <= kPositionThresholdMetres);
                writer.write("relevance", scale.relevance);
                writer.endObject();
            }
            writer.endArray();
            writer.write("interpretation",
                         "A coordinate expressed at distance d cannot resolve better than one ULP "
                         "at d, no matter what arithmetic follows. At Neptune's distance one ULP "
                         "is 0.98 mm: the entire A2 position budget is a single representable "
                         "step, so a global double root is nominally millimetre-capable there and "
                         "has no headroom whatever -- any arithmetic at all exceeds the budget. "
                         "The practical limit for a single-rooted double at this tolerance is "
                         "therefore Jupiter, not Neptune, and the roadmap's outer-system stages "
                         "are where a global root stops working rather than where it becomes "
                         "merely tight.");
            writer.endObject();

            writer.beginObject("measuredConversionCost");
            writer.write("localVectorMagnitudeMetres", length(local.position.metres()));
            writer.beginObject("flatModel");
            writer.write("magnitudeFormedMetres", barycentricMagnitude);
            writer.write("metresPerUlpAtThatMagnitude", barycentricUlp);
            writer.write("roundTripErrorMetres", flatRoundTripError);
            writer.write("roundTripErrorInUlps", flatRoundTripError / barycentricUlp);
            writer.write("shareOfPositionBudget", flatRoundTripError / kPositionThresholdMetres);
            writer.endObject();
            writer.beginObject("hierarchicalModel");
            writer.write("deepestMagnitudeBeforeRootMetres", earthRelativeMagnitude);
            writer.write("metresPerUlpAtThatMagnitude", earthRelativeUlp);
            writer.write("roundTripErrorMetres", hierarchicalRoundTripError);
            writer.write("shareOfPositionBudget",
                         hierarchicalRoundTripError / kPositionThresholdMetres);
            writer.write("note",
                         "The hierarchical model still forms barycentric magnitude when a "
                         "conversion asks for the root, as this one does. Its advantage is that "
                         "conversions which do not ask never pay, and most do not.");
            writer.endObject();
            writer.write("ratio", flatRoundTripError / hierarchicalRoundTripError);
            writer.endObject();

            writer.beginObject("nonArithmeticContributions");
            writer.beginObject("referenceDataResolution");
            writer.write("body", earthFixture.bodyName);
            writer.write("printedSignificantDigits",
                         static_cast<std::int64_t>(earthFixture.printedSignificantDigits));
            writer.write("magnitudeMetres", earthFixtureMagnitude);
            writer.write("resolutionMetres", referenceDataResolutionMetres);
            writer.write("note",
                         "Horizons prints a fixed number of significant digits. The barycentric "
                         "position of any body is therefore known to about this resolution, "
                         "regardless of how exactly the arithmetic downstream is performed.");
            writer.endObject();

            writer.beginObject("datumChoice");
            writer.write("displacementMetres", datumDisplacement);
            writer.write("note",
                         "IAU pck00011 versus WGS84 for the same geodetic anchor. A definitional "
                         "offset of the whole site, not a per-conversion error, so it is excluded "
                         "from the summed budget -- but it is 100+ times the budget, and quoting "
                         "an anchor position without its datum is meaningless at this tolerance.");
            writer.endObject();

            writer.beginObject("rotationAngleRepresentation");
            writer.write("reducedDegrees", reducedDegrees);
            writer.write("unreducedDegrees", unreducedDegrees);
            writer.write("differenceDegrees", angleDifferenceDegrees);
            writer.write("costAtLaunchSiteMetres", angleCostMetres);
            writer.write("shareOfPositionBudgetIfUnreduced",
                         angleCostMetres / kPositionThresholdMetres);
            writer.write("note",
                         "Earth's IAU rate is 360.9856235 deg/day, so 26 years past J2000 the "
                         "unreduced angle is about 3.4e6 degrees and its ULP is worth this much "
                         "arc at the launch site. Removing whole turns before multiplying is "
                         "exact and free. A2 uses the reduced form; the unreduced number is "
                         "measured so the reason is on record.");
            writer.endObject();
            writer.endObject();

            writer.beginObject("summedBudget");
            writer.write("model", "hierarchical-parent-relative, surface anchor, per conversion");
            writer.write("roundTripArithmeticMetres", hierarchicalRoundTripError);
            writer.write("deepestNonRootUlpMetres", earthRelativeUlp);
            writer.write("implementedTotalMetres", implementedConversionBudget);
            writer.write("thresholdMetres", kPositionThresholdMetres);
            writer.write("headroomFactor", kPositionThresholdMetres / implementedConversionBudget);
            writer.write("rotationAngleTermIfReductionDropped", angleCostMetres);
            writer.write("totalIfAngleReductionDropped", budgetIfAngleUnreduced);
            writer.write("excluded",
                         "The datum displacement and the reference-data resolution are excluded "
                         "as definitional offsets of the whole scene rather than errors between "
                         "two conversions of one state. The angle-reduction term is excluded "
                         "because the implementation performs the reduction and therefore never "
                         "pays it; it is carried as the price of dropping it, which is the "
                         "decision the number exists to inform.");
            writer.endObject();

            writer.beginObject("scenarioAllocations");
            writer.write("allocationCount", allocationCounts.allocationCount);
            writer.write("totalAllocatedBytes", allocationCounts.totalAllocatedBytes);
            writer.endObject();

            writer.beginObject("checks");
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "precision-budget.json");
        report.writeToFile(outputPath);
        std::cout << "PrecisionBudget: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("PrecisionBudget");
    } catch (const std::exception& error) {
        std::cerr << "PrecisionBudget: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
