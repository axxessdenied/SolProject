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

// --- Zoom and pan (Phase 8h) -------------------------------------------------

// A screen-space magnifier applied on top of whichever projection a view
// already used. Deliberately post-projection: it magnifies exactly what the
// player is looking at, including the system view's crowded playfield bubble,
// and leaves both tiers' extent maths untouched. Uniform, so shapes are
// preserved and a scaled radius is just radius * zoom.
struct MapView
{
    core::Vec2 origin; // the view's centre, what zoom happens about
    core::Vec2 pan;    // screen pixels
    float zoom = 1.0f;

    [[nodiscard]] core::Vec2 operator()(core::Vec2 point) const
    {
        return {origin.x + (point.x - origin.x) * zoom + pan.x,
                origin.y + (point.y - origin.y) * zoom + pan.y};
    }
    [[nodiscard]] float scaled(float length) const { return length * zoom; }
};

// The pan that keeps whatever sits under `anchor` under `anchor` across a
// zoom change. Without it the map slides away from the very thing the player
// put the cursor on to look at.
[[nodiscard]] inline core::Vec2 panHoldingAnchor(core::Vec2 pan, core::Vec2 origin,
                                                 core::Vec2 anchor, float fromZoom, float toZoom)
{
    if (!(fromZoom > 0.0f)) {
        return pan;
    }
    const float ratio = toZoom / fromZoom;
    return {anchor.x - (anchor.x - origin.x - pan.x) * ratio - origin.x,
            anchor.y - (anchor.y - origin.y - pan.y) * ratio - origin.y};
}

// How far the content may be dragged: half the magnified overshoot in each
// axis, which reaches every part of it without letting it be lost off-panel.
// Zero at 1x, where there is nothing to pan to.
[[nodiscard]] inline core::Vec2 panLimit(const Rect& view, float zoom)
{
    const float excess = zoom > 1.0f ? zoom - 1.0f : 0.0f;
    return {view.width() * 0.5f * excess, view.height() * 0.5f * excess};
}

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

// And the same rule one tier down (Phase 8q): what a system map may show of a
// system the player is not standing in. The knowledge rung gates the *kinds*
// of thing that can appear; whether an individual signal appears is a separate
// question answered by SurveySim's per-signal discovery bit, which the fill
// applies on top of this - so an unswept Surveyed-adjacent system still shows
// no sites, because it has none discovered.
//
// Charted is the ladder's own words: "named by a gate you have stood at;
// contents blank". You know the system is there and what it is called, so you
// get the star - which is the system - and nothing around it. Visited is the
// ladder's words too: "you have been here: owner, stations, and bodies are
// known". Surveyed adds no new *kind*, only more signal bits already set,
// which is why the table below has two interesting rows rather than four.
//
// Bookmarks ignore the rung entirely, for the reason the galaxy map's
// bookmarkCount already ignores it: a bookmark is the player's own knowledge
// and they were standing there when they made it.
[[nodiscard]] inline bool markerVisible(MapKnowledge knowledge, MapMarkerRow::Kind kind)
{
    if (kind == MapMarkerRow::Kind::Bookmark) {
        return knowledge != MapKnowledge::Unknown;
    }
    if (knowledge == MapKnowledge::Unknown) {
        return false;
    }
    if (kind == MapMarkerRow::Kind::Star) {
        return true;
    }
    // Everything else is contents, and contents need a visit.
    return knowledge >= MapKnowledge::Visited;
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

// Orbital tier: distance from the star. Orbits run from about 4e10 m out to
// 4e11, so this uses a far coarser decade than the playfield does — under the
// playfield's curve every orbit lands in the outermost few percent of the
// disc and the planets pile up on the rim.
[[nodiscard]] inline float orbitMapRadius(double meters)
{
    return static_cast<float>(std::log10(1.0 + std::abs(meters) / 1.0e9));
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

// The same, restricted to one tier: the orbital extent is measured on the
// orbit curve over the bodies, the playfield extent on the playfield curve
// over everything else.
[[nodiscard]] inline float orbitExtent(std::span<const MapMarkerRow> markers,
                                       core::Vec2 hubPosition)
{
    // The hub itself has to fit, even in a system whose only body is the one
    // the playfield hangs off.
    float extent = orbitMapRadius(
        std::sqrt(static_cast<double>(hubPosition.x) * hubPosition.x
                  + static_cast<double>(hubPosition.y) * hubPosition.y));
    for (const MapMarkerRow& marker : markers) {
        if (marker.inPlayfield) {
            continue;
        }
        const double x = marker.position.x;
        const double y = marker.position.y;
        extent = std::max(extent, orbitMapRadius(std::sqrt(x * x + y * y)));
    }
    return extent > 0.0f ? extent : 1.0f;
}

// Playfield tier: how far out the furthest thing in the bubble is, in meters.
// Unlike the rest of the system this is *linear*, because the playfield spans
// well under a decade (a station 1e8 m out, a gate 6e8) — a log curve over
// that range squeezes everything into a thin ring at the bubble's edge, which
// undoes the whole point of expanding it. Linear also means relative
// positions inside the bubble are exactly true, which is what you want of the
// part of the map you actually fly.
[[nodiscard]] inline float playfieldSpan(std::span<const MapMarkerRow> markers)
{
    float span = 0.0f;
    for (const MapMarkerRow& marker : markers) {
        if (!marker.inPlayfield) {
            continue;
        }
        const double x = marker.position.x;
        const double y = marker.position.y;
        span = std::max(span, static_cast<float>(std::sqrt(x * x + y * y)));
    }
    return span > 0.0f ? span : 1.0f;
}

[[nodiscard]] inline core::Vec2 playfieldPoint(core::Vec2 offsetMeters, core::Vec2 center,
                                               float discRadius, float spanMeters)
{
    if (!(spanMeters > 0.0f)) {
        return center;
    }
    const float scale = discRadius / spanMeters;
    return {center.x + offsetMeters.x * scale, center.y + offsetMeters.y * scale};
}

// Where an orbital body lands on the disc: the star is the middle, and
// bearing is exact so the shape of the system is real.
[[nodiscard]] inline core::Vec2 orbitMapPoint(core::Vec2 offsetMeters, core::Vec2 center,
                                              float discRadius, float extent)
{
    const double x = offsetMeters.x;
    const double y = offsetMeters.y;
    const double distance = std::sqrt(x * x + y * y);
    if (distance <= 0.0 || extent <= 0.0f) {
        return center;
    }
    const float scaled = discRadius * (orbitMapRadius(distance) / extent);
    const float angle = static_cast<float>(std::atan2(y, x));
    return {center.x + std::cos(angle) * scaled, center.y + std::sin(angle) * scaled};
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
