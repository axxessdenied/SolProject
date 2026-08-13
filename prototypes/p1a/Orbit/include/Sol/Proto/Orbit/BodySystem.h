#pragma once

#include "Sol/Proto/Frames/ReferenceData.h"
#include "Sol/Proto/Orbit/TwoBody.h"

#include <vector>

namespace sol::proto::orbit {

/// NAIF body codes used throughout increment A3, named so no call site carries a bare integer.
inline constexpr int kNaifSun = 10;
inline constexpr int kNaifEarthMoonBarycentre = 3;
inline constexpr int kNaifEarth = 399;
inline constexpr int kNaifMoon = 301;

/// The patched-conic body hierarchy A3 propagates in, built from the pinned ADR 0008 data.
///
/// ### The gravitational parent is not the frame parent
///
/// A2's frame graph parents Earth and the Moon to the Earth-Moon barycentre, and parents that
/// barycentre to the Solar System barycentre. That is the right hierarchy for *coordinates*,
/// because it keeps every stored offset at the scale of the relationship it describes.
///
/// It is the wrong hierarchy for *gravity*. A barycentre has no mass and exerts no force, so it
/// cannot own a sphere of influence and nothing orbits it in the ADR 0011 sense. Under patched
/// conics the Moon's primary is Earth, Earth's primary is the Sun, and the Sun is the root.
///
/// A3 therefore carries two parent relations over the same bodies, and conflating them is the
/// mistake this class exists to prevent. Frames answer "where is this expressed"; this class
/// answers "what is pulling on it".
///
/// ### The Sun is the root, not the Solar System barycentre
///
/// ADR 0011 gives the Sun no gravitational parent, so its sphere of influence is unbounded and
/// its own motion is outside the model. See ConicEphemeris.h, which records what that costs.
class BodySystem {
public:
    /// Builds the hierarchy from the pinned reference data.
    ///
    /// Gravitational parameters come from the pinned gm_de440 kernel and the semi-major axes
    /// from the pinned Horizons state fixtures, so every sphere-of-influence radius below is
    /// derived from checksummed data rather than from a published round number. Throws when a
    /// required body or constant is absent, on the same principle as ReferenceData: a prototype
    /// that substitutes a default for reference data produces output that looks like evidence.
    [[nodiscard]] static BodySystem fromReferenceData(const frames::ReferenceData& data);

    /// Bodies in a fixed order: Sun, Earth, Moon. Fixed rather than incidental, because ADR
    /// 0010's determinism guarantee covers the order results are emitted in.
    [[nodiscard]] const std::vector<GravitationalBody>& bodies() const noexcept
    {
        return m_bodies;
    }

    /// Looks up a body by NAIF id. Throws when it is absent.
    [[nodiscard]] const GravitationalBody& body(int naifId) const;

    /// The gravitational primary of @p naifId, or @p naifId itself when it is the root.
    [[nodiscard]] int primaryOf(int naifId) const;

    [[nodiscard]] bool isRoot(int naifId) const;

    /// Semi-major axis of @p naifId's conic about its primary, in metres. Infinite for the root.
    [[nodiscard]] double semiMajorAxisAboutPrimary(int naifId) const;

    /// The Laplace sphere-of-influence radius of @p naifId, in metres.
    ///
    ///     r = a * (m / M)^(2/5)
    ///
    /// as ADR 0011 specifies, with a the body's semi-major axis about its primary and m and M
    /// the body's and primary's masses. Masses appear only as a ratio, so the gravitational
    /// parameters are used directly and the gravitational constant never has to be known --
    /// which matters, because G is the least precisely known constant in physics and the pinned
    /// kernel supplies GM products rather than masses for exactly that reason.
    [[nodiscard]] double sphereOfInfluenceRadius(int naifId) const;

    /// Mass ratio of the Moon to the Earth-Moon system, dimensionless.
    ///
    /// Needed to split the Earth-Moon barycentre into its two bodies, which is the one place
    /// the frame hierarchy and the gravitational hierarchy have to be reconciled numerically
    /// rather than merely distinguished.
    [[nodiscard]] double moonMassFraction() const noexcept { return m_moonMassFraction; }

private:
    std::vector<GravitationalBody> m_bodies;
    std::vector<std::pair<int, int>> m_primaries;
    std::vector<std::pair<int, double>> m_semiMajorAxes;
    double m_moonMassFraction{0.0};
};

/// Altitude above the Earth reference ellipsoid's equatorial radius at which ADR 0011 declares
/// the propagation drag-free and the analytical coast eligible, in metres.
///
/// ### This number is chosen, not derived, and the reason matters
///
/// The obvious derivation is to place the boundary where neglected drag costs less than the 1 m
/// handoff tolerance over one orbit. That derivation gives roughly 450 km, which would make the
/// entire P1a reference case -- a 200 km orbit -- permanently ineligible for analytical coast,
/// and would mean the first playable's own contract orbit could never be time-warped. A
/// threshold that excludes the mission the game is built around is a sign the derivation is
/// answering the wrong question.
///
/// It is the wrong question because ADR 0011 does not *approximate* a drag-affected trajectory;
/// it *defines* orbits as drag-free and non-decaying. There is no true trajectory for the coast
/// to diverge from, so there is no accuracy budget to spend. The 1 m tolerance governs the
/// discontinuity introduced *at a transition*, which is a self-consistency property of the
/// handoff and is unaffected by where the boundary sits.
///
/// What the boundary actually selects is which regime a craft is simulated in, and that is a
/// gameplay and physics-model decision: below it, aerodynamic forces act on an active craft and
/// ascent and reentry are flown; above it, they do not. 140 km is chosen because it sits above
/// the altitude where drag meaningfully shapes a powered ascent and below any orbit the player
/// is expected to hold. A3 records it; final tuning belongs to P2/M5 and is listed in the open
/// questions.
inline constexpr double kEarthAtmosphereLimitAltitudeMetres = 140.0e3;

} // namespace sol::proto::orbit
