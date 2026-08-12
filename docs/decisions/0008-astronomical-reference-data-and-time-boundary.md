# ADR 0008 — Astronomical reference data and time boundary

**Status:** Accepted

**Date:** 2026-08-12

## Context

The 2026 campaign start, real Solar System scale, orbital reference tests, and later reproducibility require a named astronomical source, explicit time scales, and a fixed launch anchor. A displayed UTC date is not by itself sufficient input to an ephemeris, and a fictional launch facility still needs deterministic prototype coordinates.

## Decision

- Use JPL DE440 SPK data as the initial reference for major Solar System body states around the first campaign epoch.
- Use NAIF SPICE conventions and generic leap-second/planetary-constants data to define reference conversions and fixtures. Linking CSPICE into the game is not selected by this ADR; P1 may use precomputed, provenance-stamped fixtures, and any runtime library dependency requires its own dependency review.
- Present the initial campaign epoch as 2026-01-01 00:00:00 UTC.
- Evaluate and record ephemeris reference states at a TDB boundary. UTC-to-TDB conversion uses a pinned leap-seconds kernel and records the kernel name/version or checksum.
- Use geodetic latitude 28.0 degrees north, longitude 80.5 degrees west (`-80.5` degrees east-positive), and 5 m above mean sea level as the reproducible P1 launch anchor.
- Record origin, reference frame, units, epoch, time scale, source kernel, and checksum with every golden orbital fixture. Do not treat unlabeled vectors or timestamps as valid reference data.
- Allow the final fictional terrain placement to shift slightly for game-design needs. Such a shift must create new content coordinates and fixtures rather than silently changing the P1 reference case.

## Consequences

- Player-facing UTC and ephemeris TDB are explicit boundaries; they are not interchangeable scalar clocks.
- The engine's internal campaign-time storage representation and Earth orientation/terrain model remain decisions for their owning milestones.
- DE440 supplies authoritative initial/reference states, not a requirement that every gameplay object be propagated by SPICE at runtime.
- Test fixtures remain reproducible if external services or generic kernels later update.

## Validation

- A reference-data generation record must reproduce the accepted epoch conversion and body-state fixtures from the pinned kernels.
- Round-trip time conversion tests must state their expected precision and include a leap-second-adjacent case even though the initial epoch is not adjacent to a leap second.
- The P1 anchor conversion must record geodetic coordinates, elevation datum, body shape/constants, and resulting body-fixed vector.

## Sources

- JPL documents DE440/DE441 and recommends SPICE for programmatic ephemeris access in its [planetary and lunar ephemeris export information](https://ssd.jpl.nasa.gov/planets/eph_export.html).
- NAIF documents UTC representations, TDB/ephemeris time, and leap-second-kernel requirements in the [SPICE Time Subsystem](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/time.html).
- NAIF publishes SPK, planetary-constants, and leap-seconds data as [generic kernels](https://naif.jpl.nasa.gov/naif/data_generic.html).
