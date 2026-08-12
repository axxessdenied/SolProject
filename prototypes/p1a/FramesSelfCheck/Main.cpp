/// Guards the A2 frame library.
///
/// A1 learned this the hard way: its JSON writer silently wrote every string as `true` while
/// the build and all tests passed, and its first contraction probe did not probe contraction.
/// The lesson recorded in that handoff was that an increment's own instruments need testing
/// before their output can be trusted. This target is A2 acting on it.
///
/// Everything checked here is a property that would otherwise be *assumed* by the measurement
/// scenarios: that the kernel parser reads adopted constants rather than documentation
/// examples, that the time boundary reproduces an independently published epoch, that a
/// transform's inverse is its inverse, and that the two candidate models describe the same
/// geometry. If any of these is wrong, every number A2 reports is wrong in a way that looks
/// entirely plausible.
///
/// Tolerances here are absolute and physical, never bitwise. ADR 0010 guarantees bit-exactness
/// only for the same build on the same machine, and this target runs in both Debug and Release.

#include "Sol/Proto/Frames/AscentProfile.h"
#include "Sol/Proto/Frames/FlatFrameModel.h"
#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/HierarchicalFrameModel.h"
#include "Sol/Proto/Frames/ReferenceData.h"
#include "Sol/Proto/Frames/Sha256.h"
#include "Sol/Proto/Frames/TextKernel.h"
#include "Sol/Proto/Frames/TimeScales.h"

#include "Sol/Proto/Harness/Check.h"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

using namespace sol::proto;
using namespace sol::proto::frames;

void checkSha256(CheckContext& checks)
{
    // Published NIST/FIPS-180-4 test vectors. Without these the digest verification in
    // ReferenceData would be self-consistent nonsense: a broken hash still matches itself.
    checks.check(sha256Hex(std::string_view{""})
                     == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 "SHA-256 of the empty string matches the published vector");
    checks.check(sha256Hex(std::string_view{"abc"})
                     == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "SHA-256 of \"abc\" matches the published vector");
    checks.check(sha256Hex(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})
                     == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                 "SHA-256 of the 56-byte vector matches, exercising the length-field padding path");

    // 1,000,000 'a' characters: the standard multi-block vector. It is the only one here that
    // crosses the 2^32-bit boundary handling in the length field.
    std::string million(1000000, 'a');
    checks.check(sha256Hex(std::string_view{million})
                     == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                 "SHA-256 of one million 'a' matches the published vector");
}

void checkKernelParsing(CheckContext& checks, const std::filesystem::path& fixtureRoot)
{
    const TextKernel planetaryConstants =
        TextKernel::loadFromFile(fixtureRoot / "kernels/pck00011.tpc");

    // pck00011.tpc assigns BODY399_RADII four times: three of them are worked examples in the
    // documentation preamble, and only the fourth is inside a \begindata block. A parser that
    // greps for the keyword picks up an example. This check is the reason the parser honours
    // data blocks at all.
    const std::vector<double> radii = planetaryConstants.numbers("BODY399_RADII", 3);
    checks.check(radii[0] == 6378.1366 && radii[1] == 6378.1366 && radii[2] == 6356.7519,
                 "BODY399_RADII comes from the data block, not a documentation example");

    const std::vector<double> primeMeridian = planetaryConstants.numbers("BODY399_PM", 3);
    checks.check(primeMeridian[0] == 190.147 && primeMeridian[1] == 360.9856235
                     && primeMeridian[2] == 0.0,
                 "BODY399_PM parses to the adopted IAU coefficients");

    const TextKernel gravitational = TextKernel::loadFromFile(fixtureRoot / "kernels/gm_de440.tpc");
    // Written with a Fortran 'D' exponent in the kernel; a parser that does not handle it
    // would throw rather than mis-read, but the check pins the behaviour either way.
    checks.check(std::abs(gravitational.number("BODY399_GM") - 3.9860043550702266e5) < 1e-9,
                 "BODY399_GM parses through the Fortran D-exponent form");
}

void checkTimeScales(CheckContext& checks, const ReferenceData& data)
{
    const TimeScales& time = data.timeScales();

    checks.check(time.leapSeconds().size() == 28,
                 "naif0012.tls supplies 28 DELTA_AT entries");
    checks.check(time.leapSeconds().back().taiMinusUtcSeconds == 37.0,
                 "the final DELTA_AT entry is 37 s");
    checks.check(time.ttMinusTaiSeconds() == 32.184, "TT - TAI is the kernel's 32.184 s");

    // ADR 0008's campaign epoch, converted through the pinned kernel, must land on the epoch
    // the Horizons fixtures were generated at. This is the increment's single most important
    // cross-check: it validates the time boundary against an independent implementation of the
    // same standard, using data neither side derived from the other.
    const UtcDateTime campaignEpoch{2026, 1, 1, 0, 0, 0.0};
    const TdbEpoch converted = time.utcToTdb(campaignEpoch);
    const double differenceSeconds =
        converted.secondsPastJ2000() - data.fixtureEpoch().secondsPastJ2000();
    // Horizons printed its epoch to 0.1 ms, so 0.1 ms is the tightest claim the comparison can
    // support. Anything larger would mean the leap-second or TDB model disagrees.
    checks.check(std::abs(differenceSeconds) < 1e-4,
                 "UTC 2026-01-01T00:00:00 converts to the Horizons fixture epoch within 0.1 ms");
    if (std::abs(differenceSeconds) >= 1e-4) {
        std::cerr << "    measured difference: " << differenceSeconds << " s\n";
    }

    checks.check(std::abs(time.taiMinusUtcSeconds(campaignEpoch) - 37.0) < 1e-12,
                 "TAI - UTC at the campaign epoch is 37 s");

    // ADR 0008 requires a leap-second-adjacent case even though the campaign epoch is not near
    // one. 2016-12-31T23:59:60Z is a real UTC instant and must keep the pre-step offset.
    const UtcDateTime leapInstant{2016, 12, 31, 23, 59, 60.0};
    const UtcDateTime afterLeap{2017, 1, 1, 0, 0, 0.0};
    checks.check(std::abs(time.taiMinusUtcSeconds(leapInstant) - 36.0) < 1e-12,
                 "TAI - UTC during the 2016 leap second is still 36 s");
    checks.check(std::abs(time.taiMinusUtcSeconds(afterLeap) - 37.0) < 1e-12,
                 "TAI - UTC immediately after the 2016 leap second is 37 s");

    // The leap second and the instant after it are one SI second apart on a uniform scale.
    // Getting this wrong by treating UTC as a scalar count of seconds gives zero.
    const double leapGap = time.utcToTdb(afterLeap).secondsPastJ2000()
                         - time.utcToTdb(leapInstant).secondsPastJ2000();
    checks.check(std::abs(leapGap - 1.0) < 1e-6,
                 "the leap second and the instant after it are 1 s apart on the TDB scale");

    // Round trip, away from a leap second.
    bool ambiguous = false;
    const UtcDateTime recovered = time.tdbToUtc(converted, &ambiguous);
    const double roundTripError =
        std::abs(nominalJulianDateUtc(recovered) - nominalJulianDateUtc(campaignEpoch))
        * Seconds::kSecondsPerDay;
    checks.check(!ambiguous, "the campaign epoch is not flagged leap-second ambiguous");
    checks.check(roundTripError < 1e-6,
                 "UTC -> TDB -> UTC returns the campaign epoch within a microsecond");
}

void checkGeodetic(CheckContext& checks, const ReferenceData& data)
{
    const Ellipsoid& ellipsoid = data.earthEllipsoid();

    struct TestPoint {
        double latitudeDegrees;
        double longitudeDegrees;
        double heightMetres;
        const char* label;
    };
    // The last two sit where each height branch is ill-conditioned, which is exactly where a
    // conversion that only ever gets tested at the launch site would fail unnoticed.
    constexpr TestPoint kPoints[]{
        {28.0, -80.5, 5.0, "ADR 0008 launch anchor"},
        {0.0, 0.0, 0.0, "equator, prime meridian, on the ellipsoid"},
        {45.0, 100.0, 200000.0, "mid-latitude at 200 km"},
        {89.9, -12.0, 1000.0, "near the north pole"},
        {-89.9, 170.0, -400.0, "near the south pole, below the ellipsoid"},
    };

    for (const TestPoint& point : kPoints) {
        Geodetic original;
        original.latitude = Radians::fromDegrees(point.latitudeDegrees);
        original.longitude = Radians::fromDegrees(point.longitudeDegrees);
        original.heightMetres = point.heightMetres;

        int iterations = 0;
        const PositionMetres bodyFixed = ellipsoid.toBodyFixed(original);
        const Geodetic recovered = ellipsoid.fromBodyFixed(bodyFixed, &iterations);

        // One nanoradian of latitude is 6.4 mm, so the angular tolerance is set from the
        // position budget rather than picked for looking small.
        const double latitudeErrorMetres =
            std::abs(recovered.latitude.radians() - original.latitude.radians())
            * ellipsoid.equatorialRadiusMetres();
        const double heightError = std::abs(recovered.heightMetres - original.heightMetres);

        checks.check(latitudeErrorMetres < 1e-6,
                     std::string{"geodetic round trip preserves latitude at "} + point.label);
        checks.check(heightError < 1e-6,
                     std::string{"geodetic round trip preserves height at "} + point.label);
        checks.check(iterations <= 12,
                     std::string{"geodetic inversion converges at "} + point.label);
    }

    // The topocentric basis must be orthonormal, or every launch-site conversion is skewed.
    const Geodetic anchor = ReferenceData::launchAnchor();
    const Vec3 east = ellipsoid.eastAxis(anchor);
    const Vec3 north = ellipsoid.northAxis(anchor);
    const Vec3 up = ellipsoid.upAxis(anchor);
    checks.check(std::abs(length(east) - 1.0) < 1e-15, "the east axis is a unit vector");
    checks.check(std::abs(length(north) - 1.0) < 1e-15, "the north axis is a unit vector");
    checks.check(std::abs(length(up) - 1.0) < 1e-15, "the up axis is a unit vector");
    checks.check(std::abs(dot(east, north)) < 1e-15, "east and north are orthogonal");
    checks.check(std::abs(dot(east, up)) < 1e-15, "east and up are orthogonal");
    checks.check(std::abs(dot(north, up)) < 1e-15, "north and up are orthogonal");
    checks.check(length(cross(east, north) - up) < 1e-15,
                 "east x north = up, so the basis is right-handed");

    // The anchor's up axis must be the ellipsoid normal, which is what makes the height in
    // ADR 0008 mean what it says.
    const Vec3 atSurface =
        ellipsoid.toBodyFixed(Geodetic{anchor.latitude, anchor.longitude, 0.0}).metres();
    const Vec3 atHeight = ellipsoid.toBodyFixed(anchor).metres();
    const Vec3 offset = atHeight - atSurface;
    checks.check(std::abs(length(offset) - anchor.heightMetres) < 1e-9,
                 "raising the anchor by its height moves it by exactly that distance");
    // The offset is analytically exactly h * up, but recovering a 5 m vector by differencing
    // two 6.37e6 m vectors cancels away all but the last few bits: one ULP at that magnitude
    // is 0.93 nm, so the direction is only good to about 2e-10 relative. That is nanometres of
    // absolute error and irrelevant to the millimetre budget -- but it is the first place in
    // this increment where forming a small quantity from two large ones costs real precision,
    // and the tolerance is set from that arithmetic rather than from optimism.
    checks.check(length(normalized(offset) - up) < 1e-9,
                 "the height offset is along the ellipsoid normal");
}

void checkTransformAlgebra(CheckContext& checks, const ReferenceData& data)
{
    const TdbEpoch epoch = data.fixtureEpoch();
    VehicleState vehicle;
    vehicle.positionInEnu = Vec3{12345.0, -6789.0, 54321.0};
    vehicle.velocityInEnu = Vec3{1234.5, -678.9, 543.2};
    const FrameGraphSnapshot snapshot = buildSnapshot(data, epoch, vehicle);

    // Every rotation in the graph must be orthonormal, including the composed Earth rotation.
    for (std::size_t i = 0; i < kFrameCount; ++i) {
        const double error = orthonormalityError(snapshot.parentToFrame[i].rotation);
        checks.check(error < 1e-14,
                     std::string{"rotation into "} + std::string{frameName(static_cast<FrameId>(i))}
                         + " is orthonormal");
    }

    // toParent must be the exact inverse of toChild, boundary by boundary.
    for (std::size_t i = 1; i < kFrameCount; ++i) {
        const FrameId frame = static_cast<FrameId>(i);
        const FrameId parent = parentFrame(frame);

        StateVector parentState;
        parentState.position = PositionMetres::fromMetres(Vec3{1.0e6, -2.0e6, 3.0e6});
        parentState.velocity = VelocityMetresPerSecond::fromMetresPerSecond(Vec3{100.0, -200.0, 300.0});
        parentState.frame = parent;
        parentState.epoch = epoch;

        const StateVector childState = toChild(snapshot.parentToFrame[i], parentState, frame);
        const StateVector recovered = toParent(snapshot.parentToFrame[i], childState, parent);

        const double positionError =
            distance(recovered.position.metres(), parentState.position.metres());
        const double velocityError =
            distance(recovered.velocity.metresPerSecond(), parentState.velocity.metresPerSecond());
        checks.check(positionError < 1e-6,
                     std::string{"toChild/toParent round trip preserves position at the "}
                         + std::string{frameName(frame)} + " boundary");
        checks.check(velocityError < 1e-9,
                     std::string{"toChild/toParent round trip preserves velocity at the "}
                         + std::string{frameName(frame)} + " boundary");
    }

    // compose() must equal applying the two transforms in sequence. The flat model is built
    // entirely out of compose(), so an error here would appear as a "model difference" that is
    // really a bug in one line of algebra.
    const std::size_t earthIndex = static_cast<std::size_t>(FrameId::EarthIcrf);
    const std::size_t bodyFixedIndex = static_cast<std::size_t>(FrameId::EarthBodyFixed);
    const FrameTransform composed =
        compose(snapshot.parentToFrame[earthIndex], snapshot.parentToFrame[bodyFixedIndex]);

    StateVector barycentric;
    barycentric.position = PositionMetres::fromMetres(Vec3{4.0e6, 5.0e6, -3.0e6});
    barycentric.velocity = VelocityMetresPerSecond::fromMetresPerSecond(Vec3{-500.0, 700.0, 100.0});
    barycentric.frame = FrameId::EarthMoonBarycentreIcrf;
    barycentric.epoch = epoch;

    const StateVector sequential =
        toChild(snapshot.parentToFrame[bodyFixedIndex],
                toChild(snapshot.parentToFrame[earthIndex], barycentric, FrameId::EarthIcrf),
                FrameId::EarthBodyFixed);
    const StateVector viaCompose = toChild(composed, barycentric, FrameId::EarthBodyFixed);

    checks.check(distance(sequential.position.metres(), viaCompose.position.metres()) < 1e-6,
                 "compose() reproduces sequential application for position");
    checks.check(distance(sequential.velocity.metresPerSecond(),
                          viaCompose.velocity.metresPerSecond()) < 1e-9,
                 "compose() reproduces sequential application for velocity");

    // The rotation rate must be the actual derivative of the rotation. Checked by central
    // difference against the analytic matrix, because an omega term that is merely plausible
    // produces velocity errors of hundreds of m/s that no round trip would reveal -- a round
    // trip through a wrong-but-consistent transform still returns to its input.
    constexpr double kStepSeconds = 1.0;
    const FrameTransform before =
        earthBodyFixedTransform(epoch.advancedBy(Seconds::fromSeconds(-kStepSeconds)),
                                data.earthPoleRightAscension(), data.earthPoleDeclination(),
                                data.earthPrimeMeridian());
    const FrameTransform after =
        earthBodyFixedTransform(epoch.advancedBy(Seconds::fromSeconds(kStepSeconds)),
                                data.earthPoleRightAscension(), data.earthPoleDeclination(),
                                data.earthPrimeMeridian());
    const FrameTransform& analytic = snapshot.parentToFrame[bodyFixedIndex];

    double worstRateError = 0.0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const double numeric =
                (after.rotation.m[row][column] - before.rotation.m[row][column])
                / (2.0 * kStepSeconds);
            worstRateError =
                std::max(worstRateError, std::abs(numeric - analytic.rotationRate.m[row][column]));
        }
    }
    // Central-difference truncation error is O(h^2 * omega^3) ~ 4e-13 at h = 1 s, so this
    // tolerance is set by the check's own accuracy, not by the transform's.
    checks.check(worstRateError < 1e-11,
                 "the Earth rotation rate matrix matches a central difference of the rotation");
    if (worstRateError >= 1e-11) {
        std::cerr << "    worst rotation-rate discrepancy: " << worstRateError << " 1/s\n";
    }
}

void checkModelsAgree(CheckContext& checks, const ReferenceData& data)
{
    const TdbEpoch epoch = data.fixtureEpoch();
    VehicleState vehicle;
    vehicle.positionInEnu = Vec3{250000.0, 10000.0, 120000.0};
    vehicle.velocityInEnu = Vec3{3000.0, 50.0, 900.0};
    const FrameGraphSnapshot snapshot = buildSnapshot(data, epoch, vehicle);

    const FlatFrameModel flat{snapshot};
    const HierarchicalFrameModel hierarchical{snapshot};

    StateVector local;
    local.position = PositionMetres::fromMetres(Vec3{10.0, -4.0, 3.0});
    local.velocity = VelocityMetresPerSecond::fromMetresPerSecond(Vec3{1.0, 2.0, -3.0});
    local.frame = FrameId::VehicleLocal;
    local.epoch = epoch;

    // The two models must describe the same geometry. They are built from one snapshot and
    // differ only in where the arithmetic happens, so any disagreement is numerical -- and at
    // barycentric magnitude, "numerical" is expected to be tens of micrometres, not zero.
    for (std::size_t i = 0; i < kFrameCount; ++i) {
        const FrameId target = static_cast<FrameId>(i);
        const StateVector viaFlat = flat.convert(local, target);
        const StateVector viaHierarchical = hierarchical.convert(local, target);
        const double positionDifference =
            distance(viaFlat.position.metres(), viaHierarchical.position.metres());
        const double velocityDifference = distance(viaFlat.velocity.metresPerSecond(),
                                                   viaHierarchical.velocity.metresPerSecond());
        checks.check(positionDifference < 1e-3,
                     std::string{"both models place the state in "}
                         + std::string{frameName(target)} + " within a millimetre");
        checks.check(velocityDifference < 1e-6,
                     std::string{"both models agree on velocity in "}
                         + std::string{frameName(target)} + " within a micrometre per second");
    }

    // Frame identity must be preserved, and the application counts must match the documented
    // path lengths.
    checks.check(flat.convert(local, FrameId::SsbIcrf).frame == FrameId::SsbIcrf,
                 "the flat model tags its output with the target frame");
    checks.check(FlatFrameModel::transformApplications(FrameId::VehicleLocal,
                                                       FrameId::LaunchSiteEnu) == 2,
                 "the flat model needs two applications even between adjacent frames");
    checks.check(HierarchicalFrameModel::transformApplications(FrameId::VehicleLocal,
                                                               FrameId::LaunchSiteEnu) == 1,
                 "the hierarchical model needs one application between adjacent frames");
    // Four applications up from the vehicle to the Earth-Moon barycentre, one back down to the
    // Moon. Notably this is *more* work than the flat model's two, which is the trade the cost
    // scenario exists to quantify rather than assume away.
    checks.check(HierarchicalFrameModel::transformApplications(FrameId::VehicleLocal,
                                                               FrameId::MoonIcrf) == 5,
                 "the hierarchical model crosses the Earth-Moon barycentre to reach the Moon");
    checks.check(HierarchicalFrameModel::lowestCommonAncestor(FrameId::EarthIcrf,
                                                              FrameId::MoonIcrf)
                     == FrameId::EarthMoonBarycentreIcrf,
                 "the lowest common ancestor of Earth and the Moon is their barycentre");

    // A state tagged with the wrong epoch must be rejected rather than silently converted.
    StateVector wrongEpoch = local;
    wrongEpoch.epoch = epoch.advancedBy(Seconds::fromSeconds(1.0));
    bool rejected = false;
    try {
        (void)flat.convert(wrongEpoch, FrameId::SsbIcrf);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    checks.check(rejected, "converting a state whose epoch differs from the snapshot is rejected");
}

void checkAscentProfile(CheckContext& checks)
{
    constexpr double kDuration = 500.0;
    constexpr double kFinalAltitude = 200000.0;
    constexpr double kFinalSpeed = 7784.0;
    const AscentProfile profile{kDuration, kFinalAltitude, kFinalSpeed,
                                Radians::fromDegrees(90.0)};

    const VehicleState atLiftoff = profile.sample(0.0);
    checks.check(length(atLiftoff.positionInEnu) == 0.0, "the ascent starts at the launch site");
    checks.check(length(atLiftoff.velocityInEnu) == 0.0, "the ascent starts at rest");

    const VehicleState atInsertion = profile.sample(kDuration);
    checks.check(std::abs(atInsertion.positionInEnu.z - kFinalAltitude) < 1e-6,
                 "the ascent reaches its stated altitude");
    checks.check(std::abs(atInsertion.velocityInEnu.z) < 1e-9,
                 "vertical speed is zero at insertion");
    checks.check(std::abs(length(atInsertion.velocityInEnu) - kFinalSpeed) < 1e-6,
                 "horizontal speed at insertion is the stated circular velocity");
    checks.check(std::abs(atInsertion.velocityInEnu.y) < 1e-6,
                 "a 90 degree azimuth flies due east");

    // Velocity must be the exact derivative of position, or every conversion measured along
    // the trajectory is testing an inconsistent state.
    constexpr double kStep = 1e-3;
    double worstError = 0.0;
    for (int i = 1; i < 20; ++i) {
        const double t = kDuration * static_cast<double>(i) / 20.0;
        const Vec3 forward = profile.sample(t + kStep).positionInEnu;
        const Vec3 backward = profile.sample(t - kStep).positionInEnu;
        const Vec3 numeric = (forward - backward) * (1.0 / (2.0 * kStep));
        worstError = std::max(worstError, distance(numeric, profile.sample(t).velocityInEnu));
    }
    checks.check(worstError < 1e-3,
                 "the profile's velocity matches a central difference of its position");
}

} // namespace

int main(int argc, char** argv)
{
    CheckContext checks;
    try {
        const std::filesystem::path fixtureRoot = parseFixtureRoot(argc, argv);
        const ReferenceData data = ReferenceData::loadFromDirectory(fixtureRoot);

        checkSha256(checks);
        checkKernelParsing(checks, fixtureRoot);
        checkTimeScales(checks, data);
        checkGeodetic(checks, data);
        checkTransformAlgebra(checks, data);
        checkModelsAgree(checks, data);
        checkAscentProfile(checks);
    } catch (const std::exception& error) {
        std::cerr << "FramesSelfCheck: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return checks.summarize("FramesSelfCheck");
}
