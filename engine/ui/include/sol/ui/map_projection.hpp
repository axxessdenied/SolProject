#pragma once

#include "sol/ui/draw_list.hpp"
#include "sol/ui/screens.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace sol::ui {

// Map projection math for the Phase 8e map screens, kept out of the screen so
// it can be asserted on headlessly: what fits in the view, what is allowed to
// be drawn at all, and how a playfield spanning six orders of magnitude lands
// on one readable disc.

// Galaxy view: light-years to screen pixels, uniform in both axes so the lane
// graph keeps its shape.
struct MapProjection
{
    core::Vec2 origin; // map-space point that lands on `center`
    float scale = 1.0f;
    core::Vec2 center;

    [[nodiscard]] core::Vec2 operator()(core::Vec2 mapPosition) const
    {
        return {center.x + (mapPosition.x - origin.x) * scale,
                center.y + (mapPosition.y - origin.y) * scale};
    }
};

// A system is on the map only once the player knows it exists. This is the
// fog rule in one place: everything that draws goes through it.
[[nodiscard]] inline bool systemVisible(const MapSystemRow& system)
{
    return system.knowledge != MapKnowledge::Unknown;
}

// A lane needs both ends known - otherwise it would point at a system the
// player has not heard of.
[[nodiscard]] inline bool laneVisible(std::span<const MapSystemRow> systems,
                                      const MapLaneRow& lane)
{
    if (lane.from < 0 || lane.to < 0 || lane.from >= static_cast<int>(systems.size())
        || lane.to >= static_cast<int>(systems.size())) {
        return false;
    }
    return systemVisible(systems[static_cast<std::size_t>(lane.from)])
           && systemVisible(systems[static_cast<std::size_t>(lane.to)]);
}

// Fits every visible system inside `view`, inset by `margin`. With nothing
// visible (or a degenerate view) the projection is the identity about the
// view's center, which draws nothing rather than dividing by zero.
[[nodiscard]] inline MapProjection fitGalaxyMap(std::span<const MapSystemRow> systems,
                                                const Rect& view, float margin = 34.0f)
{
    MapProjection projection;
    projection.center = {(view.min.x + view.max.x) * 0.5f, (view.min.y + view.max.y) * 0.5f};
    core::Vec2 minimum{0.0f, 0.0f};
    core::Vec2 maximum{0.0f, 0.0f};
    bool any = false;
    for (const MapSystemRow& system : systems) {
        if (!systemVisible(system)) {
            continue;
        }
        if (!any) {
            minimum = system.position;
            maximum = system.position;
            any = true;
            continue;
        }
        minimum.x = std::min(minimum.x, system.position.x);
        minimum.y = std::min(minimum.y, system.position.y);
        maximum.x = std::max(maximum.x, system.position.x);
        maximum.y = std::max(maximum.y, system.position.y);
    }
    if (!any) {
        return projection;
    }
    projection.origin = {(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f};
    const float spanX = std::max(maximum.x - minimum.x, 1.0f);
    const float spanY = std::max(maximum.y - minimum.y, 1.0f);
    const float usableX = std::max(view.width() - margin * 2.0f, 1.0f);
    const float usableY = std::max(view.height() - margin * 2.0f, 1.0f);
    projection.scale = std::min(usableX / spanX, usableY / spanY);
    return projection;
}

// System view: a playfield runs from a station 1e8 m out to a star 1e11 m
// out, so radius is drawn on a log curve. Bearing is kept exact - the shape
// of the system is real, only the distances are compressed.
[[nodiscard]] inline float systemMapRadius(double meters)
{
    return static_cast<float>(std::log10(1.0 + std::abs(meters) / 1.0e5));
}

// Largest compressed radius over a marker set, the denominator that keeps the
// outermost marker just inside the disc. Never zero.
[[nodiscard]] inline float systemMapExtent(std::span<const MapMarkerRow> markers)
{
    float extent = 0.0f;
    for (const MapMarkerRow& marker : markers) {
        const double x = marker.position.x;
        const double y = marker.position.y;
        extent = std::max(extent, systemMapRadius(std::sqrt(x * x + y * y)));
    }
    return extent > 0.0f ? extent : 1.0f;
}

// Where a marker lands on the disc of radius `discRadius` around `center`.
[[nodiscard]] inline core::Vec2 systemMapPoint(core::Vec2 offsetMeters, core::Vec2 center,
                                               float discRadius, float extent)
{
    const double x = offsetMeters.x;
    const double y = offsetMeters.y;
    const double distance = std::sqrt(x * x + y * y);
    if (distance <= 0.0 || extent <= 0.0f) {
        return center;
    }
    const float scaled = discRadius * (systemMapRadius(distance) / extent);
    const float angle = static_cast<float>(std::atan2(y, x));
    return {center.x + std::cos(angle) * scaled, center.y + std::sin(angle) * scaled};
}

} // namespace sol::ui
