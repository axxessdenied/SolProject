#pragma once

// The camera a viewport tool picks and drags against (engine plan Phase 31
// stage D).
//
// ⚑⚑ NOTHING HERE IS NEW. Every function below was written for stage E1 and
// lived in `point_tool.cpp`'s anonymous namespace for eleven stages, because
// there was exactly one tool that pointed at the viewport. Stage D is the
// second, and the file it was promoted out of says why copying it would be
// wrong in its own words: *"Two expressions of one thing is how a drag ends up
// going somewhere other than where the cursor went."* A mount is dragged
// through the same pixels a vertex is.
//
// ⚑ THE BASIS IS READ OUT OF THE VIEW MATRIX RATHER THAN REBUILT FROM THE ORBIT
// CAMERA'S YAW AND PITCH. Both tools already depend on the view matrix for the
// projection, so they may as well depend on it for the drag - and the orbit
// camera is then free to move for reasons neither tool knows about (`F` frames
// the mesh, a reframe follows an open) without either of them going stale.
//
// ⚑ Pure math over `sol::core` and `sol::ui`. No ImGui, no device, no document -
// so `sol_forge_tests` can reach it, which is the same line `part_pick.hpp` and
// `list_layout.hpp` were promoted under.

#include "sol/core/math/math.hpp"
#include "sol/ui/pick.hpp"

#include <cmath>

namespace forge {

// ⚑ ONE FRAME'S VIEWPORT STATE, FILLED ONCE IN `main.cpp` AND HANDED TO EVERY
// TOOL THAT POINTS AT IT (promoted here at Phase 31 stage D; it was
// `PointTool::Viewport` from stage E1). Two tools reading two structs is two
// answers to "where is the cursor and who owns the button", and the arbitration
// between them is the first thing that goes wrong when they disagree.

struct ViewportInput
{
    sol::core::Vec2 cursor{};      // window pixels
    sol::core::Vec2 cursorDelta{}; // this frame's mouse movement, pixels
    sol::core::Vec2 center{};      // window centre, pixels
    float focal = 1.0f;            // pick.hpp's focalLength for this window
    float height = 1.0f;           // viewport height in pixels
    float verticalFov = 1.0f;      // radians
    float cameraDistance = 1.0f;   // for sizing the markers
    sol::core::Mat4 view = sol::core::Mat4::identity();
    bool leftPressed = false; // went down this frame
    bool leftDown = false;
    // ⚑ Stage O2. Raw input like the two above, carried because the MIDDLE
    // button pans and a pan moves the eye exactly as an orbit does - so a
    // hover frozen only for LMB would still skip parts under a middle drag.
    // Deliberately NOT gated on `previewLevel` the way the left button is:
    // that gate exists to withhold a PRESS from the tool, and this is not a
    // press, it is "the camera is moving".
    bool middleDown = false;
    bool uiCaptured = false; // ImGui wants the mouse: the viewport gets nothing
    // -1 free in the view plane, 0/1/2 to lock the drag to world X/Y/Z.
    int axisLock = -1;
};

// The view matrix takes world to camera, so its rotation ROWS are the camera's
// axes in world space. Column-major storage (`m[column * 4 + row]`) puts row 0
// at m[0], m[4], m[8].
[[nodiscard]] inline sol::core::Vec3 cameraRight(const sol::core::Mat4& view)
{
    return {view.m[0], view.m[4], view.m[8]};
}

[[nodiscard]] inline sol::core::Vec3 cameraUp(const sol::core::Mat4& view)
{
    return {view.m[1], view.m[5], view.m[9]};
}

// ⚑ Row 2 is the camera's BACKWARD axis, not its forward one - the camera looks
// down -Z, so +Z_camera points at the viewer. Adding a little of it to a world
// point pulls that point toward the camera, which is what an overlay needs and
// the reason this is not simply `-cameraForward`.
[[nodiscard]] inline sol::core::Vec3 cameraBackward(const sol::core::Mat4& view)
{
    return {view.m[2], view.m[6], view.m[10]};
}

// The camera's world-space eye. The view matrix maps world to camera as
// `R * (p - eye)` with the rotation stored by rows, so the translation column
// holds `-R * eye` and the eye comes back by applying R's transpose.
[[nodiscard]] inline sol::core::Vec3 cameraEye(const sol::core::Mat4& view)
{
    const sol::core::Vec3 right = cameraRight(view);
    const sol::core::Vec3 up = cameraUp(view);
    const sol::core::Vec3 backward = cameraBackward(view);
    return {-((view.m[12] * right.x) + (view.m[13] * up.x) + (view.m[14] * backward.x)),
            -((view.m[12] * right.y) + (view.m[13] * up.y) + (view.m[14] * backward.y)),
            -((view.m[12] * right.z) + (view.m[13] * up.z) + (view.m[14] * backward.z))};
}

// The world-space direction the cursor points along.
//
// ⚑ `rayDirectionCamera` is the one place in this programme that runs a
// projection backwards. It is allowed because a pixel names a DIRECTION
// exactly; what `pick.hpp` forbids is inverting to recover a POSITION, which
// would need a depth nobody has. `ui.unit` pins the round trip against
// `screenPoint`, so neither sign is trusted alone.
[[nodiscard]] inline sol::core::Vec3
cameraRay(const sol::core::Mat4& view, sol::core::Vec2 cursor, sol::core::Vec2 center, float focal)
{
    const sol::core::Vec3 local = sol::ui::rayDirectionCamera(cursor, center, focal);
    const sol::core::Vec3 right = cameraRight(view);
    const sol::core::Vec3 up = cameraUp(view);
    const sol::core::Vec3 backward = cameraBackward(view);
    // local.z is exactly -1, so this is `u*right + v*up - backward`.
    return {(local.x * right.x) + (local.y * up.x) + (local.z * backward.x),
            (local.x * right.y) + (local.y * up.y) + (local.z * backward.y),
            (local.x * right.z) + (local.y * up.z) + (local.z * backward.z)};
}

// Distance in pixels from `cursor` to the segment [a, b].
//
// ⚑ This is a whole hit test on its own, and it is why edge picking did NOT
// need Moller-Trumbore: a segment is a segment on screen once both ends are
// projected, so the test is two dimensional. A ray is what an INTERIOR needs.
//
// A segment whose ends project to one pixel degenerates to a point, which is
// the answer rather than a division by zero.
[[nodiscard]] inline float distanceToSegment(sol::core::Vec2 a, sol::core::Vec2 b, sol::core::Vec2 cursor)
{
    const float runX = b.x - a.x;
    const float runY = b.y - a.y;
    const float lengthSquared = runX * runX + runY * runY;
    float along = 0.0f;
    if (lengthSquared > 1e-6f) {
        along = ((cursor.x - a.x) * runX + (cursor.y - a.y) * runY) / lengthSquared;
        along = along < 0.0f ? 0.0f : (along > 1.0f ? 1.0f : along);
    }
    const float dx = cursor.x - (a.x + runX * along);
    const float dy = cursor.y - (a.y + runY * along);
    return std::sqrt(dx * dx + dy * dy);
}

// ⚑ A DRAG'S DEPTH IS FIXED AT THE MOMENT IT STARTS, and this is the conversion
// that spends it: pixels of cursor movement become metres in the view plane at
// that depth. Recomputing the depth per frame makes the thing being dragged
// accelerate as it moves toward the camera and stall as it moves away, which
// reads as the tool fighting the hand.
//
// The same expression `OrbitCamera::pan` uses, at a different depth - the camera
// pans at its target's, a drag moves at the grabbed thing's.
[[nodiscard]] inline float metresPerPixel(float depth, float verticalFov, float height)
{
    const float safe = depth > 0.001f ? depth : 0.001f;
    return height > 1e-6f ? 2.0f * safe * std::tan(verticalFov * 0.5f) / height : 0.0f;
}

// How far a grabbed point moves in the world for this frame's cursor movement,
// with `axisLock` of -1 for the free view plane or 0/1/2 for world X/Y/Z.
[[nodiscard]] inline sol::core::Vec3 dragDelta(const sol::core::Mat4& view,
                                               sol::core::Vec2 cursorDelta,
                                               float depth,
                                               float verticalFov,
                                               float height,
                                               int axisLock)
{
    const float scale = metresPerPixel(depth, verticalFov, height);
    const sol::core::Vec3 right = cameraRight(view);
    const sol::core::Vec3 up = cameraUp(view);
    // Screen Y grows downward and world up is up, hence the sign on the second.
    sol::core::Vec3 delta = (right * (cursorDelta.x * scale)) - (up * (cursorDelta.y * scale));
    if (axisLock < 0 || axisLock > 2) {
        return delta;
    }
    // ⚑ The component of the view-plane move ALONG the locked world axis, and
    // nothing else. So a lock is not "the hand only moves one way" - the hand
    // moves wherever it likes and the point takes the part of that which points
    // along the axis, which is why an axis the camera is looking straight down
    // yields nothing: no cursor movement in the view plane means "toward the
    // eye", and inventing a number there would move the point by an amount the
    // hand never expressed.
    const float along = axisLock == 0 ? delta.x : (axisLock == 1 ? delta.y : delta.z);
    return {axisLock == 0 ? along : 0.0f, axisLock == 1 ? along : 0.0f, axisLock == 2 ? along : 0.0f};
}

} // namespace forge
