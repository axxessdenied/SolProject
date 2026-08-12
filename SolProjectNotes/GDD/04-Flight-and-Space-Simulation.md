# 04 — Flight and Space Simulation

## Experience goal

The player should feel that the craft they built is genuinely traveling through one continuous environment. Surface, atmosphere, near-space, orbit, and interplanetary space may use different internal models, but controls, vehicle identity, mission state, and visual continuity persist.

## Direct flight first

Early development prioritizes hands-on flight. Strategic command and mission planning are added after a single craft is satisfying to build and fly. Eventually the player chooses which critical operations to fly personally and which to delegate to crews, procedures, or automation.

The first playable uses keyboard/mouse, an external third-person camera centered on the active craft, flight instrumentation, and an orbital map. The default bindings are remappable:

| Input | Default action |
|---|---|
| W / S | Pitch |
| A / D | Yaw |
| Q / E | Roll |
| Shift / Ctrl | Increase / decrease throttle |
| Z / X | Full / cut throttle |
| Space | Activate next stage |
| T | Toggle stability assist |
| M | Toggle orbital map |
| Right-mouse drag | Orbit the external camera around the active craft |
| Mouse wheel | Zoom the external camera |

Camera orbit and zoom do not command the craft. Cockpit/IVA is planned after the exterior flight experience works. Walking inside ships is deferred beyond the current roadmap. Exact instrument depth still requires definition; gamepad, HOTAS, and other devices are later evaluations.

The world uses real Solar System names, dimensions, orbital distances, and vetted astronomical data, with an initial campaign epoch of 2026-01-01 00:00:00 UTC. Detailed surface content is initially concentrated around a fictional commercial launch facility near 28 degrees north on Florida's Atlantic coast, geographically distinct from real launch complexes, but planetary and orbital scale are not reduced. Exact longitude, terrain placement, and regulatory/operating context remain open.

## Fidelity layers

Constraints can be genuine or simplified according to difficulty. Candidate systems include:

- gravity and orbital trajectories;
- thrust, propellant, mass change, staging, torque, and control authority;
- atmosphere, aerodynamic forces, stability, and heating;
- structure, collision, docking, damage, and recovery;
- power, communication, data transmission, and signal delay;
- life support, radiation, maintenance, and reliability.

Normal difficulty uses genuine orbital mechanics, propulsion, mass change, and resource constraints, with simplified aerodynamics and heating. Communication and life-support constraints are introduced in later layers rather than required by the first playable. Because play begins in 2026, stability hold, attitude-rate damping, throttle hold, heading/prograde/retrograde indicators, maneuver guidance, and staging warnings are standard initial capabilities. Automated launch, maneuver execution, rendezvous/docking, and mission scripting are progression-gated. Accessibility and input aids should not be withheld merely to simulate technological progression. Difficulty should change assists, information, tolerances, and selected constraints in coherent presets; it should not make the same named component obey inexplicably unrelated rules without communicating the difference.

## Hybrid simulation

High-fidelity local physics is reserved for active situations. Stable coasts can use analytical propagation, and later remote operations may use scheduled/aggregate simulation. The player should see why warp or analytical mode is unavailable near collision, atmosphere, staging, docking, or other discontinuities.

## Mission planning

The orbital map should grow from explanation into command: current orbit, predicted trajectory, events/encounters, maneuver planning, resource/time estimates, uncertainty where relevant, alarms, and eventually multi-craft schedules. Planning is not a substitute for flight; it makes longer missions legible.

## Failure

A good early failure is deterministic and attributable: design margin, piloting, environment, or planning. Random manufacturing defects are excluded from the first playable. Crew death and mission-revert options are configurable. Wear, probabilistic reliability, quicksave details, and recovery economics can be designed later.

## Decisions

| Decision | Status | Why |
|---|---|---|
| Direct spacecraft flight before strategic command | Confirmed | User response |
| Seamless surface-to-space experience | Confirmed | User response |
| Hybrid local/analytical simulation | Confirmed | User response |
| Adjustable realism/difficulty | Confirmed | User response |
| Real-scale Solar System with high detail concentrated locally | Confirmed | User accepted recommendation |
| External third-person, instruments, and map first | Confirmed | User accepted recommendation |
| Cockpit/IVA later; walking deferred | Confirmed | User response |
| Normal fidelity baseline described above | Confirmed | User accepted recommendation |
| Deterministic early failures; configurable crew death/revert | Confirmed | User accepted recommendation |
| Keyboard/mouse is the first input target | Confirmed | User response |
| Named initial assistance set above; automated operations through research | Confirmed | User accepted recommendation |
| Remappable default bindings and external orbit-camera behavior above | Confirmed | User accepted recommendation |
| Exact instruments/HUD depth and later device support | Open | Needed before active-flight implementation |
