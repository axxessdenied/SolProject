#pragma once

#include "Sol/Proto/Orbit/TwoBody.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace sol::proto::orbit {

/// The fixed-step local numerical integrators A3 compares.
///
/// The P1a plan asks for "a candidate fixed-step local numerical integrator" and its time-box
/// trigger says to narrow to a single candidate if the increment overruns. Three are
/// implemented because the choice is not a matter of accuracy alone, and a comparison that
/// varied only the order would not surface the property that actually matters here:
///
///   RungeKutta4    - 4th order, not symplectic. Best short-run accuracy per step, but its
///                    energy error is secular: it accumulates in one direction for as long as
///                    the run lasts. Under ADR 0011 orbits do not decay, so an integrator that
///                    quietly loses energy manufactures exactly the decay the ADR forbids.
///   VelocityVerlet - 2nd order, symplectic. Much larger per-step error, but its energy error
///                    is bounded and oscillatory rather than accumulating.
///   Yoshida4       - 4th order, symplectic, by composing three leapfrog stages. The candidate
///                    that has both properties, at three acceleration evaluations per step.
///
/// A3's reference-orbit gate is one orbit, which is short enough that the secular term barely
/// shows. The gate is therefore not the whole comparison, and the increment reports long-run
/// energy behaviour alongside it.
enum class IntegratorKind : std::uint8_t {
    RungeKutta4,
    VelocityVerlet,
    Yoshida4,
};

/// Every candidate, in a fixed order.
///
/// Fixed rather than incidental: scenarios iterate this array to produce their reports, and
/// ADR 0010's determinism guarantee covers the order results are emitted in as well as their
/// values.
inline constexpr std::array<IntegratorKind, 3> kIntegratorCandidates{
    IntegratorKind::RungeKutta4,
    IntegratorKind::VelocityVerlet,
    IntegratorKind::Yoshida4,
};

[[nodiscard]] std::string_view integratorName(IntegratorKind kind) noexcept;

/// Whether the integrator's error in a conserved quantity is bounded rather than secular.
[[nodiscard]] bool isSymplectic(IntegratorKind kind) noexcept;

/// Acceleration evaluations one step costs.
///
/// The honest cost unit for comparing integrators, since the acceleration is the only
/// non-trivial work in an ADR 0011 step. Comparing candidates at equal step size flatters the
/// cheap ones and comparing at equal wall time hides why; A3 reports both, and this is what
/// makes the equal-cost comparison possible.
///
/// VelocityVerlet is reported as 2 because that is what this implementation performs. A
/// production loop would carry the end-of-step acceleration into the next step and pay 1,
/// which is a real factor-of-two the evidence must not claim by asserting it here.
[[nodiscard]] int accelerationEvaluationsPerStep(IntegratorKind kind) noexcept;

/// Advances @p state by exactly @p stepSeconds under ADR 0011 two-body gravity.
///
/// Fixed-step by construction: no adaptive control, no step rejection, no error estimate
/// feeding back into the step size. Adaptivity is deliberately excluded, because a step size
/// that depends on the trajectory makes the sequence of floating-point operations depend on
/// it too, and ADR 0010's bit-exactness guarantee then holds only for runs that happen to take
/// the same path. Fixed steps make reproducibility structural rather than incidental.
[[nodiscard]] TwoBodyState integrateStep(IntegratorKind kind, const TwoBodyState& state,
                                         double gravitationalParameter,
                                         double stepSeconds) noexcept;

} // namespace sol::proto::orbit
