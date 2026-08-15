#pragma once

#include "space_world.hpp"

#include "sol/core/math/math.hpp"

#include <cmath>

namespace game {

enum class CameraMode
{
    FirstPerson,
    ThirdPerson,
    Free,
};

// Ship-anchored camera rig: cockpit view locked to the hull, chase view with
// smoothed orientation so the ship visibly banks inside the frame.
class ShipCamera
{
public:
    [[nodiscard]] CameraFrame firstPerson(const Transform& ship) const
    {
        return {
            .position = ship.position + sol::core::toDVec3(rotate(ship.orientation, kCockpitOffset)),
            .orientation = ship.orientation,
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

private:
    static constexpr sol::core::Vec3 kCockpitOffset = {0.0f, 0.8f, -5.0f};
    static constexpr sol::core::Vec3 kChaseOffset = {0.0f, 8.0f, 30.0f};
    static constexpr float kChasePitch = -0.16f;
    static constexpr float kChaseRate = 6.0f;

    sol::core::Quat m_chaseOrientation = sol::core::Quat::identity();
};

} // namespace game
