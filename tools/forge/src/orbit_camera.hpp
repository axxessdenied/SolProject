#pragma once

// Turntable camera for the Forge viewport (engine plan Phase 9 stage C).
//
// ⚑ Every control here is scaled by the current distance, because the range
// this camera has to cover is four orders of magnitude wide: a cockpit
// instrument is 0.2 m across and a station is 200. A linear dolly that felt
// right at the station would take a thousand notches to reach the instrument,
// and a linear pan would throw it off screen.

#include "sol/core/math/math.hpp"

#include <cmath>

namespace forge {

class OrbitCamera
{
public:
    void orbit(float yawDelta, float pitchDelta)
    {
        m_yaw += yawDelta;
        // Short of straight up: at the pole the up vector and the view
        // direction are parallel and lookAt's cross product collapses.
        constexpr float kPitchLimit = sol::core::kHalfPi - 0.02f;
        m_pitch = sol::core::clamp(m_pitch + pitchDelta, -kPitchLimit, kPitchLimit);
    }

    // Pixel drag to world movement of the orbit target, exact at the target's
    // depth - so the thing under the cursor stays under it.
    void pan(float rightPixels, float upPixels, float viewportHeight, float verticalFov)
    {
        if (viewportHeight <= 0.0f) {
            return;
        }
        const float worldPerPixel =
            2.0f * m_distance * std::tan(verticalFov * 0.5f) / viewportHeight;
        const sol::core::Vec3 forward = -direction();
        const sol::core::Vec3 right =
            sol::core::normalize(sol::core::cross(forward, {0.0f, 1.0f, 0.0f}));
        const sol::core::Vec3 up = sol::core::cross(right, forward);
        m_target = m_target - right * (rightPixels * worldPerPixel) +
                   up * (upPixels * worldPerPixel);
    }

    void dolly(float notches)
    {
        constexpr float kPerNotch = 1.12f;
        m_distance = sol::core::clamp(m_distance * std::pow(kPerNotch, -notches), kMinDistance,
                                      kMaxDistance);
    }

    // Fits a bounding sphere in the vertical field of view, with a margin so
    // the silhouette is not touching the edge of the frame.
    void frame(sol::core::Vec3 center, float radius, float verticalFov)
    {
        m_target = center;
        const float fitted = radius > 0.0f ? radius / std::sin(verticalFov * 0.5f) : kMinDistance;
        m_distance = sol::core::clamp(fitted * 1.35f, kMinDistance, kMaxDistance);
    }

    [[nodiscard]] sol::core::Vec3 target() const { return m_target; }
    [[nodiscard]] float distance() const { return m_distance; }
    [[nodiscard]] sol::core::Vec3 eye() const { return m_target + direction() * m_distance; }

    [[nodiscard]] sol::core::Mat4 view() const
    {
        return sol::core::lookAt(eye(), m_target, {0.0f, 1.0f, 0.0f});
    }

private:
    // 2 cm to 20 km: closer than the near plane is pointless, and further than
    // this is past anything this engine authors as a single mesh.
    static constexpr float kMinDistance = 0.02f;
    static constexpr float kMaxDistance = 20'000.0f;

    // Target-to-eye, unit length.
    [[nodiscard]] sol::core::Vec3 direction() const
    {
        const float cosPitch = std::cos(m_pitch);
        return {cosPitch * std::sin(m_yaw), std::sin(m_pitch), cosPitch * std::cos(m_yaw)};
    }

    sol::core::Vec3 m_target{};
    float m_distance = 20.0f;
    float m_yaw = 0.7f;
    float m_pitch = 0.35f;
};

} // namespace forge
