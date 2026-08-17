#pragma once

#include "space_world.hpp"

#include "sol/core/math/math.hpp"

#include <cmath>

namespace game {

enum class CameraMode
{
    // Named for what it is since Phase 8m, which is also what the HUD's mode
    // line has been printing for it since 8d.
    Cockpit,
    ThirdPerson,
    Free,
};

// Ship-anchored camera rig: cockpit view locked to the hull, chase view with
// smoothed orientation so the ship visibly banks inside the frame.
class ShipCamera
{
public:
    // Free-look limits (Phase 8m). Far enough round to check your six, not so
    // far that the pilot's head turns through the seat.
    static constexpr float kMaxLookYaw = sol::core::radians(120.0f);
    static constexpr float kMaxLookPitch = sol::core::radians(60.0f);

    // Turns the eye inside the cockpit while the Free Look binding is held and
    // eases it back to the boresight when it is released. The head rotates;
    // the eye does not move, and neither does the ship - `headOffset` is a
    // camera-space rotation applied on top of the hull's orientation, so
    // flight input, weapons and autopilot never see it.
    void updateLook(sol::core::Vec2 mouseDelta, bool looking, float deltaSeconds,
                    float sensitivity, bool invertPitch)
    {
        if (looking) {
            const float scale = kLookSensitivity * sensitivity;
            const float pitchDelta = invertPitch ? -mouseDelta.y : mouseDelta.y;
            m_lookYaw -= mouseDelta.x * scale; // mouse right = look right
            m_lookPitch -= pitchDelta * scale; // mouse up = look up
            m_lookYaw = sol::core::clamp(m_lookYaw, -kMaxLookYaw, kMaxLookYaw);
            m_lookPitch = sol::core::clamp(m_lookPitch, -kMaxLookPitch, kMaxLookPitch);
        } else if (m_lookYaw != 0.0f || m_lookPitch != 0.0f) {
            // The same exponential the chase camera settles on, so letting go
            // of the key reads like the head coming back rather than a cut.
            const float recenter = std::exp(-kLookRecenterRate * deltaSeconds);
            m_lookYaw *= recenter;
            m_lookPitch *= recenter;
            if (std::abs(m_lookYaw) < 0.0005f && std::abs(m_lookPitch) < 0.0005f) {
                m_lookYaw = 0.0f;
                m_lookPitch = 0.0f;
            }
        }
    }

    // The eye's rotation within the cockpit. Identity means looking straight
    // down the nose, which is the only state that existed before free-look.
    [[nodiscard]] sol::core::Quat headOffset() const
    {
        if (m_lookYaw == 0.0f && m_lookPitch == 0.0f) {
            return sol::core::Quat::identity();
        }
        return sol::core::fromAxisAngle({0.0f, 1.0f, 0.0f}, m_lookYaw) *
               sol::core::fromAxisAngle({1.0f, 0.0f, 0.0f}, m_lookPitch);
    }

    [[nodiscard]] bool looking() const { return m_lookYaw != 0.0f || m_lookPitch != 0.0f; }

    void snapLookAhead()
    {
        m_lookYaw = 0.0f;
        m_lookPitch = 0.0f;
    }

    [[nodiscard]] CameraFrame cockpit(const Transform& ship) const
    {
        return {
            .position = ship.position + sol::core::toDVec3(rotate(ship.orientation, kCockpitOffset)),
            .orientation = ship.orientation * headOffset(),
        };
    }

    [[nodiscard]] CameraFrame thirdPerson(const Transform& ship, float deltaSeconds)
    {
        const float t = 1.0f - std::exp(-kChaseRate * deltaSeconds);
        m_chaseOrientation = slerp(m_chaseOrientation, ship.orientation, t);
        const sol::core::Vec3 offset = rotate(m_chaseOrientation, kChaseOffset);
        return {
            .position = ship.position + sol::core::toDVec3(offset),
            .orientation = m_chaseOrientation *
                           sol::core::fromAxisAngle({1.0f, 0.0f, 0.0f}, kChasePitch),
        };
    }

    void snapTo(const Transform& ship) { m_chaseOrientation = ship.orientation; }

    // Where the eye sits in ship space. cockpit.gltf is authored around this
    // exact point by tools/scripts/gen_assets.ps1 - move it and the pilot ends
    // up outside their own canopy.
    static constexpr sol::core::Vec3 kCockpitOffset = {0.0f, 0.8f, -5.0f};

private:
    static constexpr sol::core::Vec3 kChaseOffset = {0.0f, 8.0f, 30.0f};
    static constexpr float kChasePitch = -0.16f;
    static constexpr float kChaseRate = 6.0f;
    // Matched to the flight stick's own sensitivity so the two mouse modes
    // feel like the same mouse; the settings multiplier applies to both.
    static constexpr float kLookSensitivity = 0.0035f;
    static constexpr float kLookRecenterRate = 9.0f;

    sol::core::Quat m_chaseOrientation = sol::core::Quat::identity();
    float m_lookYaw = 0.0f;
    float m_lookPitch = 0.0f;
};

} // namespace game
