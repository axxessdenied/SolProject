#include <sol/ui/pick.hpp>
#include <sol/ui/radar_projection.hpp>

#include <sol/test/test.hpp>

#include <vector>

using sol::core::Vec2;
using sol::core::Vec3;
using sol::ui::focalLength;
using sol::ui::insideDisc;
using sol::ui::kNoPick;
using sol::ui::PickCandidate;
using sol::ui::pickNearest;
using sol::ui::pickNearestPoint;
using sol::ui::screenPoint;

namespace {

bool nearlyEqual(float a, float b, float tolerance = 1.0e-3f)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < tolerance;
}

constexpr Vec2 kCenter = {640.0f, 360.0f};
const float kFocal = focalLength(720.0f, 1.0f); // 45 degrees half-angle: focal = height/2

// A candidate `pixels` to the right of the boresight at `range` meters. Built
// from the projection's own inverse relation so the tests state where a thing
// is on screen rather than restating the arithmetic that puts it there.
PickCandidate at(float pixelsRight, float pixelsDown, double range, std::uint32_t selection,
                 float screenRadius = 0.0f)
{
    PickCandidate candidate;
    candidate.directionCamera = {pixelsRight / kFocal, -pixelsDown / kFocal, -1.0f};
    candidate.screenRadius = screenRadius;
    candidate.rangeMeters = range;
    candidate.selection = selection;
    return candidate;
}

} // namespace

SOL_TEST(screen_point_puts_the_boresight_at_the_center)
{
    const sol::ui::ScreenPoint dead = screenPoint({0.0f, 0.0f, -1.0f}, kCenter, kFocal);
    SOL_CHECK(dead.inFront);
    SOL_CHECK(nearlyEqual(dead.position.x, kCenter.x));
    SOL_CHECK(nearlyEqual(dead.position.y, kCenter.y));

    // Camera space: +x right, +y up, screen y grows downward.
    const sol::ui::ScreenPoint up = screenPoint({0.0f, 1.0f, -1.0f}, kCenter, kFocal);
    SOL_CHECK(up.position.y < kCenter.y);
    const sol::ui::ScreenPoint right = screenPoint({1.0f, 0.0f, -1.0f}, kCenter, kFocal);
    SOL_CHECK(right.position.x > kCenter.x);
}

SOL_TEST(screen_point_rejects_anything_behind_the_camera)
{
    // The projection would mirror it to a perfectly plausible place on screen,
    // which is exactly the bug this flag exists to prevent.
    SOL_CHECK(!screenPoint({0.0f, 0.0f, 1.0f}, kCenter, kFocal).inFront);
    SOL_CHECK(!screenPoint({0.0f, 0.0f, 0.0f}, kCenter, kFocal).inFront);
}

SOL_TEST(pick_takes_the_candidate_under_the_cursor)
{
    const std::vector<PickCandidate> candidates = {at(0.0f, 0.0f, 1000.0, 7)};
    // Dead on, and a few pixels off, both hit.
    SOL_CHECK(pickNearest(candidates, kCenter, kCenter, kFocal) == 0);
    SOL_CHECK(pickNearest(candidates, {kCenter.x + 10.0f, kCenter.y}, kCenter, kFocal) == 0);
}

SOL_TEST(pick_misses_past_the_grab_radius)
{
    const std::vector<PickCandidate> candidates = {at(0.0f, 0.0f, 1000.0, 7)};
    const float outside = sol::ui::kPickGrabPixels + 2.0f;
    SOL_CHECK(pickNearest(candidates, {kCenter.x + outside, kCenter.y}, kCenter, kFocal) == kNoPick);
    // A miss is a miss however few candidates there are: the click must not
    // fall back to "nearest anything", or an empty-space click would steal the
    // player's target.
    SOL_CHECK(pickNearest(candidates, {0.0f, 0.0f}, kCenter, kFocal) == kNoPick);
}

SOL_TEST(pick_takes_the_nearest_to_the_cursor_then_the_nearer_in_world)
{
    // A fighter 10 px right at 2 km, a freighter 4 px right at 90 km: the
    // cursor is on the freighter, and screen distance decides.
    const std::vector<PickCandidate> candidates = {at(10.0f, 0.0f, 2000.0, 1),
                                                   at(4.0f, 0.0f, 90000.0, 2)};
    SOL_CHECK(pickNearest(candidates, {kCenter.x + 4.0f, kCenter.y}, kCenter, kFocal) == 1);

    // Exactly co-located on screen: the nearer one in world wins, so a fighter
    // silhouetted against a planet is picked over the planet.
    const std::vector<PickCandidate> stacked = {at(6.0f, 0.0f, 1.0e9, 3),
                                                at(6.0f, 0.0f, 3000.0, 4)};
    const std::size_t hit = pickNearest(stacked, {kCenter.x + 6.0f, kCenter.y}, kCenter, kFocal);
    SOL_CHECK(hit == 1);
    SOL_CHECK(stacked[hit].selection == 4);
}

SOL_TEST(pick_grows_the_grab_box_to_an_object_screen_size)
{
    // A station 200 px across is clicked in the middle of its hull, not within
    // 28 px of the point its centre projects to.
    const float far = 120.0f;
    const std::vector<PickCandidate> candidates = {at(0.0f, 0.0f, 5000.0, 9, 200.0f)};
    SOL_CHECK(pickNearest(candidates, {kCenter.x + far, kCenter.y}, kCenter, kFocal) == 0);
    // ...but not beyond its own edge.
    SOL_CHECK(pickNearest(candidates, {kCenter.x + 260.0f, kCenter.y}, kCenter, kFocal) == kNoPick);
}

SOL_TEST(pick_never_takes_a_candidate_behind_the_camera)
{
    PickCandidate behind;
    behind.directionCamera = {0.0f, 0.0f, 1.0f};
    behind.rangeMeters = 500.0;
    behind.selection = 5;
    const std::vector<PickCandidate> candidates = {behind};
    SOL_CHECK(pickNearest(candidates, kCenter, kCenter, kFocal) == kNoPick);
}

SOL_TEST(radar_dot_is_where_the_blip_is_drawn)
{
    const float range = sol::ui::radarRange(sol::ui::kRadarRangeMeters);
    const Vec2 center = {400.0f, 500.0f};
    const Vec3 offset = {3000.0f, 900.0f, -4000.0f};

    // The composition the draw loop performs: plotted point, carried to the
    // end of its altitude stalk. The hit test is only honest if it agrees.
    const Vec2 point =
        sol::ui::radarPoint(offset, center, sol::ui::kRadarPlotRadius, range);
    const float stalk = sol::ui::radarStalk(offset, sol::ui::kRadarStalkLimit);
    const Vec2 dot =
        sol::ui::radarDot(offset, center, sol::ui::kRadarPlotRadius, range,
                          sol::ui::kRadarStalkLimit);
    SOL_CHECK(nearlyEqual(dot.x, point.x));
    SOL_CHECK(nearlyEqual(dot.y, point.y + stalk));
    SOL_CHECK(!nearlyEqual(stalk, 0.0f)); // the case worth testing is a real stalk
}

SOL_TEST(radar_pick_hits_the_blip_that_was_drawn)
{
    const float range = sol::ui::radarRange(sol::ui::kRadarRangeMeters);
    const Vec2 center = sol::ui::radarCenter({1280.0f, 720.0f}, 24.0f);

    // A high near contact, a low far one, and one past the rim that clamps -
    // the three cases the disc draws differently and the pick must not.
    const std::vector<Vec3> offsets = {{200.0f, 400.0f, -300.0f},
                                       {-9000.0f, -2000.0f, 5000.0f},
                                       {0.0f, 0.0f, -1.0e12f}};
    std::vector<Vec2> dots;
    for (const Vec3& offset : offsets) {
        dots.push_back(sol::ui::radarDot(offset, center, sol::ui::kRadarPlotRadius, range,
                                         sol::ui::kRadarStalkLimit));
    }

    for (std::size_t i = 0; i < dots.size(); ++i) {
        SOL_CHECK(pickNearestPoint(dots, dots[i]) == i);
        // Every dot lands inside the disc, stalk included - that inset is what
        // keeps a clickable blip clickable.
        SOL_CHECK(insideDisc(dots[i], center, sol::ui::kRadarRadius));
    }

    // A click in empty disc space selects nothing rather than the nearest blip.
    SOL_CHECK(pickNearestPoint(dots, center) == kNoPick);
}

SOL_TEST(inside_disc_routes_the_click)
{
    const Vec2 center = sol::ui::radarCenter({1280.0f, 720.0f}, 24.0f);
    SOL_CHECK(insideDisc(center, center, sol::ui::kRadarRadius));
    SOL_CHECK(insideDisc({center.x + sol::ui::kRadarRadius - 1.0f, center.y}, center,
                         sol::ui::kRadarRadius));
    SOL_CHECK(!insideDisc({center.x + sol::ui::kRadarRadius + 1.0f, center.y}, center,
                          sol::ui::kRadarRadius));
    // The disc sits above the bottom margin, and the flight view above it.
    SOL_CHECK(!insideDisc({640.0f, 360.0f}, center, sol::ui::kRadarRadius));
}

// Phase 15: the streaming pick the map feeds from its own draw loop.
SOL_TEST(nearest_pick_takes_the_nearest_candidate_it_is_shown)
{
    sol::ui::NearestPick pick({100.0f, 100.0f}, 14.0f);
    SOL_CHECK(pick.active());
    pick.consider(0, {130.0f, 100.0f}); // 30 px away: outside the grab
    SOL_CHECK(pick.result() == kNoPick);
    pick.consider(1, {108.0f, 100.0f}); // 8 px
    SOL_CHECK(pick.result() == 1);
    pick.consider(2, {103.0f, 100.0f}); // 3 px: closer, so it wins
    SOL_CHECK(pick.result() == 2);
    pick.consider(3, {110.0f, 100.0f}); // 10 px: further, so it does not
    SOL_CHECK(pick.result() == 2);
}

// The index is the caller's, not a count of how many were considered - the map
// skips fogged markers, so the numbering has gaps by construction.
SOL_TEST(nearest_pick_reports_the_index_it_was_given)
{
    sol::ui::NearestPick pick({0.0f, 0.0f}, 14.0f);
    pick.consider(7, {1.0f, 0.0f});
    SOL_CHECK(pick.result() == 7);
}

// Ties go to the first considered, the same rule pickNearestPoint follows.
SOL_TEST(nearest_pick_breaks_a_tie_toward_the_first)
{
    sol::ui::NearestPick pick({0.0f, 0.0f}, 14.0f);
    pick.consider(4, {5.0f, 0.0f});
    pick.consider(5, {0.0f, 5.0f});
    SOL_CHECK(pick.result() == 4);
}

// A default-constructed pick is what a frame with no click uses, and it must
// answer kNoPick however many candidates the draw walks past it.
SOL_TEST(an_inactive_nearest_pick_never_picks)
{
    sol::ui::NearestPick pick;
    SOL_CHECK(!pick.active());
    pick.consider(0, {0.0f, 0.0f});
    pick.consider(1, {1.0f, 1.0f});
    SOL_CHECK(pick.result() == kNoPick);
}
