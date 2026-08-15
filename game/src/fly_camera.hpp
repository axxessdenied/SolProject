#pragma once

#include "sol/core/math/math.hpp"
#include "sol/platform/window.hpp"

namespace game {

// Debug free-fly camera: hold RMB to mouse-look, WASD + Space/Ctrl to move,
// Shift for speed boost, wheel to scale base speed.
class FlyCamera
{
public:
    void update(sol::platform::Window& window, float deltaSeconds)
    {
        using sol::platform::Key;
        using sol::platform::MouseButton;

        const bool looking = window.isMouseButtonDown(MouseButton::Right);
        window.setCursorLocked(looking);

        if (looking) {
            const sol::core::Vec2 delta = window.mouseDelta();
            m_yaw -= delta.x * kLookSensitivity;
            m_pitch -= delta.y * kLookSensitivity;
            m_pitch = sol::core::clamp(m_pitch, -sol::core::kHalfPi + 0.01f,
                                       sol::core::kHalfPi - 0.01f);
        }

        const float wheel = window.wheelDelta();
        if (wheel != 0.0f) {
            m_speed = sol::core::clamp(m_speed * (wheel > 0.0f ? 1.3f : 1.0f / 1.3f), 0.1f, 1000.0f);
        }

        sol::core::Vec3 move = {};
        if (window.isKeyDown(Key::W)) move.z -= 1.0f;
        if (window.isKeyDown(Key::S)) move.z += 1.0f;
        if (window.isKeyDown(Key::A)) move.x -= 1.0f;
        if (window.isKeyDown(Key::D)) move.x += 1.0f;
        if (window.isKeyDown(Key::Space)) move.y += 1.0f;
        if (window.isKeyDown(Key::LeftControl)) move.y -= 1.0f;

        if (move != sol::core::Vec3{}) {
            const float boost = window.isKeyDown(Key::LeftShift) ? 5.0f : 1.0f;
            const sol::core::Vec3 world = rotate(orientation(), normalize(move));
            m_position += toDVec3(world * (m_speed * boost * deltaSeconds));
        }
    }

    [[nodiscard]] sol::core::Quat orientation() const
    {
        return sol::core::fromAxisAngle({0.0f, 1.0f, 0.0f}, m_yaw) * sol::core::fromAxisAngle({1.0f, 0.0f, 0.0f}, m_pitch);
    }

    // Rotation-only view matrix; translation is handled camera-relatively.
    [[nodiscard]] sol::core::Mat4 viewRotation() const { return transpose(toMat4(orientation())); }

    [[nodiscard]] sol::core::DVec3 position() const { return m_position; }
    [[nodiscard]] float speed() const { return m_speed; }

private:
    static constexpr float kLookSensitivity = 0.0025f;

    sol::core::DVec3 m_position = {0.0, 2.5, 10.0};
    float m_yaw = 0.0f;
    float m_pitch = -0.15f;
    float m_speed = 6.0f;
};

} // namespace game
