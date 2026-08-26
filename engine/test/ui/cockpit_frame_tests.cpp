#include <cmath>

#include <sol/core/math/scalar.hpp>
#include <sol/test/test.hpp>
#include <sol/ui/cockpit_frame.hpp>
#include <sol/ui/radar_projection.hpp>

using sol::core::Quat;
using sol::core::Vec2;
using sol::core::Vec3;
using sol::ui::cockpitFrame;
using sol::ui::HudFrame;
using sol::ui::screenFrame;

namespace {

bool nearlyEqual(float a, float b, float tolerance = 1.0e-3f)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < tolerance;
}

constexpr Vec2 kScreen = {1280.0f, 720.0f};
constexpr float kMargin = 24.0f;
// The game's own vertical field of view (game/src/scene_renderer.hpp).
const float kTanHalfFov = std::tan(sol::core::radians(70.0f) * 0.5f);

Quat yaw(float degrees)
{
    return sol::core::fromAxisAngle({0.0f, 1.0f, 0.0f}, sol::core::radians(degrees));
}

Quat pitch(float degrees)
{
    return sol::core::fromAxisAngle({1.0f, 0.0f, 0.0f}, sol::core::radians(degrees));
}

} // namespace

SOL_TEST(screen_frame_is_the_layout_the_hud_had_before_the_cockpit)
{
    const Vec2 discCentre = sol::ui::radarCenter(kScreen, kMargin);
    const HudFrame frame = screenFrame(kScreen, kMargin, discCentre);

    SOL_CHECK(!frame.cockpit);
    // Bottom-left and bottom-right corners, one margin in, exactly as
    // drawFlightPanel and drawPowerPanel placed themselves by hand.
    SOL_CHECK(frame.leftConsole.visible);
    SOL_CHECK(nearlyEqual(frame.leftConsole.position.x, kMargin));
    SOL_CHECK(nearlyEqual(frame.leftConsole.position.y, kScreen.y - kMargin));
    SOL_CHECK(nearlyEqual(frame.rightConsole.position.x, kScreen.x - kMargin));
    SOL_CHECK(nearlyEqual(frame.rightConsole.position.y, kScreen.y - kMargin));
    SOL_CHECK(nearlyEqual(frame.centreConsole.position.x, discCentre.x));
    SOL_CHECK(nearlyEqual(frame.centreConsole.position.y, discCentre.y));

    // Nothing to be occluded by: the whole screen is glass.
    SOL_CHECK(!frame.apertureEmpty());
    SOL_CHECK(frame.insideAperture({1.0f, 1.0f}));
    SOL_CHECK(frame.insideAperture({kScreen.x - 1.0f, kScreen.y - 1.0f}));
}

SOL_TEST(cockpit_anchors_land_on_the_dash_below_the_glass)
{
    const HudFrame frame = cockpitFrame(Quat::identity(), kScreen, kTanHalfFov);
    SOL_CHECK(frame.cockpit);
    SOL_CHECK(frame.leftConsole.visible && frame.rightConsole.visible && frame.centreConsole.visible);

    // Symmetric about the boresight, and the centre console is on it.
    const float centreX = kScreen.x * 0.5f;
    SOL_CHECK(frame.leftConsole.position.x < centreX);
    SOL_CHECK(frame.rightConsole.position.x > centreX);
    SOL_CHECK(nearlyEqual(centreX - frame.leftConsole.position.x, frame.rightConsole.position.x - centreX));
    SOL_CHECK(nearlyEqual(frame.centreConsole.position.x, centreX));

    // Every mount point is below the glass and on the screen: a panel pinned
    // to one of them sits on the dash rather than over the view.
    SOL_CHECK(frame.leftConsole.position.y > frame.apertureMax.y);
    SOL_CHECK(frame.rightConsole.position.y > frame.apertureMax.y);
    SOL_CHECK(frame.centreConsole.position.y > frame.apertureMax.y);
    SOL_CHECK(frame.leftConsole.position.y <= kScreen.y);
    SOL_CHECK(frame.rightConsole.position.y <= kScreen.y);

    // The consoles are further out than the disc is wide, so the flight panel
    // and the power block cannot collide with the radar between them.
    SOL_CHECK(centreX - frame.leftConsole.position.x > sol::ui::kRadarRadius);
}

SOL_TEST(the_disc_and_its_range_label_stay_on_screen_at_every_ui_scale)
{
    // The projection takes its focal length from the screen HEIGHT, so raising
    // the UI scale shortens the virtual screen and drags the whole dash down
    // it. The disc is the piece that runs out of room first, because it is 78
    // px of radius with a range label underneath, and a label printed off the
    // bottom edge is a disc whose scale is a secret. 1.25 and 2.0 are the
    // scales the settings screen actually offers.
    constexpr float kDiscReach = sol::ui::kRadarRadius + sol::ui::kRadarLabelBand;
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const Vec2 screen = {1280.0f / scale, 720.0f / scale};
        const HudFrame frame = cockpitFrame(Quat::identity(), screen, kTanHalfFov);
        SOL_CHECK(frame.centreConsole.visible);
        SOL_CHECK(frame.centreConsole.position.y + kDiscReach <= screen.y);
        // ...and the flight panel hangs below it without leaving the screen.
        SOL_CHECK(frame.leftConsole.position.y <= screen.y);
    }
}

SOL_TEST(the_aperture_is_the_glass_and_leaves_the_boresight_in_it)
{
    const HudFrame frame = cockpitFrame(Quat::identity(), kScreen, kTanHalfFov);
    SOL_CHECK(!frame.apertureEmpty());

    // The frame eats screen on all four sides, but the crosshair is in the
    // clear looking straight ahead.
    SOL_CHECK(frame.apertureMin.x > 0.0f);
    SOL_CHECK(frame.apertureMin.y > 0.0f);
    SOL_CHECK(frame.apertureMax.x < kScreen.x);
    SOL_CHECK(frame.apertureMax.y < kScreen.y);
    SOL_CHECK(frame.insideAperture({kScreen.x * 0.5f, kScreen.y * 0.5f}));

    // Symmetric, and wider than it is tall - it is a canopy, not a letterbox.
    SOL_CHECK(nearlyEqual(frame.apertureMin.x, kScreen.x - frame.apertureMax.x));
    SOL_CHECK(frame.apertureMax.x - frame.apertureMin.x > frame.apertureMax.y - frame.apertureMin.y);
}

SOL_TEST(the_frame_scales_with_the_screen_rather_than_being_written_in_pixels)
{
    const HudFrame small = cockpitFrame(Quat::identity(), kScreen, kTanHalfFov);
    const Vec2 big = {kScreen.x * 1.5f, kScreen.y * 1.5f};
    const HudFrame large = cockpitFrame(Quat::identity(), big, kTanHalfFov);

    // Same field of view, one and a half times the screen: every projected
    // point is one and a half times as far from the centre. This is the whole
    // reason the anchors are projected instead of written down.
    const auto fromCentre = [](const Vec2& point, const Vec2& size) {
        return Vec2{point.x - size.x * 0.5f, point.y - size.y * 0.5f};
    };
    const Vec2 smallLeft = fromCentre(small.leftConsole.position, kScreen);
    const Vec2 largeLeft = fromCentre(large.leftConsole.position, big);
    SOL_CHECK(nearlyEqual(largeLeft.x, smallLeft.x * 1.5f, 0.05f));
    SOL_CHECK(nearlyEqual(largeLeft.y, smallLeft.y * 1.5f, 0.05f));
}

SOL_TEST(the_frame_sits_at_fixed_angles_so_a_narrower_view_sees_less_of_it)
{
    const HudFrame wide = cockpitFrame(Quat::identity(), kScreen, kTanHalfFov);
    const HudFrame narrow =
        cockpitFrame(Quat::identity(), kScreen, std::tan(sol::core::radians(50.0f) * 0.5f));

    // A canopy pillar is at a fixed ANGLE off the nose, not at a fixed pixel.
    // Narrowing the field of view narrows the cone until the pillars fall
    // outside it altogether, so there is MORE glass on screen, not less - and
    // the dash, which is much closer to the boresight, is still there.
    SOL_CHECK(narrow.apertureMax.x - narrow.apertureMin.x > wide.apertureMax.x - wide.apertureMin.x);
    SOL_CHECK(narrow.apertureMax.y < kScreen.y); // the shelf survives the zoom

    // The consoles are the same story from the other side: magnified with
    // everything else, so they spread further apart.
    const float centreX = kScreen.x * 0.5f;
    SOL_CHECK(centreX - narrow.leftConsole.position.x > centreX - wide.leftConsole.position.x);
}

SOL_TEST(turning_the_head_sweeps_the_frame_across_the_view)
{
    const HudFrame ahead = cockpitFrame(Quat::identity(), kScreen, kTanHalfFov);
    // Free-look to the LEFT: the cockpit swings right across the screen, the
    // same way the world does.
    const HudFrame left = cockpitFrame(yaw(25.0f), kScreen, kTanHalfFov);
    SOL_CHECK(left.leftConsole.position.x > ahead.leftConsole.position.x);
    SOL_CHECK(left.centreConsole.position.x > ahead.centreConsole.position.x);
    SOL_CHECK(left.apertureMin.x >= ahead.apertureMin.x);

    // Looking UP drops the dash toward the bottom of the screen.
    const HudFrame up = cockpitFrame(pitch(20.0f), kScreen, kTanHalfFov);
    SOL_CHECK(up.centreConsole.position.y > ahead.centreConsole.position.y);
}

SOL_TEST(looking_away_from_the_glass_leaves_no_glass_to_draw_in)
{
    // Ninety degrees off the nose is the side window, and there is not one:
    // an edge of the opening is behind the eye, so the aperture collapses and
    // every world-referenced marker clips away with it.
    const HudFrame side = cockpitFrame(yaw(100.0f), kScreen, kTanHalfFov);
    SOL_CHECK(side.apertureEmpty());
    SOL_CHECK(!side.insideAperture({kScreen.x * 0.5f, kScreen.y * 0.5f}));

    // Straight back is the bulkhead: the mount points are behind the eye too,
    // and must report themselves invisible rather than projecting mirrored
    // onto the wrong side of the screen.
    const HudFrame behind = cockpitFrame(yaw(180.0f), kScreen, kTanHalfFov);
    SOL_CHECK(!behind.leftConsole.visible);
    SOL_CHECK(!behind.rightConsole.visible);
    SOL_CHECK(!behind.centreConsole.visible);
    SOL_CHECK(behind.apertureEmpty());
}

SOL_TEST(the_aperture_never_leaves_the_screen)
{
    // Whatever the head is doing, a panel pinned to a corner of the aperture
    // must still be on the screen the player is looking at.
    for (float degrees = -80.0f; degrees <= 80.0f; degrees += 10.0f) {
        const HudFrame frame = cockpitFrame(yaw(degrees), kScreen, kTanHalfFov);
        if (frame.apertureEmpty()) {
            continue;
        }
        SOL_CHECK(frame.apertureMin.x >= 0.0f);
        SOL_CHECK(frame.apertureMin.y >= 0.0f);
        SOL_CHECK(frame.apertureMax.x <= kScreen.x);
        SOL_CHECK(frame.apertureMax.y <= kScreen.y);
    }
}
