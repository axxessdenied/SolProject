#include "Sol/Proto/Frames/FrameGraph.h"

namespace sol::proto::frames {
namespace {

/// Origin state of a celestial frame at @p epoch, extrapolated from its fixture.
/// See OriginMotionModel for what this is and, more importantly, what it is not.
struct OriginState {
    Vec3 position{};
    Vec3 velocity{};
};

[[nodiscard]] OriginState extrapolate(const BodyStateFixture& fixture, TdbEpoch epoch) noexcept
{
    const double elapsed = epoch.secondsPastJ2000() - fixture.epoch.secondsPastJ2000();
    OriginState state;
    state.velocity = fixture.velocity.metresPerSecond();
    state.position = fixture.position.metres() + state.velocity * elapsed;
    return state;
}

} // namespace

Vec3 launchAnchorBodyFixed(const ReferenceData& data)
{
    return data.earthEllipsoid().toBodyFixed(ReferenceData::launchAnchor()).metres();
}

Mat3 launchSiteEnuRotation(const ReferenceData& data)
{
    const Geodetic anchor = ReferenceData::launchAnchor();
    const Ellipsoid& ellipsoid = data.earthEllipsoid();

    const Vec3 east = ellipsoid.eastAxis(anchor);
    const Vec3 north = ellipsoid.northAxis(anchor);
    const Vec3 up = ellipsoid.upAxis(anchor);

    // Rows are the basis vectors, so multiplying by a body-fixed vector projects it onto
    // east, north, and up in turn.
    Mat3 rotation;
    rotation.m[0][0] = east.x;  rotation.m[0][1] = east.y;  rotation.m[0][2] = east.z;
    rotation.m[1][0] = north.x; rotation.m[1][1] = north.y; rotation.m[1][2] = north.z;
    rotation.m[2][0] = up.x;    rotation.m[2][1] = up.y;    rotation.m[2][2] = up.z;
    return rotation;
}

FrameGraphSnapshot buildSnapshot(const ReferenceData& data, TdbEpoch epoch,
                                 const VehicleState& vehicle)
{
    FrameGraphSnapshot snapshot;
    snapshot.epoch = epoch;

    const OriginState sun = extrapolate(data.bodyState(10), epoch);
    const OriginState earthMoonBarycentre = extrapolate(data.bodyState(3), epoch);
    const OriginState earth = extrapolate(data.bodyState(399), epoch);
    const OriginState moon = extrapolate(data.bodyState(301), epoch);

    const auto index = [](FrameId frame) { return static_cast<std::size_t>(frame); };

    // Root: identity against itself.
    snapshot.parentToFrame[index(FrameId::SsbIcrf)] = FrameTransform{};

    FrameTransform& sunTransform = snapshot.parentToFrame[index(FrameId::SunIcrf)];
    sunTransform.originInParent = sun.position;
    sunTransform.originVelocityInParent = sun.velocity;

    FrameTransform& barycentreTransform =
        snapshot.parentToFrame[index(FrameId::EarthMoonBarycentreIcrf)];
    barycentreTransform.originInParent = earthMoonBarycentre.position;
    barycentreTransform.originVelocityInParent = earthMoonBarycentre.velocity;

    // Earth and Moon are stored against the Earth-Moon barycentre, which is what makes the
    // hierarchical model's advantage real rather than nominal: these offsets are 4.7e6 m and
    // 3.8e8 m, not 1.5e11 m.
    FrameTransform& earthTransform = snapshot.parentToFrame[index(FrameId::EarthIcrf)];
    earthTransform.originInParent = earth.position - earthMoonBarycentre.position;
    earthTransform.originVelocityInParent = earth.velocity - earthMoonBarycentre.velocity;

    FrameTransform& moonTransform = snapshot.parentToFrame[index(FrameId::MoonIcrf)];
    moonTransform.originInParent = moon.position - earthMoonBarycentre.position;
    moonTransform.originVelocityInParent = moon.velocity - earthMoonBarycentre.velocity;

    snapshot.parentToFrame[index(FrameId::EarthBodyFixed)] =
        earthBodyFixedTransform(epoch, data.earthPoleRightAscension(), data.earthPoleDeclination(),
                                data.earthPrimeMeridian());

    FrameTransform& launchSiteTransform = snapshot.parentToFrame[index(FrameId::LaunchSiteEnu)];
    launchSiteTransform.originInParent = launchAnchorBodyFixed(data);
    launchSiteTransform.originVelocityInParent = Vec3{};
    launchSiteTransform.rotation = launchSiteEnuRotation(data);
    // The launch site is fixed in the body-fixed frame, so this boundary contributes no
    // rotation rate of its own. Earth's rotation enters one boundary lower, exactly once.
    launchSiteTransform.rotationRate = zeroMat3();

    FrameTransform& vehicleTransform = snapshot.parentToFrame[index(FrameId::VehicleLocal)];
    vehicleTransform.originInParent = vehicle.positionInEnu;
    vehicleTransform.originVelocityInParent = vehicle.velocityInEnu;
    vehicleTransform.rotation = identityMat3();
    vehicleTransform.rotationRate = zeroMat3();

    return snapshot;
}

} // namespace sol::proto::frames
