# ADR 0008 — Astronomical reference data and time boundary

**Status:** Accepted

**Date:** 2026-08-12

**Amended:** 2026-08-12 after P1a increment A2, on two points: the ephemeris solution is named as a family rather than a single product, and the launch anchor's elevation is defined against the reference ellipsoid rather than mean sea level. Both amendments are marked inline and explained in [Amendments](#amendments-a2-2026-08-12).

## Context

The 2026 campaign start, real Solar System scale, orbital reference tests, and later reproducibility require a named astronomical source, explicit time scales, and a fixed launch anchor. A displayed UTC date is not by itself sufficient input to an ephemeris, and a fictional launch facility still needs deterministic prototype coordinates.

## Decision

- Use the **JPL DE440/DE441 solution family** as the initial reference for major Solar System body states around the first campaign epoch. DE441 is the long-span integration of the same solution and the two agree over the campaign interval, so either is acceptable; **every fixture must record which product actually supplied it.** *(Amended 2026-08-12; originally named DE440 alone.)*
- Use NAIF SPICE conventions and generic leap-second/planetary-constants data to define reference conversions and fixtures. Linking CSPICE into the game is not selected by this ADR; P1 may use precomputed, provenance-stamped fixtures, and any runtime library dependency requires its own dependency review.
- Present the initial campaign epoch as 2026-01-01 00:00:00 UTC.
- Evaluate and record ephemeris reference states at a TDB boundary. UTC-to-TDB conversion uses a pinned leap-seconds kernel and records the kernel name/version or checksum.
- Use geodetic latitude 28.0 degrees north, longitude 80.5 degrees west (`-80.5` degrees east-positive), and **5 m above the reference ellipsoid** as the reproducible P1 launch anchor. The ellipsoid is the one defined by `BODY399_RADII` in the pinned planetary-constants kernel. *(Amended 2026-08-12; originally "5 m above mean sea level".)*
- Record origin, reference frame, units, epoch, time scale, source kernel, and checksum with every golden orbital fixture. Do not treat unlabeled vectors or timestamps as valid reference data.
- Record the datum with every geodetic coordinate. A datum is part of the coordinate, not context for it.
- Allow the final fictional terrain placement to shift slightly for game-design needs. Such a shift must create new content coordinates and fixtures rather than silently changing the P1 reference case.

## Consequences

- Player-facing UTC and ephemeris TDB are explicit boundaries; they are not interchangeable scalar clocks.
- The engine's internal campaign-time storage representation and Earth orientation/terrain model remain decisions for their owning milestones.
- The DE440/DE441 solution supplies authoritative initial/reference states, not a requirement that every gameplay object be propagated by SPICE at runtime.
- Test fixtures remain reproducible if external services or generic kernels later update.

## Validation

- A reference-data generation record must reproduce the accepted epoch conversion and body-state fixtures from the pinned kernels.
- Round-trip time conversion tests must state their expected precision and include a leap-second-adjacent case even though the initial epoch is not adjacent to a leap second.
- The P1 anchor conversion must record geodetic coordinates, elevation datum, body shape/constants, and resulting body-fixed vector.

## Amendments (A2, 2026-08-12)

Both amendments were raised by P1a increment A2 and accepted by the user. Full measurements are
in the [A2 evidence index](../../evidence/p1a/A2/Index.md) and the
[fixture provenance record](../../fixtures/p1a/Provenance.md).

### DE440 named a product; the data is served as DE441

Every state vector fetched from JPL Horizons for the campaign epoch reports `{source: DE441}`.
DE441 is the long-span integration of the same JPL solution, and over the interval containing
2026 the two are the same dynamical fit — the difference is which time span was published, not a
different determination of where the planets are. The gravitational parameters do come from
`gm_de440.tpc`, so constants and states are from one family but not from identically named
products.

The alternative was to download `de440.bsp` and evaluate it directly. That was rejected: it is a
114 MB binary requiring Git LFS under ADR 0012, and it reopens the CSPICE runtime dependency
question under ADR 0007 to obtain numbers that agree with what Horizons already provides. A2
demonstrated that frame work needs a handful of provenance-stamped state vectors rather than a
queryable ephemeris.

Naming the family and requiring each fixture to record its actual product keeps the ADR truthful
without buying a dependency. If a later milestone needs a queryable ephemeris — A3's SOI
transitions are the first plausible candidate — that is a dependency decision on its own merits,
not a provenance patch.

### "Mean sea level" was a geoid statement the project cannot honour

Mean sea level is defined by the geoid, which departs from the reference ellipsoid by roughly
−30 m in the region of the P1 anchor. Honouring the original wording would require adopting a
geoid model such as EGM96 or EGM2008 as a new data dependency, in order to place a **fictional**
launch facility whose real-world elevation does not exist to be faithful to. That is 30 m of
machinery in service of nothing, against a P1a position budget of 1 mm.

Defining the 5 m against the reference ellipsoid instead makes the ADR state what is actually
computable from the pinned kernels, keeps the anchor reproducible from checksummed data, and
removes a 30 m discrepancy that would otherwise have sat unresolved behind a plausible number.

The ellipsoid choice is not free either. A2 measured that the IAU `pck00011` ellipsoid
(equatorial radius 6 378 136.6 m) and WGS84 (6 378 137.0 m) place the same geodetic anchor
**0.403 m apart** — 400 times the position budget. The IAU value is adopted because it arrives
with a checksum from the source this ADR already names; no pinned NAIF kernel supplies WGS84.
That measurement is why the Decision section now requires a datum to travel with every geodetic
coordinate: at this tolerance an unlabelled latitude and longitude is not a location.

If a later milestone adopts WGS84 for interoperability with real geospatial data, it must create
new content coordinates and fixtures rather than silently reinterpreting the P1 reference case,
under the existing rule for terrain placement shifts.

## Sources

- JPL documents DE440/DE441 and recommends SPICE for programmatic ephemeris access in its [planetary and lunar ephemeris export information](https://ssd.jpl.nasa.gov/planets/eph_export.html).
- NAIF documents UTC representations, TDB/ephemeris time, and leap-second-kernel requirements in the [SPICE Time Subsystem](https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/time.html).
- NAIF publishes SPK, planetary-constants, and leap-seconds data as [generic kernels](https://naif.jpl.nasa.gov/naif/data_generic.html).
