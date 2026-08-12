# P1a reference-data fixtures

Pinned astronomical reference data for increment A2, acquired 2026-08-12 under
[ADR 0008](../../docs/decisions/0008-astronomical-reference-data-and-time-boundary.md).

ADR 0008 requires that every golden orbital fixture record origin, reference frame, units,
epoch, time scale, source kernel, and checksum, and forbids treating an unlabelled vector or
timestamp as valid reference data. This file is the human-readable half of that record. The
machine-readable half is emitted by the `ReferenceFixtures` scenario.

**These files are frozen.** They are inputs to published measurements, not a cache. The
expected SHA-256 of each is compiled into `prototypes/p1a/Frames/src/ReferenceData.cpp` and
verified at load, so a scenario cannot silently read an edited or re-downloaded file. Updating
a fixture means changing the data and the recorded digest in one reviewed commit, and re-running
every measurement that depended on it.

## Files

| File | Role | SHA-256 |
|---|---|---|
| `kernels/naif0012.tls` | Leap seconds and the TDB coefficients (SPICE LSK) | `678e32bdb5a744117a467cd9601cd6b373f0e9bc9bbde1371d5eee39600a039b` |
| `kernels/pck00011.tpc` | IAU body shape and orientation constants (SPICE PCK) | `3dff7b1dbeceaa01f25467767d3fa25816051c85d162d1edf04acb310ee28bb1` |
| `kernels/gm_de440.tpc` | DE440 gravitational parameters (SPICE PCK) | `924ddf4fb9ead9fe8a1aa55780bcabde40b09d00065d58226e24b68d8092f140` |
| `horizons/ssb-sun.txt` | Sun state relative to the Solar System barycentre | `a2124d8896fa3f5d27c47aa78ff0c667665ffaf89d8e006cb73c78a425d0c218` |
| `horizons/ssb-emb.txt` | Earth-Moon barycentre state | `73dbc96ba0e1478b4db940f7fe2c92a82c7b965b19ed1c4bc21b17e6fcd24d4a` |
| `horizons/ssb-earth.txt` | Earth state | `36b486dc26c703b8909d9c02af8d9c812f99ea0bc6a52af658939cba002900cb` |
| `horizons/ssb-moon.txt` | Moon state | `18c3099749c69534e5bd2497d9713385e8fa3597169ef26fa6f4dfbc3078c8c0` |

## Sources

NAIF generic kernels, from `https://naif.jpl.nasa.gov/pub/naif/generic_kernels/`:

```
lsk/naif0012.tls
pck/pck00011.tpc
pck/gm_de440.tpc
```

The DE440 SPK itself is **not** checked in. `de440.bsp` is roughly 114 MB, and A2 needs four
state vectors rather than a queryable ephemeris. Those four come from JPL Horizons, which
evaluates the same JPL solution and reports which one it used.

## Body-state acquisition

Each Horizons file was fetched once with the following parameters, differing only in `COMMAND`:

| Parameter | Value | Why |
|---|---|---|
| `EPHEM_TYPE` | `VECTORS` | Cartesian state, not an observer-relative table |
| `CENTER` | `500@0` | Solar System barycentre |
| `REF_PLANE` | `FRAME` | ICRF equatorial axes, **not** the ecliptic. Getting this wrong rotates every vector by 23.4 degrees and still parses cleanly. |
| `REF_SYSTEM` | `ICRF` | |
| `VEC_TABLE` | `2` | Position and velocity |
| `VEC_CORR` | `NONE` | Geometric states. Light-time or aberration corrections would shift positions by hundreds of kilometres. |
| `OUT_UNITS` | `KM-S` | Converted to metres once, at the parse boundary |
| `CSV_FORMAT` | `YES` | |
| `TLIST` | `2461041.5008007398` | The campaign epoch on the TDB scale, see below |

`COMMAND` was `'10'` (Sun), `'3'` (Earth-Moon barycentre), `'399'` (Earth), and `'301'` (Moon).

The loader re-checks the units, the frame, and the geometric-state declaration in every file's
own header rather than trusting this table, and refuses a file that disagrees.

## Epoch

The campaign epoch is displayed as **2026-01-01 00:00:00 UTC** (ADR 0008). Converting it through
the pinned leap-second kernel gives:

| Step | Value | Source |
|---|---|---|
| TAI − UTC | 37 s | `DELTET/DELTA_AT`, final entry, effective 2017-01-01 |
| TT − TAI | 32.184 s | `DELTET/DELTA_T_A` |
| TDB − TT | −7.83 × 10⁻⁵ s | `DELTET/K`, `DELTET/EB`, `DELTET/M` |
| **TDB − UTC** | **69.18392 s** | |
| **JD TDB** | **2461041.5008007398** | |

Horizons echoes the requested epoch as `A.D. 2026-Jan-01 00:01:09.1839 TDB`, which agrees to its
printed 0.1 ms resolution. That agreement is the cross-check that matters: it validates this
project's time boundary against an independent implementation of the same standard, using data
neither side derived from the other. `FramesSelfCheck` asserts it on every run.

## Ephemeris product: DE441

Every Horizons file reports `{source: DE441}`. The gravitational parameters come from
`gm_de440.tpc`.

DE441 is the long-span integration of the same JPL solution; over the interval containing 2026
the two are the same dynamical fit, and the difference is which time span was published rather
than a different determination. ADR 0008 originally named DE440 alone; **it was amended on
2026-08-12 to name the DE440/DE441 family and to require each fixture to record which product
supplied it.** This file and the `ReferenceFixtures` report are that record.

The rejected alternative was downloading `de440.bsp` and evaluating it directly, which is a
114 MB binary under Git LFS and reopens the CSPICE dependency question under ADR 0007, to obtain
numbers that agree with what Horizons already serves. See ADR 0008's Amendments section.

## Resolution of the data itself

Horizons prints 16 significant digits. For Earth's barycentric position, about 1.5 × 10⁸ km, the
last printed digit is worth roughly 0.15 mm. No downstream computation can be more certain about
an absolute barycentric position than that, regardless of how exactly the arithmetic is done.
The precision budget records this explicitly rather than treating the fixtures as exact.

## What is *not* pinned here

- **Earth orientation.** A2 uses the IAU_EARTH definition from `pck00011.tpc`, which omits
  nutation, polar motion, and UT1−UTC and can differ from ITRF by tens of metres at the surface.
  That is adequate for measuring the numerics of a rotating-frame boundary and is not adequate
  for navigation. The production Earth orientation model is an open decision.
- **A geoid.** None is used, and none is needed: ADR 0008 was amended on 2026-08-12 to define the
  anchor's 5 m against the reference ellipsoid rather than mean sea level, so the roughly −30 m
  geoid separation in this region no longer sits between the ADR's words and the computation.
- **WGS84.** A2 uses the IAU radii from `pck00011.tpc`; WGS84 places the same geodetic anchor
  0.403 m away. Both are recorded, and the pinned kernel is the adopted datum because it arrives
  with a checksum from the source ADR 0008 names. Adopting WGS84 later would require new content
  coordinates and fixtures rather than a reinterpretation of this one.
