/// @file
/// A world-space vector, in metres, always double precision.

#pragma once

namespace sol::render {

/// A position or direction in the authoritative world frame, in metres.
///
/// `double` is not negotiable here. P1a increment A2 measured that one ULP of a `double` is
/// 0.98 mm at Neptune's distance — a `float` cannot represent Solar System coordinates at all.
/// Narrowing to `float` happens only after subtracting the camera position, which is what
/// camera-relative rendering means and where the renderer's precision budget is spent.
struct WorldVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

} // namespace sol::render
