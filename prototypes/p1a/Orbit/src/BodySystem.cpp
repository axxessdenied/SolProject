#include "Sol/Proto/Orbit/BodySystem.h"

#include "Sol/Proto/Orbit/OrbitalElements.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace sol::proto::orbit {
namespace {

using frames::BodyStateFixture;
using frames::ReferenceData;
using frames::Vec3;

/// The state of @p body relative to @p centre at the fixture epoch, from two SSB-relative
/// fixtures.
///
/// Differencing two barycentric vectors to get a much smaller one is the cancellation A2
/// measured in detail: at 1.5e11 m one ULP is 3e-5 m, so an Earth-relative vector formed this
/// way carries tens of microns of noise. That is four orders of magnitude below anything a
/// sphere-of-influence radius or a conic semi-major axis is sensitive to, so the difference is
/// taken here rather than routed through the frame graph, which would cost the same and add a
/// dependency this file does not otherwise need.
[[nodiscard]] TwoBodyState relativeState(const BodyStateFixture& body,
                                         const BodyStateFixture& centre) noexcept
{
    TwoBodyState state;
    state.position = body.position.metres() - centre.position.metres();
    state.velocity = body.velocity.metresPerSecond() - centre.velocity.metresPerSecond();
    return state;
}

} // namespace

BodySystem BodySystem::fromReferenceData(const ReferenceData& data)
{
    BodySystem system;

    const double muSun = data.gravitationalParameter(kNaifSun);
    const double muEarth = data.gravitationalParameter(kNaifEarth);
    const double muMoon = data.gravitationalParameter(kNaifMoon);

    if (muSun <= 0.0 || muEarth <= 0.0 || muMoon <= 0.0) {
        throw std::runtime_error(
            "BodySystem: the pinned kernel supplied a non-positive gravitational parameter");
    }

    system.m_moonMassFraction = muMoon / (muEarth + muMoon);

    // The Earth-Moon system's heliocentric conic. The barycentre is what actually follows a
    // conic about the Sun -- Earth alone does not, because it is being swung about the
    // barycentre by the Moon -- so the semi-major axis that belongs in Earth's Laplace radius
    // is the barycentre's.
    const TwoBodyState barycentreAboutSun =
        relativeState(data.bodyState(kNaifEarthMoonBarycentre), data.bodyState(kNaifSun));
    const OrbitalElements barycentreElements =
        elementsFromState(barycentreAboutSun, muSun + muEarth + muMoon);

    // The Moon's geocentric conic, which is the two-body problem of the Earth-Moon pair.
    const TwoBodyState moonAboutEarth =
        relativeState(data.bodyState(kNaifMoon), data.bodyState(kNaifEarth));
    const OrbitalElements moonElements = elementsFromState(moonAboutEarth, muEarth + muMoon);

    if (!(barycentreElements.semiMajorAxis > 0.0) || !(moonElements.semiMajorAxis > 0.0)) {
        throw std::runtime_error(
            "BodySystem: a fixture produced a non-elliptical conic where a bound orbit was "
            "required");
    }

    const frames::Ellipsoid& earthEllipsoid = data.earthEllipsoid();

    // Surface radii from the pinned planetary-constants kernel, which carries BODY10_RADII and
    // BODY301_RADII alongside Earth's. An earlier revision left the Sun's and the Moon's at zero
    // on the belief the kernel did not supply them; it does. Zero was not a neutral placeholder
    // either -- the below-surface eligibility rejection is guarded on a positive radius, so a
    // zero silently disabled it, and a lunar coast could pass through the Moon unremarked.
    GravitationalBody sun;
    sun.naifId = kNaifSun;
    sun.name = "Sun";
    sun.gravitationalParameter = muSun;
    sun.surfaceRadiusMetres = data.equatorialRadiusMetres(kNaifSun);
    sun.sphereOfInfluenceRadiusMetres = std::numeric_limits<double>::infinity();
    sun.atmosphereLimitRadiusMetres = 0.0;

    GravitationalBody earth;
    earth.naifId = kNaifEarth;
    earth.name = "Earth";
    earth.gravitationalParameter = muEarth;
    earth.surfaceRadiusMetres = earthEllipsoid.equatorialRadiusMetres();
    earth.sphereOfInfluenceRadiusMetres =
        barycentreElements.semiMajorAxis
        * std::pow((muEarth + muMoon) / muSun, 0.4);
    earth.atmosphereLimitRadiusMetres =
        earth.surfaceRadiusMetres + kEarthAtmosphereLimitAltitudeMetres;

    GravitationalBody moon;
    moon.naifId = kNaifMoon;
    moon.name = "Moon";
    moon.gravitationalParameter = muMoon;
    moon.surfaceRadiusMetres = data.equatorialRadiusMetres(kNaifMoon);
    moon.sphereOfInfluenceRadiusMetres =
        moonElements.semiMajorAxis * std::pow(muMoon / muEarth, 0.4);
    // Airless. Zero here is a physical statement, not a missing value: the eligibility rules
    // read it as "analytical coast is available at any altitude".
    moon.atmosphereLimitRadiusMetres = 0.0;

    system.m_bodies = {sun, earth, moon};
    system.m_primaries = {{kNaifSun, kNaifSun}, {kNaifEarth, kNaifSun}, {kNaifMoon, kNaifEarth}};
    system.m_semiMajorAxes = {{kNaifSun, std::numeric_limits<double>::infinity()},
                              {kNaifEarth, barycentreElements.semiMajorAxis},
                              {kNaifMoon, moonElements.semiMajorAxis}};
    return system;
}

const GravitationalBody& BodySystem::body(int naifId) const
{
    for (const GravitationalBody& candidate : m_bodies) {
        if (candidate.naifId == naifId) {
            return candidate;
        }
    }
    throw std::runtime_error("BodySystem: no body with NAIF id " + std::to_string(naifId));
}

int BodySystem::primaryOf(int naifId) const
{
    for (const auto& [child, parent] : m_primaries) {
        if (child == naifId) {
            return parent;
        }
    }
    throw std::runtime_error("BodySystem: no primary recorded for NAIF id "
                             + std::to_string(naifId));
}

bool BodySystem::isRoot(int naifId) const
{
    return primaryOf(naifId) == naifId;
}

double BodySystem::semiMajorAxisAboutPrimary(int naifId) const
{
    for (const auto& [id, axis] : m_semiMajorAxes) {
        if (id == naifId) {
            return axis;
        }
    }
    throw std::runtime_error("BodySystem: no semi-major axis recorded for NAIF id "
                             + std::to_string(naifId));
}

double BodySystem::sphereOfInfluenceRadius(int naifId) const
{
    return body(naifId).sphereOfInfluenceRadiusMetres;
}

} // namespace sol::proto::orbit
