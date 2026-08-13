#pragma once

#include "Sol/Proto/Frames/Ellipsoid.h"
#include "Sol/Proto/Frames/FrameTransform.h"
#include "Sol/Proto/Frames/TimeScales.h"
#include "Sol/Proto/Frames/Units.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sol::proto::frames {

/// Provenance for one checked-in fixture file.
///
/// ADR 0008 forbids treating an unlabelled vector or timestamp as valid reference data. This
/// struct is what "labelled" means in code: the file, where it came from, what it is, and the
/// digest that says it is still the file the numbers were derived from.
struct FixtureProvenance {
    std::string fileName;
    std::string role;
    std::string sourceUrl;
    std::string expectedSha256;
    std::string actualSha256;
};

/// One body state read from a Horizons vector table.
struct BodyStateFixture {
    std::string bodyName;
    /// NAIF integer body code, e.g. 399 for Earth.
    int naifId{0};
    /// Centre the state is relative to, as reported by Horizons.
    std::string centreName;
    /// Reference frame, as reported by Horizons.
    std::string referenceFrame;
    /// The epoch of the state, from the table's own JDTDB column.
    TdbEpoch epoch{};
    /// Calendar TDB string exactly as Horizons printed it.
    std::string calendarTdb;
    /// The ephemeris solution Horizons reported for this body, e.g. "DE441".
    std::string ephemerisSource;
    PositionMetres position{};
    VelocityMetresPerSecond velocity{};
    /// Number of significant decimal digits Horizons printed for the position components.
    /// The reference data cannot resolve better than this, and the precision budget says so.
    int printedSignificantDigits{0};
};

/// Everything A2 needs from the pinned ADR 0008 reference data, loaded and checksum-verified.
///
/// Construction fails rather than degrading. A missing kernel, a changed digest, or a
/// Horizons table whose epoch disagrees with the requested one all throw. A prototype that
/// quietly substitutes a default for reference data is worse than one that does not run,
/// because its output looks exactly like evidence.
class ReferenceData {
public:
    /// @param fixtureRoot  Directory holding `kernels/` and `horizons/`.
    /// @param verifyChecksums  When false, digests are still computed and reported but a
    ///                         mismatch does not throw. Only for diagnosing a fixture update;
    ///                         never for an evidence run.
    [[nodiscard]] static ReferenceData loadFromDirectory(const std::filesystem::path& fixtureRoot,
                                                         bool verifyChecksums = true);

    [[nodiscard]] const TimeScales& timeScales() const noexcept { return m_timeScales; }

    /// The IAU Earth ellipsoid from the pinned planetary-constants kernel.
    [[nodiscard]] const Ellipsoid& earthEllipsoid() const noexcept { return m_earthEllipsoid; }

    /// BODY399_POLE_RA / POLE_DEC / PM coefficient triples, in kernel units.
    [[nodiscard]] const double* earthPoleRightAscension() const noexcept { return m_poleRa; }
    [[nodiscard]] const double* earthPoleDeclination() const noexcept { return m_poleDec; }
    [[nodiscard]] const double* earthPrimeMeridian() const noexcept { return m_primeMeridian; }

    /// Gravitational parameters, converted from the kernel's km^3/s^2 to m^3/s^2.
    [[nodiscard]] double gravitationalParameter(int naifId) const;

    /// Equatorial radius from the pinned kernel's BODY<id>_RADII, in metres. Throws when absent.
    ///
    /// The equatorial rather than mean radius, deliberately: it is the largest, so a
    /// below-surface test built on it triggers no later than the true surface at any latitude.
    /// Available for the Sun (10), the Moon (301), and Earth (399); the Sun and Moon are
    /// spherical in this kernel, so for them the distinction does not arise.
    [[nodiscard]] double equatorialRadiusMetres(int naifId) const;

    /// The body state fixtures, in load order.
    [[nodiscard]] const std::vector<BodyStateFixture>& bodyStates() const noexcept
    {
        return m_bodyStates;
    }

    /// Looks up a body state by NAIF id. Throws when it is absent.
    [[nodiscard]] const BodyStateFixture& bodyState(int naifId) const;

    /// The common epoch every body fixture shares. Throws at load time if they disagree,
    /// since states at different instants cannot be differenced into a frame graph.
    [[nodiscard]] TdbEpoch fixtureEpoch() const noexcept { return m_fixtureEpoch; }

    [[nodiscard]] const std::vector<FixtureProvenance>& provenance() const noexcept
    {
        return m_provenance;
    }

    /// The ADR 0008 launch anchor: 28.0 N, 80.5 W, 5 m above the reference ellipsoid.
    ///
    /// Written here as the ADR's literal numbers rather than read from a data file, because
    /// the ADR is the source of truth for it. Changing it is an ADR amendment, and ADR 0008
    /// requires such a change to create new fixtures rather than silently move the P1
    /// reference case.
    [[nodiscard]] static Geodetic launchAnchor() noexcept;

private:
    TimeScales m_timeScales;
    Ellipsoid m_earthEllipsoid{"uninitialised", 1.0, 1.0};
    double m_poleRa[3]{};
    double m_poleDec[3]{};
    double m_primeMeridian[3]{};
    std::vector<std::pair<int, double>> m_gravitationalParameters;
    std::vector<std::pair<int, double>> m_equatorialRadii;
    std::vector<BodyStateFixture> m_bodyStates;
    std::vector<FixtureProvenance> m_provenance;
    TdbEpoch m_fixtureEpoch{};
};

/// Parses `--fixtures <dir>` from the command line, defaulting to the directory baked in at
/// configure time.
///
/// Mirrors the harness's `--out` handling deliberately: a scenario that silently fell back to
/// a default after being given a bad path would attribute its numbers to the wrong data.
/// Throws std::invalid_argument when the flag is present without a value.
[[nodiscard]] std::filesystem::path parseFixtureRoot(int argc, char** argv);

/// Parses one Horizons vector table.
///
/// Reads the `$$SOE`/`$$EOE` block and the header lines that identify the target, centre,
/// frame, and ephemeris solution. Throws when the table holds anything other than exactly one
/// epoch, because a fixture with several rows invites a caller to pick the wrong one.
[[nodiscard]] BodyStateFixture parseHorizonsVectorTable(const std::filesystem::path& path);

} // namespace sol::proto::frames
