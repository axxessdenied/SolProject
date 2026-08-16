#include <sol/ui/map_projection.hpp>

#include <sol/test/test.hpp>

#include <vector>

using sol::core::Vec2;
using sol::ui::fitGalaxyMap;
using sol::ui::laneVisible;
using sol::ui::MapKnowledge;
using sol::ui::MapLaneRow;
using sol::ui::MapMarkerRow;
using sol::ui::MapProjection;
using sol::ui::MapSystemRow;
using sol::ui::Rect;
using sol::ui::systemMapExtent;
using sol::ui::systemMapPoint;
using sol::ui::systemMapRadius;
using sol::ui::systemVisible;

namespace {

bool nearlyEqual(float a, float b, float tolerance = 1.0e-3f)
{
    const float difference = a - b;
    return (difference < 0.0f ? -difference : difference) < tolerance;
}

// Four systems in a square, of which one is still unknown.
std::vector<MapSystemRow> squareGalaxy()
{
    std::vector<MapSystemRow> systems(4);
    systems[0].position = {-10.0f, -10.0f};
    systems[0].knowledge = MapKnowledge::Visited;
    systems[1].position = {10.0f, -10.0f};
    systems[1].knowledge = MapKnowledge::Charted;
    systems[2].position = {10.0f, 10.0f};
    systems[2].knowledge = MapKnowledge::Surveyed;
    systems[3].position = {400.0f, 400.0f}; // far away, and unknown
    systems[3].knowledge = MapKnowledge::Unknown;
    return systems;
}

} // namespace

SOL_TEST(map_projection_fits_known_systems_inside_the_view)
{
    const std::vector<MapSystemRow> systems = squareGalaxy();
    const Rect view = {{100.0f, 100.0f}, {500.0f, 400.0f}};
    const MapProjection project = fitGalaxyMap(systems, view);

    // The unknown outlier must not stretch the fit: the three known systems
    // span 20 ly, and the view's short axis (300 px, less 2x34 margin) is what
    // the scale is chosen against.
    SOL_CHECK(nearlyEqual(project.scale, (300.0f - 68.0f) / 20.0f));
    SOL_CHECK(nearlyEqual(project.origin.x, 0.0f));
    SOL_CHECK(nearlyEqual(project.origin.y, 0.0f));

    for (const MapSystemRow& system : systems) {
        if (!systemVisible(system)) {
            continue;
        }
        const Vec2 point = project(system.position);
        SOL_CHECK(point.x >= view.min.x && point.x <= view.max.x);
        SOL_CHECK(point.y >= view.min.y && point.y <= view.max.y);
    }
    // The center of the known set lands on the center of the view.
    const Vec2 middle = project({0.0f, 0.0f});
    SOL_CHECK(nearlyEqual(middle.x, 300.0f));
    SOL_CHECK(nearlyEqual(middle.y, 250.0f));
}

SOL_TEST(map_projection_survives_an_empty_or_degenerate_map)
{
    std::vector<MapSystemRow> systems(2); // both Unknown by default
    const Rect view = {{0.0f, 0.0f}, {200.0f, 200.0f}};
    const MapProjection empty = fitGalaxyMap(systems, view);
    SOL_CHECK(nearlyEqual(empty.scale, 1.0f));
    SOL_CHECK(nearlyEqual(empty({0.0f, 0.0f}).x, 100.0f));

    // One known system: no span to fit, and the scale must stay finite.
    systems[0].knowledge = MapKnowledge::Visited;
    systems[0].position = {5.0f, -3.0f};
    const MapProjection single = fitGalaxyMap(systems, view);
    SOL_CHECK(single.scale > 0.0f);
    SOL_CHECK(nearlyEqual(single(systems[0].position).x, 100.0f));
    SOL_CHECK(nearlyEqual(single(systems[0].position).y, 100.0f));

    // A view smaller than its own margins still yields a drawable projection.
    const MapProjection tiny = fitGalaxyMap(squareGalaxy(), {{0.0f, 0.0f}, {10.0f, 10.0f}});
    SOL_CHECK(tiny.scale > 0.0f);
}

SOL_TEST(map_projection_culls_lanes_into_the_unknown)
{
    const std::vector<MapSystemRow> systems = squareGalaxy();
    SOL_CHECK(laneVisible(systems, {.from = 0, .to = 1, .onRoute = false}));
    SOL_CHECK(laneVisible(systems, {.from = 1, .to = 2, .onRoute = false}));
    // System 3 is unknown, so nothing may point at it.
    SOL_CHECK(!laneVisible(systems, {.from = 2, .to = 3, .onRoute = false}));
    SOL_CHECK(!laneVisible(systems, {.from = 3, .to = 0, .onRoute = false}));
    // Out-of-range indices are refused rather than indexed.
    SOL_CHECK(!laneVisible(systems, {.from = 0, .to = 9, .onRoute = false}));
    SOL_CHECK(!laneVisible(systems, {.from = -1, .to = 0, .onRoute = false}));
}

SOL_TEST(map_system_view_compresses_range_but_keeps_order_and_bearing)
{
    // A playfield really does span these distances: a wreck 2,000 km out and a
    // star 40 million km out have to share one disc.
    SOL_CHECK(systemMapRadius(0.0) == 0.0f);
    SOL_CHECK(systemMapRadius(2.0e6) < systemMapRadius(1.0e8));
    SOL_CHECK(systemMapRadius(1.0e8) < systemMapRadius(4.0e10));
    // Ten times further is one more decade, not ten times the pixels.
    SOL_CHECK(nearlyEqual(systemMapRadius(1.0e10) - systemMapRadius(1.0e9), 1.0f, 0.01f));

    std::vector<MapMarkerRow> markers(3);
    markers[0].position = {0.0f, 0.0f};        // the hub itself
    markers[1].position = {1.0e8f, 0.0f};      // a station, due +x
    markers[2].position = {0.0f, -4.0e10f};    // the star, off the -y axis
    const float extent = systemMapExtent(markers);
    SOL_CHECK(extent > 0.0f);

    const Vec2 center = {200.0f, 200.0f};
    constexpr float kDisc = 100.0f;
    const Vec2 hub = systemMapPoint(markers[0].position, center, kDisc, extent);
    const Vec2 station = systemMapPoint(markers[1].position, center, kDisc, extent);
    const Vec2 star = systemMapPoint(markers[2].position, center, kDisc, extent);

    SOL_CHECK(nearlyEqual(hub.x, center.x) && nearlyEqual(hub.y, center.y));
    // Bearing is exact: +x stays +x, -y stays -y.
    SOL_CHECK(station.x > center.x && nearlyEqual(station.y, center.y));
    SOL_CHECK(star.y < center.y && nearlyEqual(star.x, center.x));
    // Everything lands inside the disc, and further really is further out.
    const float stationRadius = station.x - center.x;
    const float starRadius = center.y - star.y;
    SOL_CHECK(stationRadius < starRadius);
    SOL_CHECK(starRadius <= kDisc + 0.01f);
    SOL_CHECK(stationRadius > kDisc * 0.3f); // and not squashed into the middle

    // A degenerate extent collapses to the center rather than dividing by zero.
    SOL_CHECK(nearlyEqual(systemMapPoint({1.0e8f, 0.0f}, center, kDisc, 0.0f).x, center.x));
}
