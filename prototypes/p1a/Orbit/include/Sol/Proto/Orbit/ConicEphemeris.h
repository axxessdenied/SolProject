#pragma once

#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/ReferenceData.h"
#include "Sol/Proto/Orbit/BodySystem.h"
#include "Sol/Proto/Orbit/TwoBody.h"

#include <string_view>

namespace sol::proto::orbit {

/// Celestial origin motion by ADR 0011 conic propagation.
///
/// ### What this replaces
///
/// A2 moved every celestial frame origin by linear extrapolation from its single fixture epoch,
/// and said so loudly: `frames::OriginMotionModel` calls itself "frame kinematics only, not an
/// ephemeris", and A2's handoff records replacing it as the one obligation carried into A3.
/// This class is that replacement.
///
/// A2's choice was correct for A2. A frame round trip is sensitive only to whether the forward
/// and reverse transforms agree, and linear motion is exactly self-consistent, so the linear
/// model measured what A2 was measuring without importing propagation into an increment that
/// had not yet decided how propagation works. It is not correct for A3, whose entire subject is
/// propagation.
///
/// ### The hierarchy
///
/// Under ADR 0011 each body follows a conic about its gravitational primary, which is not its
/// frame parent (see BodySystem.h):
///
///   - the Moon follows a conic about Earth with mu = GM_Earth + GM_Moon;
///   - Earth and the Moon are then placed either side of their barycentre by mass ratio, which
///     is what makes the frame graph's barycentric parenting agree with the gravitational
///     hierarchy instead of contradicting it;
///   - the Earth-Moon barycentre follows a conic about the Sun with
///     mu = GM_Sun + GM_Earth + GM_Moon.
///
/// ### What is still not modelled, and why it is stated rather than hidden
///
/// **The Sun's motion about the Solar System barycentre remains linear.** ADR 0011 gives the
/// Sun no gravitational parent, so there is no conic to propagate: the Sun's barycentric wobble
/// is driven by Jupiter and Saturn, and those are precisely the perturbations the ADR excludes.
/// The honest options were to leave it linear and say so, or to declare the Sun fixed at the
/// barycentre. Linear is kept because it is at least consistent with the fixture's own velocity.
///
/// This is a real limitation with a bounded cost, and A3 measures rather than asserts it. It
/// affects only the SsbIcrf-to-SunIcrf boundary. Nothing A3 gates on crosses that boundary:
/// every propagation result is geocentric or Moon-relative, and the sphere-of-influence
/// hierarchy is rooted at the Sun rather than at the barycentre for this reason.
///
/// **Earth's orientation is still IAU_EARTH**, unchanged from A2 -- no nutation, no polar
/// motion, no UT1-UTC. That was A2's second recorded limitation and A3 does not close it; it is
/// an orientation model, not a propagation model, and it belongs to whichever milestone selects
/// the production Earth orientation.
class ConicEphemeris {
public:
    /// Anchors every conic at the fixtures' shared epoch.
    [[nodiscard]] static ConicEphemeris fromReferenceData(const frames::ReferenceData& data,
                                                          const BodySystem& system);

    /// State of @p naifId relative to the Solar System barycentre at @p epoch, in metres and
    /// metres per second on ICRF axes.
    [[nodiscard]] TwoBodyState barycentricState(int naifId, frames::TdbEpoch epoch) const;

    /// State of @p naifId relative to its gravitational primary at @p epoch.
    ///
    /// The quantity the conic actually propagates, exposed directly so a caller that wants the
    /// Moon's geocentric position does not have to form it by differencing two barycentric
    /// vectors and paying the cancellation A2 characterised.
    [[nodiscard]] TwoBodyState stateAboutPrimary(int naifId, frames::TdbEpoch epoch) const;

    /// State of @p naifId relative to @p centreNaifId at @p epoch.
    ///
    /// Uses the direct conic when @p centreNaifId is the body's gravitational primary, and only
    /// then falls back to differencing two barycentric states. The distinction is worth the
    /// branch: the Moon's geocentric position formed by differencing two 1.5e11 m vectors
    /// carries tens of microns of cancellation noise, while the conic produces it at its own
    /// 3.8e8 m scale where one ULP is 60 nanometres.
    [[nodiscard]] TwoBodyState stateRelativeTo(int naifId, int centreNaifId,
                                               frames::TdbEpoch epoch) const;

    /// The A2 model's answer for the same body and epoch: linear extrapolation from the fixture.
    ///
    /// Present so A3 can measure the divergence rather than assert that the replacement was
    /// needed. Keeping both in one class means the comparison uses one set of fixtures and one
    /// epoch convention, which a scenario reaching into two libraries would not guarantee.
    [[nodiscard]] TwoBodyState linearBarycentricState(int naifId, frames::TdbEpoch epoch) const;

    /// Builds an A2-shaped frame snapshot whose celestial origins move by conic propagation.
    ///
    /// Implemented by building A2's snapshot and replacing only the four celestial transforms.
    /// Deliberate: the rotating Earth boundary, the launch-site basis, and the floating vehicle
    /// origin must stay bit-for-bit what A2 measured, so that any difference between an A2 and
    /// an A3 conversion is attributable to origin motion alone and not to a re-derivation of
    /// something A2 already settled.
    [[nodiscard]] frames::FrameGraphSnapshot buildSnapshot(
        const frames::ReferenceData& data, frames::TdbEpoch epoch,
        const frames::VehicleState& vehicle) const;

    [[nodiscard]] frames::TdbEpoch anchorEpoch() const noexcept { return m_anchorEpoch; }

    /// Description recorded in evidence reports, mirroring frames::OriginMotionModel so the two
    /// are directly comparable in the output.
    static constexpr std::string_view kDescription =
        "ADR 0011 conic propagation about each body's gravitational primary, anchored at the "
        "fixture epoch; the Sun's motion about the Solar System barycentre remains linear "
        "because ADR 0011 gives the Sun no primary";

private:
    frames::TdbEpoch m_anchorEpoch{};

    TwoBodyState m_sunAboutBarycentre{};
    TwoBodyState m_barycentreAboutSun{};
    TwoBodyState m_moonAboutEarth{};

    double m_muSun{0.0};
    double m_muEarthMoonSystem{0.0};
    double m_muSunSystem{0.0};
    double m_moonMassFraction{0.0};

    /// Fixture states, retained for the linear comparison path.
    TwoBodyState m_linearSun{};
    TwoBodyState m_linearBarycentre{};
    TwoBodyState m_linearEarth{};
    TwoBodyState m_linearMoon{};
};

} // namespace sol::proto::orbit
