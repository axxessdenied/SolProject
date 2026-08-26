#pragma once

#include "sol/core/math/quat.hpp"
#include "sol/ui/pick.hpp"
#include "sol/ui/radar_projection.hpp"
#include "sol/ui/screens.hpp"

#include <algorithm>
#include <cstddef>

namespace sol::ui {

// Where the flight HUD hangs (engine plan Phase 8m), kept out of the HUD so it
// can be asserted on headlessly — the same arrangement map_projection.hpp,
// radar_projection.hpp and pick.hpp already have: the data contract lives in
// screens.hpp and the maths lives here.
//
// The whole file is one ruling: a HUD panel's position comes from projecting a
// point on the cockpit FORWARDS onto the screen, through the same
// `screenPoint` the target marker and the click-pick already agree through.
// The alternative — writing the panel positions down in pixels, which is what
// the HUD did before there was a cockpit — cannot survive a change of field of
// view, of window size, of UI scale, or a head that turns. Projecting forwards
// makes the panel land on the dash by construction, for the same reason Phase
// 8j gave for the radar disc.
//
// Every point below is measured FROM THE EYE, in cockpit space (+x right,
// +y up, −z forward), and cockpit space is ship space because the cockpit is
// attached to the ship. The same numbers are authored into the mesh by
// tools/scripts/gen_assets.ps1 — change one, change both, or the instruments
// come off the dash they are supposed to be bolted to.

// Anchors are the corner a panel is pinned by, not the middle of a surface:
// the two consoles carry the BOTTOM corner their panel grows upward from, so
// this header never has to know how tall a panel is. That is also what lets an
// external camera answer with the screen corners the HUD used before this
// phase and get pixel-identical output.
inline constexpr core::Vec3 kAnchorLeftConsole = {-0.95f, -0.66f, -1.00f};
inline constexpr core::Vec3 kAnchorRightConsole = {0.95f, -0.66f, -1.00f};
// The disc rides HIGHER on the dash than the two side panels, and the reason is
// arithmetic rather than taste. Everything here is projected through a focal
// length taken from the screen's HEIGHT, so a short virtual screen — a 1.25 UI
// scale on a 720p window is already one — shrinks the dash in pixels while the
// disc stays 78 px of radius plus its range label. At −0.48 the label ran off
// the bottom edge at the game's own default resolution; −0.40 keeps the whole
// disc on screen down to a virtual height of about 500 px, which is past any
// scale the settings screen offers.
inline constexpr core::Vec3 kAnchorCentreConsole = {0.00f, -0.40f, -1.00f};

// The canopy opening, as the four silhouette edges that bound it. Left and
// right are taken at the BROW, where the pillars lean furthest inboard and the
// opening is therefore narrowest; the bottom is the instrument shelf's FRONT
// edge, which is the shallower of its two edges and so the one the player
// actually sees the world above.
inline constexpr core::Vec3 kApertureTop = {0.00f, 0.545f, -0.86f};
inline constexpr core::Vec3 kApertureBottom = {0.00f, -0.32f, -1.01f};
inline constexpr core::Vec3 kApertureLeft = {-0.80f, 0.00f, -0.86f};
inline constexpr core::Vec3 kApertureRight = {0.80f, 0.00f, -0.86f};

// `HudAnchorPoint` and `HudFrame` are the data contract and live in
// screens.hpp with the rest of it; only the numbers and the projection are
// here.

// The layout the HUD had before there was a cockpit, and still has in chase
// and free cameras: panels in the screen's corners, `margin` in from each
// edge, and no frame to be occluded by. `radarCentre` is passed in rather than
// recomputed because radar_projection.hpp owns where the disc goes — Phase 8j
// made a blip clickable, so the draw and the hit test must read one function.
[[nodiscard]] inline HudFrame screenFrame(core::Vec2 screenSize, float margin, core::Vec2 radarCentre)
{
    HudFrame frame;
    frame.cockpit = false;
    frame.leftConsole = {{margin, screenSize.y - margin}, true};
    frame.rightConsole = {{screenSize.x - margin, screenSize.y - margin}, true};
    frame.centreConsole = {radarCentre, true};
    frame.apertureMin = {0.0f, 0.0f};
    frame.apertureMax = screenSize;
    return frame;
}

// The cockpit's own layout. `headToShip` is the free-look rotation of the eye
// within the cockpit — identity when the player is looking straight ahead —
// so a cockpit-space point lands in camera space as
// rotate(conjugate(headToShip), point). The ship's orientation cancels out
// entirely, which is why this function needs nothing from the world: the
// cockpit and the eye are both bolted to the same hull.
[[nodiscard]] inline HudFrame cockpitFrame(core::Quat headToShip, core::Vec2 screenSize, float tanHalfFovY)
{
    const core::Vec2 centre = {screenSize.x * 0.5f, screenSize.y * 0.5f};
    const float focal = focalLength(screenSize.y, tanHalfFovY);
    const core::Quat shipToHead = conjugate(headToShip);
    const auto project = [&](const core::Vec3& cockpitPoint) {
        const ScreenPoint point = screenPoint(rotate(shipToHead, cockpitPoint), centre, focal);
        return HudAnchorPoint{point.position, point.inFront};
    };

    HudFrame frame;
    frame.cockpit = true;
    frame.leftConsole = project(kAnchorLeftConsole);
    frame.rightConsole = project(kAnchorRightConsole);
    frame.centreConsole = project(kAnchorCentreConsole);

    // The one place this header gives up on the projection. A 2.0 UI scale
    // leaves a 360 px virtual screen, and the disc is 78 px of radius plus a
    // 20 px range label — past a point the dash simply projects below the
    // bottom edge, and a range label that is not drawn is a disc whose scale is
    // a secret. Clamping is screen-relative, which is exactly what the rest of
    // the frame avoids, so it is written as a floor rather than a rule: with
    // the head straight it engages only past a 1.5 UI scale, and everywhere
    // else the disc still slides freely with the dash.
    const float discFloor = screenSize.y - (kRadarRadius + kRadarLabelBand);
    frame.centreConsole.position.y = std::min(frame.centreConsole.position.y, discFloor);

    const HudAnchorPoint top = project(kApertureTop);
    const HudAnchorPoint bottom = project(kApertureBottom);
    const HudAnchorPoint left = project(kApertureLeft);
    const HudAnchorPoint right = project(kApertureRight);
    if (!top.visible || !bottom.visible || !left.visible || !right.visible) {
        // Looking far enough off the nose that an edge of the opening is
        // behind the eye: there is no glass in front of the player, so the
        // aperture collapses and everything that clips to it disappears. An
        // empty clip rect DROPS geometry (the Phase 8d zero-area rule), which
        // is exactly the behaviour wanted here.
        frame.apertureMin = {};
        frame.apertureMax = {};
        return frame;
    }
    frame.apertureMin = {std::min(left.position.x, right.position.x),
                         std::min(top.position.y, bottom.position.y)};
    frame.apertureMax = {std::max(left.position.x, right.position.x),
                         std::max(top.position.y, bottom.position.y)};
    // Clamp to the screen so a panel anchored to a corner of the aperture
    // cannot be pushed off it by an opening that projects wider than the view.
    frame.apertureMin.x = std::max(frame.apertureMin.x, 0.0f);
    frame.apertureMin.y = std::max(frame.apertureMin.y, 0.0f);
    frame.apertureMax.x = std::min(frame.apertureMax.x, screenSize.x);
    frame.apertureMax.y = std::min(frame.apertureMax.y, screenSize.y);
    return frame;
}

} // namespace sol::ui
