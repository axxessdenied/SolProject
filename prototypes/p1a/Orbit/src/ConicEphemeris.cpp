#include "Sol/Proto/Orbit/ConicEphemeris.h"

#include "Sol/Proto/Orbit/KeplerPropagator.h"

#include <stdexcept>
#include <string>

namespace sol::proto::orbit {
namespace {

using frames::BodyStateFixture;
using frames::FrameId;
using frames::FrameTransform;
using frames::ReferenceData;
using frames::TdbEpoch;
using frames::Vec3;

[[nodiscard]] TwoBodyState fixtureState(const BodyStateFixture& fixture) noexcept
{
    return TwoBodyState{fixture.position.metres(), fixture.velocity.metresPerSecond()};
}

[[nodiscard]] TwoBodyState difference(const TwoBodyState& a, const TwoBodyState& b) noexcept
{
    return TwoBodyState{a.position - b.position, a.velocity - b.velocity};
}

[[nodiscard]] TwoBodyState sum(const TwoBodyState& a, const TwoBodyState& b) noexcept
{
    return TwoBodyState{a.position + b.position, a.velocity + b.velocity};
}

[[nodiscard]] TwoBodyState scaled(const TwoBodyState& state, double factor) noexcept
{
    return TwoBodyState{state.position * factor, state.velocity * factor};
}

/// Linear extrapolation, exactly as A2's frames::buildSnapshot performs it.
[[nodiscard]] TwoBodyState extrapolateLinear(const TwoBodyState& anchor,
                                             double elapsedSeconds) noexcept
{
    return TwoBodyState{anchor.position + anchor.velocity * elapsedSeconds, anchor.velocity};
}

} // namespace

ConicEphemeris ConicEphemeris::fromReferenceData(const ReferenceData& data,
                                                 const BodySystem& system)
{
    ConicEphemeris ephemeris;
    ephemeris.m_anchorEpoch = data.fixtureEpoch();

    ephemeris.m_linearSun = fixtureState(data.bodyState(kNaifSun));
    ephemeris.m_linearBarycentre = fixtureState(data.bodyState(kNaifEarthMoonBarycentre));
    ephemeris.m_linearEarth = fixtureState(data.bodyState(kNaifEarth));
    ephemeris.m_linearMoon = fixtureState(data.bodyState(kNaifMoon));

    const double muSun = system.body(kNaifSun).gravitationalParameter;
    const double muEarth = system.body(kNaifEarth).gravitationalParameter;
    const double muMoon = system.body(kNaifMoon).gravitationalParameter;

    ephemeris.m_muSun = muSun;
    ephemeris.m_muEarthMoonSystem = muEarth + muMoon;
    ephemeris.m_muSunSystem = muSun + muEarth + muMoon;
    ephemeris.m_moonMassFraction = system.moonMassFraction();

    ephemeris.m_sunAboutBarycentre = ephemeris.m_linearSun;
    ephemeris.m_barycentreAboutSun =
        difference(ephemeris.m_linearBarycentre, ephemeris.m_linearSun);
    ephemeris.m_moonAboutEarth = difference(ephemeris.m_linearMoon, ephemeris.m_linearEarth);

    return ephemeris;
}

TwoBodyState ConicEphemeris::stateAboutPrimary(int naifId, TdbEpoch epoch) const
{
    const double elapsed = epoch.secondsPastJ2000() - m_anchorEpoch.secondsPastJ2000();

    switch (naifId) {
    case kNaifSun:
        // The root has no primary. Returning a zero state is the correct answer to "where is the
        // Sun relative to the thing it orbits", because under ADR 0011 it does not orbit
        // anything.
        return TwoBodyState{};
    case kNaifEarth:
        return propagateKepler(m_barycentreAboutSun, m_muSunSystem, elapsed).state;
    case kNaifMoon:
        return propagateKepler(m_moonAboutEarth, m_muEarthMoonSystem, elapsed).state;
    default:
        break;
    }
    throw std::runtime_error("ConicEphemeris: no conic for NAIF id " + std::to_string(naifId));
}

TwoBodyState ConicEphemeris::barycentricState(int naifId, TdbEpoch epoch) const
{
    const double elapsed = epoch.secondsPastJ2000() - m_anchorEpoch.secondsPastJ2000();

    // The one term outside the model. See the class comment.
    const TwoBodyState sun = extrapolateLinear(m_sunAboutBarycentre, elapsed);
    if (naifId == kNaifSun) {
        return sun;
    }

    const TwoBodyState barycentreAboutSun =
        propagateKepler(m_barycentreAboutSun, m_muSunSystem, elapsed).state;
    const TwoBodyState barycentre = sum(sun, barycentreAboutSun);
    if (naifId == kNaifEarthMoonBarycentre) {
        return barycentre;
    }

    // The Earth-Moon pair, split about their common barycentre by mass ratio. This is where the
    // gravitational hierarchy and the frame hierarchy are reconciled: the conic is the Moon's
    // orbit about Earth, and the barycentre placement follows from it rather than being an
    // independent fixture that could drift out of agreement with it.
    const TwoBodyState moonAboutEarth =
        propagateKepler(m_moonAboutEarth, m_muEarthMoonSystem, elapsed).state;

    if (naifId == kNaifEarth) {
        return sum(barycentre, scaled(moonAboutEarth, -m_moonMassFraction));
    }
    if (naifId == kNaifMoon) {
        return sum(barycentre, scaled(moonAboutEarth, 1.0 - m_moonMassFraction));
    }

    throw std::runtime_error("ConicEphemeris: no conic for NAIF id " + std::to_string(naifId));
}

TwoBodyState ConicEphemeris::stateRelativeTo(int naifId, int centreNaifId, TdbEpoch epoch) const
{
    if (naifId == centreNaifId) {
        return TwoBodyState{};
    }
    // The two primary relations this hierarchy actually has. Taking the conic directly avoids
    // forming a small vector as the difference of two large ones.
    if ((naifId == kNaifMoon && centreNaifId == kNaifEarth)
        || (naifId == kNaifEarth && centreNaifId == kNaifSun)) {
        return stateAboutPrimary(naifId, epoch);
    }
    if ((naifId == kNaifEarth && centreNaifId == kNaifMoon)
        || (naifId == kNaifSun && centreNaifId == kNaifEarth)) {
        return scaled(stateAboutPrimary(centreNaifId, epoch), -1.0);
    }
    return difference(barycentricState(naifId, epoch), barycentricState(centreNaifId, epoch));
}

TwoBodyState ConicEphemeris::linearBarycentricState(int naifId, TdbEpoch epoch) const
{
    const double elapsed = epoch.secondsPastJ2000() - m_anchorEpoch.secondsPastJ2000();

    switch (naifId) {
    case kNaifSun:                  return extrapolateLinear(m_linearSun, elapsed);
    case kNaifEarthMoonBarycentre:  return extrapolateLinear(m_linearBarycentre, elapsed);
    case kNaifEarth:                return extrapolateLinear(m_linearEarth, elapsed);
    case kNaifMoon:                 return extrapolateLinear(m_linearMoon, elapsed);
    default:                        break;
    }
    throw std::runtime_error("ConicEphemeris: no fixture for NAIF id " + std::to_string(naifId));
}

frames::FrameGraphSnapshot ConicEphemeris::buildSnapshot(const ReferenceData& data,
                                                         TdbEpoch epoch,
                                                         const frames::VehicleState& vehicle) const
{
    // Start from A2's snapshot so every non-celestial boundary is identical to what A2
    // measured, then replace only the origins this class owns.
    frames::FrameGraphSnapshot snapshot = frames::buildSnapshot(data, epoch, vehicle);

    const auto index = [](FrameId frame) { return static_cast<std::size_t>(frame); };

    const TwoBodyState sun = barycentricState(kNaifSun, epoch);
    const TwoBodyState barycentre = barycentricState(kNaifEarthMoonBarycentre, epoch);
    const TwoBodyState earth = barycentricState(kNaifEarth, epoch);
    const TwoBodyState moon = barycentricState(kNaifMoon, epoch);

    FrameTransform& sunTransform = snapshot.parentToFrame[index(FrameId::SunIcrf)];
    sunTransform.originInParent = sun.position;
    sunTransform.originVelocityInParent = sun.velocity;

    FrameTransform& barycentreTransform =
        snapshot.parentToFrame[index(FrameId::EarthMoonBarycentreIcrf)];
    barycentreTransform.originInParent = barycentre.position;
    barycentreTransform.originVelocityInParent = barycentre.velocity;

    // Parented to the barycentre, matching A2's graph exactly. The offsets stay at 4.7e6 m and
    // 3.8e8 m rather than 1.5e11 m, which is the property the hierarchical model was selected
    // for and which A3 must not quietly give up by re-parenting to the Sun.
    FrameTransform& earthTransform = snapshot.parentToFrame[index(FrameId::EarthIcrf)];
    earthTransform.originInParent = earth.position - barycentre.position;
    earthTransform.originVelocityInParent = earth.velocity - barycentre.velocity;

    FrameTransform& moonTransform = snapshot.parentToFrame[index(FrameId::MoonIcrf)];
    moonTransform.originInParent = moon.position - barycentre.position;
    moonTransform.originVelocityInParent = moon.velocity - barycentre.velocity;

    return snapshot;
}

} // namespace sol::proto::orbit
