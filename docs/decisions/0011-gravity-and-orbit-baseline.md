# ADR 0011 — Gravity and orbit baseline

**Status:** Accepted

**Date:** 2026-08-12

## Context

`SolProjectNotes/Open-Questions.md` listed the gravity/orbit baseline — patched conics, selective n-body effects, perturbations, and sphere-of-influence behavior — as needed before active-flight implementation. The P1a increment A3 transition contract cannot be written without it: the eligibility rules, handoff tolerances, and analytical coast method all follow from which model is authoritative.

The choice also determines what trajectory prediction can promise. A model with perturbations produces predictions that drift from the executed trajectory, which changes what the orbital map can honestly display and what maneuver planning can guarantee.

## Decision

Use **patched conics with spheres of influence** as the authoritative orbital model.

- Exactly one body exerts gravity on an object at any instant: the body whose sphere of influence currently contains it.
- Analytical coast is Kepler propagation of a conic section about that body.
- Sphere-of-influence radii use the Laplace formulation, `r = a·(m/M)^(2/5)`, computed from the ADR 0008 reference data and recorded as fixtures.
- Sphere-of-influence crossings are discrete, scheduled events with explicit state ownership on each side of the boundary.
- **No perturbations enter the orbital propagation.** No J2 oblateness, no third-body effects, no solar radiation pressure, and no atmospheric drag above the local-physics regime.
- Orbits do not decay. A 200 km circular orbit is stable indefinitely in campaign time.

### Atmospheric forces are not excluded from flight

This ADR governs **orbital propagation**, not local flight. Aerodynamic drag, lift, and heating apply as forces on an active craft inside the atmosphere in the local physics regime, exactly as GDD note 04 describes. Ascent is not drag-free.

The boundary is: below the atmospheric limit, an active craft is integrated numerically with aerodynamic forces. Above it, propagation is conic and drag-free. The altitude of that boundary and the handoff behavior across it belong to increment A3 and P2/M5.

## Alternatives considered

- **Conics plus selective perturbations (J2, drag):** would make low orbits decay realistically and enable sun-synchronous orbits and realistic station-keeping. Rejected for the first playable because it makes the analytical coast approximate rather than exact, which complicates every handoff tolerance, makes trajectory prediction uncertain in a way the orbital map would have to communicate, and adds warp-determinism problems. It is the most likely future upgrade path.
- **Numerical n-body for active craft, conic for coast:** enables Lagrange points, real transfer windows, and genuine perturbation effects. Rejected as disproportionate to the first playable and costly in warp determinism, prediction, and handoff tolerance.
- **Full n-body for everything:** already excluded by the Engine-Plan scope controls.

## Consequences

- **Lagrange points do not exist.** L-point stations, halo orbits, and the associated infrastructure are unavailable while this ADR stands. P5's habitats and any L-point content would require superseding it.
- **Perturbation-driven mission design is unavailable.** Sun-synchronous orbits, frozen orbits, J2-assisted plane changes, and station-keeping as an ongoing operational cost do not arise. P4's asteroid operations and P3's persistent mission scheduling are the places where a player familiar with real spaceflight is most likely to notice.
- **Orbits are permanent.** No decay means no deorbit-by-neglect, no debris lifetime, and no fuel budget for maintaining low orbits. This removes a genuine long-term operational pressure from the company economy.
- Trajectory prediction is exact, cheap, and closed-form. The orbital map can display a predicted path with no uncertainty band, and maneuver planning can guarantee its result.
- Time warp is trivially safe during coast, because coast is an analytic evaluation rather than an integration.
- The A3 100 m one-orbit gate compares a numerical integrator against the Kepler analytic solution — a direct validation of the integrator and handoff, since the analytic solution *is* the authoritative model rather than merely a test oracle.
- The Orbital Environmental Survey contract's stable-orbit requirement needs no special handling.
- Superseding this ADR later is a substantial change. Adding perturbations makes the analytic coast approximate and forces every handoff tolerance, prediction display, and warp rule to be revisited. It should be treated as a milestone, not a patch.

## Validation

**Completed by P1a increment A3 on 2026-08-12.** Evidence: [`evidence/p1a/A3/Index.md`](../../evidence/p1a/A3/Index.md). This ADR is **confirmed from measurement**; nothing in it was amended.

| Validation item | Result |
|---|---|
| Increment A3 measures the numerical integrator against the Kepler analytic reference over one 200 km orbit period and meets the accepted 100 m gate. | **Met.** 44.6 m at a 64 s RK4 step; 0.5 mm at 4 s. The eccentric 200 × 2000 km case gives the same ordering. |
| Sphere-of-influence radii are computed from pinned ADR 0008 reference data. | **Met.** Earth 9.2918 × 10⁸ m, Moon 6.6195 × 10⁷ m, derived from `gm_de440.tpc` and the checksummed Horizons tables rather than copied from a published figure. They are computed at load from the existing fixtures rather than checked in as new ones, so no fixture with separate provenance was added. |
| Sphere-of-influence crossings preserve state within the 1 m and 1 mm/s handoff tolerance and are ordered deterministically under time warp per ADR 0010. | **Met.** Worst discontinuity 4.0 µm at Earth's boundary and 15 nm at the Moon's. Crossing instants are identical to the nanosecond across warp granularities from 1 s to 10 000 s — for trajectories that cross the boundary decisively. See the exception below. |
| A scenario confirms that a 200 km circular orbit shows no secular altitude change over an extended warped coast, since decay is not modelled. | **Met.** Over 100 days of warped coast the semi-major axis moves −5.8 µm, periapsis −13.2 µm, apoapsis +1.7 µm. That is arithmetic, not decay. |

### One measured exception, recorded rather than waived

A trajectory captured into an orbit that runs *tangent* to a sphere-of-influence boundary — a marginal capture — has no well-conditioned crossing time. An arbitrarily small change of state moves a crossing arbitrarily far in time or removes it, so no sampling scheme reproduces the chronology across warp factors. A3 measured 6, 6, 4, 2, and 2 crossings for the same trajectory at five warp granularities.

This is a property of the physics under this ADR's model, not a defect in the propagator, and it does not affect the state tolerance: those crossings still hand the state over within 34 nanometres. Deterministic ordering under warp therefore holds for decisive crossings and does not hold for tangent ones. The response a game needs is most likely a deliberate rule about when the simulation commits to a capture, and is tracked in [Open Questions](../../SolProjectNotes/Open-Questions.md).

### What A3 settled that this ADR had left open

- **The atmosphere limit** — which this ADR explicitly assigned to increment A3 — is **140 km** above Earth's reference-ellipsoid equatorial radius. It is a physics-regime boundary rather than a tolerance-derived one; deriving it from the handoff tolerance gives roughly 450 km, which would make a 200 km orbit permanently un-warpable. The reasoning is in [`docs/architecture.md`](../architecture.md) and Finding 1 of A3's evidence index. Final tuning belongs to P2/M5.
- **The handoff behaviour across that boundary** is the transition contract recorded in [`docs/architecture.md`](../architecture.md).

## Sources

- NASA documents the patched-conic approximation and sphere-of-influence method in its [Basics of Space Flight, Interplanetary Trajectories](https://science.nasa.gov/learn/basics-of-space-flight/chapter4-1/).
- Vallado, *Fundamentals of Astrodynamics and Applications*, covers the Laplace sphere-of-influence formulation and the accuracy limits of patched-conic methods.
- Reference body states and gravitational parameters come from the DE440/DE441 data pinned in [ADR 0008](0008-astronomical-reference-data-and-time-boundary.md).
