#include <sol/test/test.hpp>
#include <sol/ui/radar_projection.hpp>

using sol::core::Vec2;
using sol::core::Vec3;
using sol::ui::radarBeyondRange;
using sol::ui::radarPoint;
using sol::ui::radarRadius;
using sol::ui::radarRange;
using sol::ui::radarStalk;

namespace {

bool nearlyEqual(float a, float b, float tolerance = 1.0e-3f)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < tolerance;
}

constexpr Vec2 kCenter = {100.0f, 200.0f};
constexpr float kDisc = 50.0f;

} // namespace

SOL_TEST(radar_radius_is_monotonic_and_zero_at_the_ship)
{
    SOL_CHECK(nearlyEqual(radarRadius(0.0), 0.0f));
    // The curve has to keep ordering across the whole playfield: a rock at
    // 800 m, a station at 1e8 m and a star at 1e11 m must stay in that order
    // on the disc, which is the entire reason it is not linear.
    SOL_CHECK(radarRadius(800.0) < radarRadius(1.0e8));
    SOL_CHECK(radarRadius(1.0e8) < radarRadius(1.0e11));
    // Sign is irrelevant: distance is distance.
    SOL_CHECK(nearlyEqual(radarRadius(-5000.0), radarRadius(5000.0)));
}

SOL_TEST(radar_bearing_puts_forward_at_the_top_of_the_disc)
{
    const float range = radarRange(1.0e5);

    // Ship space: -z forward, +x right, +y up. Screen y grows downward, so
    // dead ahead has to draw ABOVE the disc center, not below it.
    const Vec2 ahead = radarPoint({0.0f, 0.0f, -1.0e5f}, kCenter, kDisc, range);
    SOL_CHECK(nearlyEqual(ahead.x, kCenter.x));
    SOL_CHECK(ahead.y < kCenter.y);

    const Vec2 behind = radarPoint({0.0f, 0.0f, 1.0e5f}, kCenter, kDisc, range);
    SOL_CHECK(nearlyEqual(behind.x, kCenter.x));
    SOL_CHECK(behind.y > kCenter.y);

    const Vec2 starboard = radarPoint({1.0e5f, 0.0f, 0.0f}, kCenter, kDisc, range);
    SOL_CHECK(starboard.x > kCenter.x);
    SOL_CHECK(nearlyEqual(starboard.y, kCenter.y));

    const Vec2 port = radarPoint({-1.0e5f, 0.0f, 0.0f}, kCenter, kDisc, range);
    SOL_CHECK(port.x < kCenter.x);
    SOL_CHECK(nearlyEqual(port.y, kCenter.y));
}

SOL_TEST(radar_altitude_does_not_move_the_dot_around_the_disc)
{
    // The stalk carries height; the dot's position on the disc is the ground
    // bearing only. Otherwise a contact directly overhead would appear to be
    // somewhere it is not.
    const float range = radarRange(1.0e5);
    const Vec2 flat = radarPoint({3.0e4f, 0.0f, -4.0e4f}, kCenter, kDisc, range);
    const Vec2 high = radarPoint({3.0e4f, 9.0e5f, -4.0e4f}, kCenter, kDisc, range);
    SOL_CHECK(nearlyEqual(flat.x, high.x));
    SOL_CHECK(nearlyEqual(flat.y, high.y));
}

SOL_TEST(radar_clamps_distant_contacts_to_the_rim)
{
    const float range = radarRange(1.0e5);
    const Vec3 far{0.0f, 0.0f, -1.0e11f};

    SOL_CHECK(radarBeyondRange(far, range));
    const Vec2 point = radarPoint(far, kCenter, kDisc, range);
    // Exactly on the rim, never outside it - the caller draws it hollow so
    // "beyond the radar" does not read as "at the edge of the radar".
    const float dx = point.x - kCenter.x;
    const float dy = point.y - kCenter.y;
    SOL_CHECK(nearlyEqual(std::sqrt(dx * dx + dy * dy), kDisc));

    const Vec3 near{0.0f, 0.0f, -1.0e3f};
    SOL_CHECK(!radarBeyondRange(near, range));
}

SOL_TEST(radar_puts_the_ship_itself_at_the_center)
{
    const float range = radarRange(1.0e5);
    const Vec2 here = radarPoint({0.0f, 0.0f, 0.0f}, kCenter, kDisc, range);
    SOL_CHECK(nearlyEqual(here.x, kCenter.x));
    SOL_CHECK(nearlyEqual(here.y, kCenter.y));
}

SOL_TEST(radar_stalk_signs_above_and_below_and_saturates)
{
    constexpr float kLimit = 20.0f;

    // Screen y grows downward, so something above the ship draws upward.
    SOL_CHECK(radarStalk({0.0f, 5.0e4f, 0.0f}, kLimit) < 0.0f);
    SOL_CHECK(radarStalk({0.0f, -5.0e4f, 0.0f}, kLimit) > 0.0f);
    SOL_CHECK(nearlyEqual(radarStalk({0.0f, 0.0f, 0.0f}, kLimit), 0.0f));

    // Further above is a longer stalk, and it never runs past the limit.
    const float low = radarStalk({0.0f, 5.0e3f, 0.0f}, kLimit);
    const float high = radarStalk({0.0f, 5.0e6f, 0.0f}, kLimit);
    SOL_CHECK(high < low); // both negative; further above is more negative
    SOL_CHECK(radarStalk({0.0f, 1.0e12f, 0.0f}, kLimit) >= -kLimit);
    SOL_CHECK(radarStalk({0.0f, -1.0e12f, 0.0f}, kLimit) <= kLimit);
}

SOL_TEST(radar_degenerate_inputs_draw_nothing_rather_than_dividing_by_zero)
{
    // A zero-radius disc or a zero range must collapse to the center, the
    // same defensive shape fitGalaxyMap uses for an empty galaxy.
    SOL_CHECK(nearlyEqual(radarPoint({1.0f, 0.0f, 0.0f}, kCenter, 0.0f, 1.0f).x, kCenter.x));
    SOL_CHECK(nearlyEqual(radarPoint({1.0f, 0.0f, 0.0f}, kCenter, kDisc, 0.0f).y, kCenter.y));
    // And an empty contact set still yields a usable range.
    SOL_CHECK(radarRange(0.0) > 0.0f);
}
