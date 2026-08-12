/// The ADR 0008 reference-data generation record.
///
/// ADR 0008's Validation section asks for three things, and this scenario is each of them:
///   - "a reference-data generation record must reproduce the accepted epoch conversion and
///     body-state fixtures from the pinned kernels";
///   - "round-trip time conversion tests must state their expected precision and include a
///     leap-second-adjacent case";
///   - "the P1 anchor conversion must record geodetic coordinates, elevation datum, body
///     shape/constants, and resulting body-fixed vector".
///
/// It computes nothing about frames. Its output is the provenance layer everything else in
/// increment A2 stands on, emitted so that a reader can check the numbers came from the data
/// they claim to come from.

#include "Sol/Proto/Frames/Ellipsoid.h"
#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/ReferenceData.h"
#include "Sol/Proto/Frames/TimeScales.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ScenarioReport.h"

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

constexpr std::uint64_t kSeed = 0;

void writeVector(JsonWriter& writer, const char* key, const Vec3& value, const char* unit)
{
    writer.beginObject(key);
    writer.write("unit", unit);
    writer.write("x", value.x);
    writer.write("y", value.y);
    writer.write("z", value.z);
    // Bit patterns alongside the decimals: shortest round-trip decimal is lossless, but a
    // one-ULP change is far easier to see in hex than in the seventeenth digit.
    writer.writeBits("xBits", value.x);
    writer.writeBits("yBits", value.y);
    writer.writeBits("zBits", value.z);
    writer.write("magnitude", length(value));
    writer.endObject();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        sol::proto::allocations::reset();
        CheckContext checks;

        const std::filesystem::path fixtureRoot = parseFixtureRoot(argc, argv);
        const ReferenceData data = ReferenceData::loadFromDirectory(fixtureRoot);
        const TimeScales& time = data.timeScales();

        // ---- Epoch conversion -------------------------------------------------------
        const UtcDateTime campaignEpoch{2026, 1, 1, 0, 0, 0.0};
        const double taiMinusUtc = time.taiMinusUtcSeconds(campaignEpoch);
        const TdbEpoch tdb = time.utcToTdb(campaignEpoch);
        const double epochAgreementSeconds =
            tdb.secondsPastJ2000() - data.fixtureEpoch().secondsPastJ2000();

        bool ambiguous = false;
        const UtcDateTime recovered = time.tdbToUtc(tdb, &ambiguous);
        const double roundTripErrorSeconds =
            (nominalJulianDateUtc(recovered) - nominalJulianDateUtc(campaignEpoch))
            * Seconds::kSecondsPerDay;

        // Leap-second-adjacent case, required by ADR 0008 even though the campaign epoch is
        // nowhere near one.
        const UtcDateTime duringLeapSecond{2016, 12, 31, 23, 59, 60.0};
        const UtcDateTime afterLeapSecond{2017, 1, 1, 0, 0, 0.0};
        const double offsetDuringLeap = time.taiMinusUtcSeconds(duringLeapSecond);
        const double offsetAfterLeap = time.taiMinusUtcSeconds(afterLeapSecond);
        const double leapSecondGap = time.utcToTdb(afterLeapSecond).secondsPastJ2000()
                                   - time.utcToTdb(duringLeapSecond).secondsPastJ2000();

        checks.check(std::abs(epochAgreementSeconds) < 1e-4,
                     "the pinned kernel reproduces the epoch the Horizons fixtures were "
                     "generated at, within their printed 0.1 ms resolution");
        checks.check(std::abs(roundTripErrorSeconds) < 1e-6,
                     "UTC -> TDB -> UTC round trips within a microsecond");
        checks.check(offsetDuringLeap == 36.0 && offsetAfterLeap == 37.0,
                     "the leap-second-adjacent case resolves to the correct offsets on each side");
        checks.check(std::abs(leapSecondGap - 1.0) < 1e-6,
                     "the leap second is one SI second wide on the uniform scale");

        // ---- Anchor conversion ------------------------------------------------------
        const Geodetic anchor = ReferenceData::launchAnchor();
        const Ellipsoid& iauEarth = data.earthEllipsoid();
        const Ellipsoid wgs84 = Ellipsoid::wgs84();

        const Vec3 anchorOnIau = iauEarth.toBodyFixed(anchor).metres();
        const Vec3 anchorOnWgs84 = wgs84.toBodyFixed(anchor).metres();
        const double datumDifference = distance(anchorOnIau, anchorOnWgs84);

        int inversionIterations = 0;
        const Geodetic anchorRecovered =
            iauEarth.fromBodyFixed(PositionMetres::fromMetres(anchorOnIau), &inversionIterations);
        const double anchorHeightError = std::abs(anchorRecovered.heightMetres - anchor.heightMetres);
        const double anchorLatitudeErrorMetres =
            std::abs(anchorRecovered.latitude.radians() - anchor.latitude.radians())
            * iauEarth.equatorialRadiusMetres();

        checks.check(anchorHeightError < 1e-6 && anchorLatitudeErrorMetres < 1e-6,
                     "the anchor survives a geodetic round trip to under a micrometre");

        // The datum difference is the point of this comparison, not an error: two defensible
        // readings of "5 m above mean sea level" put the pad in measurably different places.
        checks.check(datumDifference > 1e-3,
                     "the IAU and WGS84 datums place the anchor measurably apart, so the datum "
                     "must be recorded with the coordinate rather than assumed");

        // ---- Emit -------------------------------------------------------------------
        ScenarioMetadata metadata;
        metadata.name = "reference.adr0008Fixtures";
        metadata.version = "1";
        metadata.inputDescription =
            "Pinned NAIF generic kernels (naif0012.tls, pck00011.tpc, gm_de440.tpc) and four JPL "
            "Horizons geometric state vectors relative to the Solar System barycentre in ICRF, "
            "all checksum-verified at load. Fixture root: " + fixtureRoot.generic_string();
        metadata.seed = kSeed;
        metadata.warmupIterations = 0;
        metadata.sampleCount = 1;

        const auto allocationCounts = sol::proto::allocations::snapshot();

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginArray("fixtures");
            for (const FixtureProvenance& record : data.provenance()) {
                writer.beginObject();
                writer.write("file", record.fileName);
                writer.write("role", record.role);
                writer.write("source", record.sourceUrl);
                writer.write("sha256", record.actualSha256);
                writer.write("matchesRecordedDigest", record.actualSha256 == record.expectedSha256);
                writer.endObject();
            }
            writer.endArray();

            writer.beginObject("timeBoundary");
            writer.write("displayedEpochUtc", formatUtc(campaignEpoch));
            writer.write("taiMinusUtcSeconds", taiMinusUtc);
            writer.write("ttMinusTaiSeconds", time.ttMinusTaiSeconds());
            writer.write("tdbMinusTtSeconds", time.tdbMinusTtSeconds(tdb));
            writer.write("tdbMinusUtcSeconds",
                         taiMinusUtc + time.ttMinusTaiSeconds() + time.tdbMinusTtSeconds(tdb));
            writer.write("tdbSecondsPastJ2000", tdb.secondsPastJ2000());
            writer.writeBits("tdbSecondsPastJ2000Bits", tdb.secondsPastJ2000());
            writer.write("tdbJulianDate", tdb.julianDate());
            writer.write("fixtureTdbJulianDate", data.fixtureEpoch().julianDate());
            writer.write("agreementWithFixtureSeconds", epochAgreementSeconds);
            writer.write("expectedPrecisionSeconds", 1e-4);
            writer.write("expectedPrecisionBasis",
                         "Horizons prints its calendar TDB epoch to 0.1 ms; no tighter claim is "
                         "supportable from the fixture alone.");
            writer.write("utcRoundTripErrorSeconds", roundTripErrorSeconds);
            writer.write("leapSecondCount",
                         static_cast<std::uint64_t>(time.leapSeconds().size()));
            writer.write("tdbModel",
                         "SPICE analytic approximation ET = TAI + DELTA_T_A + K sin(E), with "
                         "K, EB, and M taken from the pinned leap-second kernel");
            writer.endObject();

            writer.beginObject("leapSecondAdjacentCase");
            writer.write("instantDuringLeapSecond", formatUtc(duringLeapSecond));
            writer.write("taiMinusUtcDuring", offsetDuringLeap);
            writer.write("instantAfterLeapSecond", formatUtc(afterLeapSecond));
            writer.write("taiMinusUtcAfter", offsetAfterLeap);
            writer.write("separationSeconds", leapSecondGap);
            writer.write("note",
                         "23:59:60 is a real UTC instant that must keep the pre-step offset. "
                         "Treating UTC as a scalar second count reports these two instants as "
                         "simultaneous.");
            writer.endObject();

            writer.beginObject("launchAnchor");
            writer.write("source", "ADR 0008");
            writer.write("latitudeDegrees", anchor.latitude.degrees());
            writer.write("longitudeDegreesEastPositive", anchor.longitude.degrees());
            writer.write("heightMetresAboveEllipsoid", anchor.heightMetres);
            writer.write("elevationDatumNote",
                         "ADR 0008 defines the anchor as 5 m above the reference ellipsoid named "
                         "by BODY399_RADII in the pinned planetary-constants kernel. It formerly "
                         "said 'above mean sea level', which is a geoid statement; the ADR was "
                         "amended after A2 measured that honouring it would require a geoid model "
                         "to resolve a 30 m offset for a fictional facility.");

            writer.beginObject("bodyShape");
            writer.write("name", iauEarth.name());
            writer.write("equatorialRadiusMetres", iauEarth.equatorialRadiusMetres());
            writer.write("polarRadiusMetres", iauEarth.polarRadiusMetres());
            writer.write("inverseFlattening", iauEarth.inverseFlattening());
            writer.write("firstEccentricitySquared", iauEarth.firstEccentricitySquared());
            writer.endObject();

            writeVector(writer, "bodyFixedVector", anchorOnIau, "m");
            writer.write("inversionIterations", static_cast<std::uint64_t>(inversionIterations));
            writer.write("roundTripHeightErrorMetres", anchorHeightError);
            writer.write("roundTripLatitudeErrorMetres", anchorLatitudeErrorMetres);

            writer.beginObject("datumComparison");
            writer.write("alternativeName", wgs84.name());
            writer.write("alternativeEquatorialRadiusMetres", wgs84.equatorialRadiusMetres());
            writer.write("alternativeInverseFlattening", wgs84.inverseFlattening());
            writeVector(writer, "alternativeBodyFixedVector", anchorOnWgs84, "m");
            writer.write("displacementMetres", datumDifference);
            writer.write("note",
                         "The two ellipsoids differ by 0.4 m in equatorial radius, which is 400 "
                         "times A2's whole millimetre position budget. The pinned IAU kernel "
                         "value is used because it arrives with a checksum from the source ADR "
                         "0008 names; this measurement is why that ADR now requires a datum to "
                         "travel with every geodetic coordinate.");
            writer.endObject();
            writer.endObject();

            writer.beginArray("bodyStates");
            for (const BodyStateFixture& state : data.bodyStates()) {
                writer.beginObject();
                writer.write("body", state.bodyName);
                writer.write("naifId", static_cast<std::int64_t>(state.naifId));
                writer.write("centre", state.centreName);
                writer.write("referenceFrame", state.referenceFrame);
                writer.write("timeScale", "TDB");
                writer.write("calendarTdb", state.calendarTdb);
                writer.write("tdbJulianDate", state.epoch.julianDate());
                writer.write("ephemerisSource", state.ephemerisSource);
                writer.write("printedSignificantDigits",
                             static_cast<std::int64_t>(state.printedSignificantDigits));
                writeVector(writer, "position", state.position.metres(), "m");
                writeVector(writer, "velocity", state.velocity.metresPerSecond(), "m/s");
                writer.endObject();
            }
            writer.endArray();

            writer.beginObject("gravitationalParameters");
            writer.write("unit", "m^3/s^2");
            writer.write("sun", data.gravitationalParameter(10));
            writer.write("earthMoonBarycentre", data.gravitationalParameter(3));
            writer.write("earth", data.gravitationalParameter(399));
            writer.write("moon", data.gravitationalParameter(301));
            writer.write("note",
                         "Loaded for A3's use under ADR 0011. A2 performs no gravitational "
                         "computation.");
            writer.endObject();

            writer.beginObject("earthOrientation");
            writer.write("model", "IAU_EARTH from pck00011 BODY399_POLE_RA/POLE_DEC/PM");
            writer.write("poleRightAscensionDegreesAtJ2000", data.earthPoleRightAscension()[0]);
            writer.write("poleRightAscensionDegreesPerCentury", data.earthPoleRightAscension()[1]);
            writer.write("poleDeclinationDegreesAtJ2000", data.earthPoleDeclination()[0]);
            writer.write("poleDeclinationDegreesPerCentury", data.earthPoleDeclination()[1]);
            writer.write("primeMeridianDegreesAtJ2000", data.earthPrimeMeridian()[0]);
            writer.write("primeMeridianDegreesPerDay", data.earthPrimeMeridian()[1]);
            writer.write("accuracyNote",
                         "IAU_EARTH omits nutation, polar motion, and UT1-UTC, so it can differ "
                         "from ITRF by tens of metres at the surface. A2 measures the numerical "
                         "behaviour of a rotating-frame boundary, not Earth's true attitude. The "
                         "production Earth orientation model remains an open decision.");
            writer.endObject();

            writer.beginObject("scenarioAllocations");
            writer.write("allocationCount", allocationCounts.allocationCount);
            writer.write("totalAllocatedBytes", allocationCounts.totalAllocatedBytes);
            writer.write("note",
                         "Fixture loading allocates by design; this is a provenance scenario, not "
                         "a measured region.");
            writer.endObject();

            writer.beginObject("checks");
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "reference-fixtures.json");
        report.writeToFile(outputPath);
        std::cout << "ReferenceFixtures: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("ReferenceFixtures");
    } catch (const std::exception& error) {
        std::cerr << "ReferenceFixtures: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
