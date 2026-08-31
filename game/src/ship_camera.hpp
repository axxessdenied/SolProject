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
    void updateLook(
        sol::core::Vec2 mouseDelta, bool looking, float deltaSeconds, float sensitivity, bool invertPitch)
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

    // ⚑⚑⚑ THE CHASE RIG STANDS OFF BY THE SIZE OF WHAT IT IS CHASING (Phase 32
    // stage B), AND THE NUMBER IT SCALES BY IS THE HULL'S RADIUS IN METRES -
    // NOT ITS `scale`. The two are the same figure on every hull shipped today,
    // because all three share one mesh with one authored radius, so no test
    // against committed content can tell them apart. They come apart on the
    // very day this phase exists for: a real mesh is authored at true length
    // and flies at `scale = 1.0` with its size in `models.toml`, and a rig
    // keyed on `scale` would frame a 45 m hull exactly like the 12 m one.
    // `ShipDef::scale` is the placeholder stretcher that makes one mesh do duty
    // for three ships; the radius survives it.
    //
    // ⚑⚑ AND IT IS THE MEASURED GEOMETRY RATHER THAN THE AUTHORED `class`,
    // which is the exact inverse of stage A's ruling and for the same reason.
    // There the class is the intent and the mesh the placeholder, so no button
    // rewrites a class to agree with a stand-in. Here the camera has to frame
    // WHAT IS ACTUALLY DRAWN: the shuttle declares `class = "light"` (20-45 m)
    // and measures 12, so framing it by its class would leave the ship the game
    // opens in as a speck a hundred metres off.
    //
    // ⚑ Linear in the radius, so the hull subtends the same slice of the frame
    // at any size - which is what "framed like a class-3 hull" means. The
    // reference is the shuttle's own 8 m, so that hull's view is unchanged to
    // the bit, and a hull with no model def at all takes the same 8 m fallback
    // `modelBaseRadius` hands out and is therefore also unchanged.
    [[nodiscard]] static sol::core::Vec3 chaseOffset(float hullRadius)
    {
        const float k = hullRadius > 0.0f ? hullRadius / kChaseReferenceRadius : 1.0f;
        return kChaseOffset * k;
    }

    [[nodiscard]] CameraFrame thirdPerson(const Transform& ship, float hullRadius, float deltaSeconds)
    {
        const float t = 1.0f - std::exp(-kChaseRate * deltaSeconds);
        m_chaseOrientation = slerp(m_chaseOrientation, ship.orientation, t);
        const sol::core::Vec3 offset = rotate(m_chaseOrientation, chaseOffset(hullRadius));
        return {
            .position = ship.position + sol::core::toDVec3(offset),
            .orientation = m_chaseOrientation * sol::core::fromAxisAngle({1.0f, 0.0f, 0.0f}, kChasePitch),
        };
    }

    void snapTo(const Transform& ship) { m_chaseOrientation = ship.orientation; }

    // Where the eye sits in ship space. assets/meshes/cockpit.forge is authored
    // around this exact point - its header says so and every coordinate in it is
    // this offset plus something. Move it and the pilot ends up outside their
    // own canopy.
    static constexpr sol::core::Vec3 kCockpitOffset = {0.0f, 0.8f, -5.0f};

    // The rig, as tuned for the shuttle. `kChaseOffset` is the stand-off at
    // `kChaseReferenceRadius`; `chaseOffset()` above is the only thing that
    // should read it. Public because the framing they produce is a claim worth
    // asserting - `game.unit` checks every shipped hull fits inside
    // `kCameraVerticalFov` from where these put the eye.
    static constexpr sol::core::Vec3 kChaseOffset = {0.0f, 8.0f, 30.0f};
    static constexpr float kChasePitch = -0.16f;
    // The shuttle's `ship` model radius from models.toml. ⚑ It is a hand-authored
    // COLLISION sphere rather than a measurement - deliberately a little larger
    // than the mesh, since "larger than what you can hit is safe" - and that is
    // the harmless direction here too: a generous sphere stands the camera off
    // slightly further than it strictly needs to.
    static constexpr float kChaseReferenceRadius = 8.0f;

private:
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
