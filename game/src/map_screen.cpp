#include "map_screen.hpp"

#include "sol/ui/layout.hpp"
#include "sol/ui/map_projection.hpp"
#include "sol/ui/pick.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace game {

namespace assets = sol::assets;

using sol::core::Vec2;
using sol::ui::Color;
using sol::ui::Column;
using sol::ui::inset;
using sol::ui::MapAction;
using sol::ui::MapKnowledge;
using sol::ui::MapLaneRow;
using sol::ui::MapMarkerRow;
using sol::ui::MapPanel;
using sol::ui::MapSystemRow;
using sol::ui::Rect;
using sol::ui::rgba;
using sol::ui::Row;
using sol::ui::TextAlign;
using sol::ui::UiContext;

namespace {

constexpr float kRowHeight = 26.0f;
constexpr float kHeaderHeight = 44.0f;
constexpr float kTabHeight = 34.0f;
constexpr float kFooterHeight = 40.0f;
constexpr float kListWidth = 290.0f;
constexpr float kButtonWidth = 118.0f;
constexpr float kNodeRadius = 5.0f;

constexpr Color kLane = rgba(0x35506AFFu);
constexpr Color kLaneRoute = rgba(0x58C8F0FFu);
constexpr Color kCharted = rgba(0x6B7C8CFFu);
constexpr Color kCurrentRing = rgba(0xF2CC59FFu);
// Phase 8i. Nothing like the bookmark's gold: "somewhere I chose" and
// "somewhere I was sent" must never read alike. Shared by both map tiers.
constexpr Color kObjective = rgba(0xFF66C4FFu);

constexpr const char* const kTabLabels[MapScreenState::TabCount] = {"Galaxy", "System"};

[[nodiscard]] Color ownerColor(const MapSystemRow& system)
{
    if (!system.hasOwner) {
        return kCharted;
    }
    return {system.ownerColor.x, system.ownerColor.y, system.ownerColor.z, 1.0f};
}

// Trade overlay (Phase 8g): cheap is green, dear is red — you buy in the
// green and sell in the red, which is the whole map in one sentence. A
// system with no reading at all stays grey rather than pretending to be
// average, and a stale reading is washed out because the market has moved
// since the player looked.
[[nodiscard]] Color tradeColor(const MapSystemRow& system)
{
    if (!system.hasTrade) {
        return rgba(0x44505CFFu);
    }
    const float level = system.tradeLevel < 0.0f ? 0.0f : system.tradeLevel > 1.0f ? 1.0f : system.tradeLevel;
    const Color cheap = rgba(0x69C48CFFu);
    const Color dear = rgba(0xE0704CFFu);
    Color color{cheap.r + (dear.r - cheap.r) * level,
                cheap.g + (dear.g - cheap.g) * level,
                cheap.b + (dear.b - cheap.b) * level,
                1.0f};
    return system.tradeStale ? color.withAlpha(0.45f) : color;
}

[[nodiscard]] Color systemColor(const MapPanel& panel, const MapSystemRow& system)
{
    return panel.tradeCommodity >= 0 ? tradeColor(system) : ownerColor(system);
}

// Marker glyph colors: what a thing is, at a glance, without a legend.
[[nodiscard]] Color markerColor(const UiContext& ui, const MapMarkerRow& marker)
{
    switch (marker.kind) {
    case MapMarkerRow::Kind::Star:
        return rgba(0xFFD97AFFu);
    case MapMarkerRow::Kind::Planet:
        return rgba(0x7FA8D0FFu);
    case MapMarkerRow::Kind::Station:
        return ui.theme().accent;
    case MapMarkerRow::Kind::Gate:
        return rgba(0xB48CE0FFu);
    case MapMarkerRow::Kind::Signal:
        return marker.scanned ? rgba(0x69C48CFFu) : rgba(0xE68C4DFFu);
    case MapMarkerRow::Kind::Field:
        return rgba(0x9C8F7AFFu); // rock
    case MapMarkerRow::Kind::Wreck:
        return rgba(0xC4696CFFu); // something died here
    case MapMarkerRow::Kind::Bookmark:
        return rgba(0xFFC850FFu); // the player's own mark, not the galaxy's
    case MapMarkerRow::Kind::Objective:
        return kObjective;
    }
    return ui.theme().textDim;
}

void clipped(UiContext& ui,
             const Rect& cell,
             std::string_view text,
             const Color& color,
             const char* style = nullptr,
             TextAlign align = TextAlign::Left)
{
    if (cell.empty() || text.empty()) {
        return;
    }
    ui.drawList().pushClip(cell);
    ui.label({{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}}, text, color, style, align);
    ui.drawList().popClip();
}

// --- Zoom and pan (Phase 8h) -------------------------------------------------

constexpr float kMinZoom = 1.0f;
constexpr float kMaxZoom = 8.0f;
constexpr float kZoomPerNotch = 1.2f;
// Phase 15's click-vs-pan slop now lives in `sol/ui/context.hpp` as
// `sol::ui::kClickSlopPixels`, because Phase 28 gave the right button the same
// question to answer. ⚑ The VALUE is unchanged at 4.0f and this file's
// behaviour is unchanged with it; the reasoning moved with the constant.
using sol::ui::kClickSlopPixels;
// The grab radius for a map marker. Wider than the radar's 11 px because a map
// dot is 3.5 px across and the two tiers can place a station and its gates
// within a few pixels of each other, and narrower than the flight view's 28
// because the map is where a player goes to be precise.
constexpr float kMapGrabPixels = 14.0f;

using sol::ui::MapView;

// Wheel zooms about the cursor, drag pans, and pan is clamped so the content
// cannot be thrown out of the panel entirely. Returns the transform to draw
// with. Only responds while the cursor is inside `view`, so the wheel still
// belongs to the system list when it is over the list.
[[nodiscard]] MapView
updateMapView(UiContext& ui, const Rect& view, MapScreenState& state, bool menuOpen, bool& clicked)
{
    const std::size_t tab = static_cast<std::size_t>(state.tab);
    const Vec2 origin = {(view.min.x + view.max.x) * 0.5f, (view.min.y + view.max.y) * 0.5f};
    const sol::ui::InputState& input = ui.input();
    // ⚑ Phase 28 stage D: an open command menu owns the mouse, and on this
    // screen that is three things rather than flight's one - the wheel zoom,
    // the pan drag and the marker pick all hang off `inside`, so taking that
    // away takes away all three at once. Overlapping BUTTONS need no guard: the
    // menu is submitted after them, so it wins `m_activeId` on the press and
    // the button underneath never sees a release it was held for.
    const bool inside = !menuOpen && input.mousePosition.x >= view.min.x &&
                        input.mousePosition.x <= view.max.x && input.mousePosition.y >= view.min.y &&
                        input.mousePosition.y <= view.max.y;
    if (menuOpen) {
        // A drag already running when the menu opened would otherwise keep
        // panning and still report a click on release, which is the one way
        // the guard above can be got around.
        state.dragging = false;
    }

    float& zoom = state.zoom[tab];
    Vec2& pan = state.pan[tab];

    if (inside && input.scrollDelta != 0.0f) {
        const float previous = zoom;
        zoom = std::clamp(zoom * std::pow(kZoomPerNotch, input.scrollDelta), kMinZoom, kMaxZoom);
        // Keep whatever is under the cursor under the cursor: solve the
        // transform for the pan that fixes that point. Without this the map
        // slides away from wherever you were trying to look at.
        if (zoom != previous) {
            pan = sol::ui::panHoldingAnchor(pan, origin, input.mousePosition, previous, zoom);
        }
    }

    // Drag to pan. The press has to start inside the view, or dragging a
    // footer button across the map would scroll it.
    if (input.mousePressed && inside) {
        state.dragging = true;
        state.dragAnchor = input.mousePosition;
        state.dragPanStart = pan;
    }
    // Phase 15: the press is already spoken for by the pan, so a click is a
    // RELEASE that never became a drag. Tested before `dragging` is cleared,
    // because that flag is the only record that the press started in here.
    clicked = false;
    if (state.dragging && !input.mouseDown) {
        const float dx = input.mousePosition.x - state.dragAnchor.x;
        const float dy = input.mousePosition.y - state.dragAnchor.y;
        clicked = dx * dx + dy * dy <= kClickSlopPixels * kClickSlopPixels;
    }
    if (!input.mouseDown) {
        state.dragging = false;
    }
    if (state.dragging) {
        pan = {state.dragPanStart.x + (input.mousePosition.x - state.dragAnchor.x),
               state.dragPanStart.y + (input.mousePosition.y - state.dragAnchor.y)};
    }

    const Vec2 slack = sol::ui::panLimit(view, zoom);
    pan.x = std::clamp(pan.x, -slack.x, slack.x);
    pan.y = std::clamp(pan.y, -slack.y, slack.y);

    return {origin, pan, zoom};
}

// Labels sit to the right of their marker, and on a crowded map they land on
// top of each other — a station, its gates and an asteroid field can project
// within a few pixels. This pushes a colliding label down until it clears and
// ties it back to its dot with a leader line, which is the ordinary
// cartographic answer and keeps every dot where it honestly belongs.
class LabelPlacer
{
public:
    void place(UiContext& ui,
               const assets::FontStyleRecord& style,
               Vec2 anchor,
               const char* text,
               const Color& color,
               float gap)
    {
        if (text == nullptr || text[0] == '\0') {
            return;
        }
        const float height = style.lineHeight > 1.0f ? style.lineHeight : 14.0f;
        // Estimated rather than measured: the draw list has no width query,
        // and for keeping names off each other an estimate is enough.
        const float width = std::min(130.0f,
                                     8.0f + 0.52f * style.pixelSize *
                                                static_cast<float>(std::char_traits<char>::length(text)));

        // Try the natural spot first, then the other side of the marker, then
        // step away vertically in both directions alternately. Most labels
        // find a home within a step or two, which matters: a leader line is
        // legible but a map full of them is not.
        const auto boxAt = [&](bool rightSide, float dy) {
            const float left = rightSide ? anchor.x + gap : anchor.x - gap - width;
            return Rect{{left, anchor.y - height * 0.5f + dy}, {left + width, anchor.y + height * 0.5f + dy}};
        };
        Rect box = boxAt(true, 0.0f);
        bool placedCleanly = !collides(box);
        if (!placedCleanly) {
            for (int step = 0; step <= 6 && !placedCleanly; ++step) {
                for (const bool rightSide : {true, false}) {
                    for (const float sign : {-1.0f, 1.0f}) {
                        if (step == 0 && (sign < 0.0f || rightSide)) {
                            continue; // already tried right/0
                        }
                        const Rect candidate = boxAt(rightSide, sign * height * step);
                        if (!collides(candidate)) {
                            box = candidate;
                            placedCleanly = true;
                            break;
                        }
                    }
                    if (placedCleanly) {
                        break;
                    }
                }
            }
        }
        if (!placedCleanly) {
            box = boxAt(true, height * 7.0f); // give up gracefully rather than overlap
        }
        m_placed.push_back(box);

        // A leader whenever the label is not sitting right beside its dot.
        const Vec2 labelEdge{box.min.x < anchor.x ? box.max.x + 3.0f : box.min.x - 3.0f,
                             (box.min.y + box.max.y) * 0.5f};
        if (std::abs(labelEdge.y - anchor.y) > 1.0f || box.min.x < anchor.x) {
            ui.drawList().addLine(anchor, labelEdge, color.withAlpha(0.4f), 1.0f);
        }
        ui.drawList().addTextInBox(
            style, box, text, color, box.min.x < anchor.x ? TextAlign::Right : TextAlign::Left);
    }

private:
    [[nodiscard]] bool collides(const Rect& box) const
    {
        for (const Rect& other : m_placed) {
            if (box.min.x < other.max.x && box.max.x > other.min.x && box.min.y < other.max.y &&
                box.max.y > other.min.y) {
                return true;
            }
        }
        return false;
    }

    std::vector<Rect> m_placed;
};

// --- Galaxy view ------------------------------------------------------------

void drawGalaxyMap(UiContext& ui,
                   const MapPanel& panel,
                   const Rect& view,
                   int selected,
                   const MapView& magnify,
                   sol::ui::NearestPick& pick)
{
    ui.drawList().addRect(view, ui.theme().background.withAlpha(0.55f));
    ui.drawList().addRectOutline(view, ui.theme().panelEdge, 1.0f);
    if (panel.systems.empty()) {
        return;
    }
    const sol::ui::MapProjection fitted = sol::ui::fitGalaxyMap(panel.systems, view);
    const auto project = [&](Vec2 position) { return magnify(fitted(position)); };
    ui.drawList().pushClip(view);

    // Lanes first, so nodes sit on top of them. A lane is only drawn when both
    // of its ends are known: the map is what the player has earned, not the
    // galaxy the generator made.
    for (const MapLaneRow& lane : panel.lanes) {
        if (!sol::ui::laneVisible(panel.systems, lane)) {
            continue;
        }
        const MapSystemRow& from = panel.systems[static_cast<std::size_t>(lane.from)];
        const MapSystemRow& to = panel.systems[static_cast<std::size_t>(lane.to)];
        ui.drawList().addLine(project(from.position),
                              project(to.position),
                              lane.onRoute ? kLaneRoute : kLane,
                              lane.onRoute ? 2.5f : 1.0f);
    }

    LabelPlacer labels;
    for (std::size_t i = 0; i < panel.systems.size(); ++i) {
        const MapSystemRow& system = panel.systems[i];
        if (!sol::ui::systemVisible(system)) {
            continue;
        }
        const Vec2 point = project(system.position);
        // Fed from the draw's own loop, so the pick sees exactly the dots that
        // were drawn - including the fog skip above it.
        pick.consider(i, point);
        const Color color = systemColor(panel, system);
        if (system.knowledge == MapKnowledge::Charted) {
            // Hollow: you know it is there and what it is called, no more.
            ui.drawList().addCircle(point, kNodeRadius, color.withAlpha(0.7f), 1.2f, 12);
        } else {
            ui.drawList().addRoundedRect({{point.x - kNodeRadius, point.y - kNodeRadius},
                                          {point.x + kNodeRadius, point.y + kNodeRadius}},
                                         kNodeRadius,
                                         color);
        }
        if (system.knowledge == MapKnowledge::Surveyed) {
            ui.drawList().addCircle(point, kNodeRadius + 3.5f, ui.theme().positive, 1.4f, 12);
        }
        if (static_cast<int>(i) == selected) {
            ui.drawList().addCircle(point, kNodeRadius + 7.0f, ui.theme().accent, 1.6f, 16);
        }
        if (system.current) {
            ui.drawList().addCircle(point, kNodeRadius + 10.0f, kCurrentRing, 1.8f, 16);
        }
        // A gold diamond above the node for a system holding bookmarks: the
        // same glyph the system map uses for them, so "my own mark" reads the
        // same at both scales. Selecting the system then plotting a route is
        // how you get back to it.
        if (system.bookmarkCount > 0) {
            const float reach = 3.5f;
            const Vec2 pin = {point.x, point.y - kNodeRadius - 8.0f};
            const Color gold = rgba(0xFFC850FFu);
            ui.drawList().addTriangle(
                {pin.x, pin.y - reach}, {pin.x - reach, pin.y}, {pin.x + reach, pin.y}, gold);
            ui.drawList().addTriangle(
                {pin.x, pin.y + reach}, {pin.x - reach, pin.y}, {pin.x + reach, pin.y}, gold);
        }
        // And a magenta ring around the node the tracked mission points at
        // (Phase 8i). Drawn widest of the lot so it survives a system that is
        // also current, also selected and also surveyed - being sent somewhere
        // is the state that must never be the one crowded out.
        if (system.hasObjective) {
            ui.drawList().addCircle(point, kNodeRadius + 13.0f, kObjective, 1.8f, 20);
        }
        // A contested system wears a ring in the ATTACKER's colour (Phase 8u),
        // drawn outside the objective ring rather than in place of it: they
        // are concentric, so a system that is both still reads as both. The
        // colour is the attacker's because the owner's is already the node.
        if (system.contested) {
            ui.drawList().addCircle(
                point,
                kNodeRadius + 16.0f,
                Color{system.contestColor.x, system.contestColor.y, system.contestColor.z, 1.0f},
                2.0f,
                24);
        }
        // A gate names where it leads, so a charted system carries its name -
        // dimmed, because that name is all you have until you go. The gap
        // clears the widest ring drawn above (the current-system ring at
        // kNodeRadius + 10), or the selection would sit on top of the text.
        labels.place(ui,
                     *ui.drawList().font()->style(ui.theme().smallStyle),
                     point,
                     system.name,
                     system.knowledge >= MapKnowledge::Visited ? ui.theme().textDim : ui.theme().textDisabled,
                     kNodeRadius + (system.contested      ? 19.0f
                                    : system.hasObjective ? 16.0f
                                                          : 13.0f));
    }
    ui.drawList().popClip();
}

// --- System view ------------------------------------------------------------

void drawSystemMap(UiContext& ui,
                   const MapPanel& panel,
                   const Rect& view,
                   int selected,
                   const MapView& magnify,
                   sol::ui::NearestPick& pick)
{
    ui.drawList().addRect(view, ui.theme().background.withAlpha(0.55f));
    ui.drawList().addRectOutline(view, ui.theme().panelEdge, 1.0f);
    if (panel.markers.empty()) {
        return;
    }
    const Vec2 center = {(view.min.x + view.max.x) * 0.5f, (view.min.y + view.max.y) * 0.5f};
    const float radius = std::min(view.width(), view.height()) * 0.5f - 26.0f;
    if (radius <= 0.0f) {
        return;
    }

    // Two tiers. The star holds the middle and the planets sit on their orbits
    // around it, which is the shape a system actually has. But everything you
    // fly to — stations, gates, signals, fields, wrecks — sits within a few
    // hundred thousand km of one planet, and at orbital scale that is a single
    // pixel, so the playfield is drawn expanded in a bubble pinned to that
    // planet. Distances inside each tier stay ordered and bearings stay exact;
    // it is the gap between the two scales that is compressed.
    const float orbitSpan = sol::ui::orbitExtent(panel.markers, panel.hubPosition);
    const float bubbleSpan = sol::ui::playfieldSpan(panel.markers);
    const float orbitRadius = radius * 0.70f;
    const float bubbleRadius = radius * 0.30f;
    const Vec2 hub = sol::ui::orbitMapPoint(panel.hubPosition, center, orbitRadius, orbitSpan);

    // Both tiers land in screen space first, then go through the magnifier
    // together - so zoom is one transform over the finished picture rather
    // than a change to either tier's curve.
    const auto project = [&](const MapMarkerRow& marker) {
        return magnify(marker.inPlayfield
                           ? sol::ui::playfieldPoint(marker.position, hub, bubbleRadius, bubbleSpan)
                           : sol::ui::orbitMapPoint(marker.position, center, orbitRadius, orbitSpan));
    };

    ui.drawList().pushClip(view);
    // An orbit ring per body, so the planets read as orbiting rather than as
    // dots that happen to be scattered.
    const Vec2 starPoint = magnify(center);
    for (const MapMarkerRow& marker : panel.markers) {
        if (marker.inPlayfield || marker.kind == MapMarkerRow::Kind::Star) {
            continue;
        }
        // Measured before the magnifier and scaled after, so the ring stays
        // concentric with the star under any pan.
        const Vec2 orbit = sol::ui::orbitMapPoint(marker.position, center, orbitRadius, orbitSpan);
        const float dx = orbit.x - center.x;
        const float dy = orbit.y - center.y;
        ui.drawList().addCircle(starPoint,
                                magnify.scaled(std::sqrt(dx * dx + dy * dy)),
                                ui.theme().panelEdge.withAlpha(0.45f),
                                1.0f,
                                48);
    }
    // The bubble's own edge, so the change of scale is visible rather than an
    // unannounced lie about how far apart these things are. Drawn only when
    // there is something in the playfield to expand: a Charted system (Phase
    // 8q) shows its star and nothing else, and an empty bubble there would be
    // a ring around a region the player has been told nothing about.
    const bool anyInPlayfield = std::any_of(panel.markers.begin(),
                                            panel.markers.end(),
                                            [](const MapMarkerRow& marker) { return marker.inPlayfield; });
    if (anyInPlayfield) {
        ui.drawList().addCircle(magnify(hub),
                                magnify.scaled(bubbleRadius + 8.0f),
                                ui.theme().panelEdge.withAlpha(0.8f),
                                1.0f,
                                40);
    }

    LabelPlacer labels;
    for (std::size_t i = 0; i < panel.markers.size(); ++i) {
        const MapMarkerRow& marker = panel.markers[i];
        const Vec2 point = project(marker);
        // Same rule as the galaxy map: the pick is fed by the draw, so it can
        // never disagree with it about where a marker is or whether it is
        // there at all.
        pick.consider(i, point);
        const Color color = markerColor(ui, marker);
        const float size = marker.kind == MapMarkerRow::Kind::Star     ? 6.0f
                           : marker.kind == MapMarkerRow::Kind::Planet ? 5.0f
                                                                       : 3.5f;
        if (marker.kind == MapMarkerRow::Kind::Bookmark) {
            // A diamond, so the player's own marks are distinguishable from
            // the galaxy's furniture by shape and not only by colour.
            const float reach = size + 1.5f;
            ui.drawList().addTriangle(
                {point.x, point.y - reach}, {point.x - reach, point.y}, {point.x + reach, point.y}, color);
            ui.drawList().addTriangle(
                {point.x, point.y + reach}, {point.x - reach, point.y}, {point.x + reach, point.y}, color);
        } else if (marker.kind == MapMarkerRow::Kind::Objective) {
            // A ring around a centre dot - the waypoint glyph, and the only
            // marker on the map that is a halo rather than a solid, so where
            // the mission says to go is findable without reading a legend
            // (Phase 8i).
            const float inner = size - 1.5f;
            ui.drawList().addRoundedRect(
                {{point.x - inner, point.y - inner}, {point.x + inner, point.y + inner}}, inner, color);
            ui.drawList().addCircle(point, size + 3.0f, color, 1.6f, 16);
        } else {
            ui.drawList().addRoundedRect({{point.x - size, point.y - size}, {point.x + size, point.y + size}},
                                         marker.kind == MapMarkerRow::Kind::Gate ? 1.0f : size,
                                         color);
        }
        if (marker.targeted) {
            ui.drawList().addCircle(point, size + 6.0f, ui.theme().accent, 1.6f, 4);
        }
        if (static_cast<int>(i) == selected) {
            ui.drawList().addCircle(point, size + 9.0f, kCurrentRing, 1.4f, 16);
        }
        labels.place(ui,
                     *ui.drawList().font()->style(ui.theme().smallStyle),
                     point,
                     marker.name,
                     ui.theme().textDim,
                     size + 10.0f);
    }
    // The ship, where it actually is, which is inside the bubble.
    if (panel.hasShip) {
        const Vec2 ship = magnify(sol::ui::playfieldPoint(panel.shipPosition, hub, bubbleRadius, bubbleSpan));
        ui.drawList().addCircle(ship, 4.0f, ui.theme().textPrimary, 1.6f, 12);
    }
    ui.drawList().popClip();
}

// --- Lists ------------------------------------------------------------------

void drawSystemList(UiContext& ui, const MapPanel& panel, const Rect& bounds, MapScreenState& state)
{
    float contentHeight = 0.0f;
    for (const MapSystemRow& system : panel.systems) {
        if (system.knowledge != MapKnowledge::Unknown) {
            contentHeight += kRowHeight + ui.theme().spacing;
        }
    }
    const Rect content = ui.beginScroll(bounds, contentHeight, state.scroll[MapScreenState::Galaxy]);
    Column column(content, 0.0f, ui.theme().spacing);
    for (std::size_t i = 0; i < panel.systems.size(); ++i) {
        const MapSystemRow& system = panel.systems[i];
        if (system.knowledge == MapKnowledge::Unknown) {
            continue;
        }
        const Rect row = column.row(kRowHeight);
        ui.pushId(static_cast<int>(i));
        Row cursor(row, ui.theme().spacing);
        const Rect marker = cursor.cell(14.0f);
        const Rect name = cursor.remaining();
        // A dot in the owner's color carries the faction without a column.
        ui.drawList().addRoundedRect({{marker.min.x + 3.0f, marker.min.y + kRowHeight * 0.5f - 4.0f},
                                      {marker.min.x + 11.0f, marker.min.y + kRowHeight * 0.5f + 4.0f}},
                                     4.0f,
                                     systemColor(panel, system));
        if (ui.selectable(name, system.name, static_cast<int>(i) == state.selectedSystem)) {
            state.selectedSystem = static_cast<int>(i);
        }
        // With the trade overlay up the right-hand tag is the price, because
        // that is the thing being compared; without it, where you are and
        // where you are going.
        if (panel.tradeCommodity >= 0) {
            if (system.hasTrade) {
                char buffer[32] = {};
                std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(system.tradePrice));
                clipped(ui, name, buffer, tradeColor(system), ui.theme().smallStyle, TextAlign::Right);
            } else {
                clipped(
                    ui, name, "no data", ui.theme().textDisabled, ui.theme().smallStyle, TextAlign::Right);
            }
        } else if (system.current) {
            clipped(ui, name, "here", ui.theme().accent, ui.theme().smallStyle, TextAlign::Right);
        } else if (system.onRoute) {
            clipped(ui, name, "route", kLaneRoute, ui.theme().smallStyle, TextAlign::Right);
        } else if (system.knowledge == MapKnowledge::Charted) {
            clipped(ui, name, "uncharted", ui.theme().textDisabled, ui.theme().smallStyle, TextAlign::Right);
        }
        ui.popId();
    }
    ui.endScroll();
}

void drawMarkerList(UiContext& ui, const MapPanel& panel, const Rect& bounds, MapScreenState& state)
{
    const float contentHeight = static_cast<float>(panel.markers.size()) * (kRowHeight + ui.theme().spacing);
    const Rect content = ui.beginScroll(bounds, contentHeight, state.scroll[MapScreenState::System]);
    Column column(content, 0.0f, ui.theme().spacing);
    for (std::size_t i = 0; i < panel.markers.size(); ++i) {
        const MapMarkerRow& marker = panel.markers[i];
        const Rect row = column.row(kRowHeight);
        ui.pushId(static_cast<int>(i));
        Row cursor(row, ui.theme().spacing);
        const Rect dot = cursor.cell(14.0f);
        const Rect detail = cursor.cellFromRight(96.0f);
        const Rect name = cursor.remaining();
        ui.drawList().addRoundedRect({{dot.min.x + 3.0f, dot.min.y + kRowHeight * 0.5f - 4.0f},
                                      {dot.min.x + 11.0f, dot.min.y + kRowHeight * 0.5f + 4.0f}},
                                     4.0f,
                                     markerColor(ui, marker));
        if (ui.selectable(name, marker.name, static_cast<int>(i) == state.selectedMarker)) {
            state.selectedMarker = static_cast<int>(i);
        }
        clipped(ui, detail, marker.detail, ui.theme().textDim, ui.theme().smallStyle, TextAlign::Right);
        ui.popId();
    }
    ui.endScroll();
}

} // namespace

bool buildMapScreen(UiContext& ui, MapPanel& panel, MapScreenState& state, const MapInput& input)
{
    panel.action = MapAction{};
    if (ui.drawList().font() == nullptr) {
        return false;
    }
    const Vec2 screen = ui.screenSize();
    // The map owns the view while it is up, like the station screen: the
    // flight HUD showing through a half-lit map is noise, not atmosphere.
    ui.drawList().addRect({{0.0f, 0.0f}, screen}, ui.theme().background);
    ui.pushId("map");

    // The map follows the ship (Phase 10, playtest session 8 note 1). Snapping
    // on CHANGE rather than every frame is the whole of the judgement here: a
    // player browsing the galaxy must not be dragged back to their own system,
    // and "Show Current" below stays the way to ask for it deliberately. This
    // sits above the tab branch so it holds whichever tab is up, and so a jump
    // taken with the map closed is picked up by the first build after it.
    //
    // Clearing the marker matters as much as the snap: selectedMarker indexes
    // the new system's list, and the range guard further down only clamps it,
    // so a system with as many markers would leave the selection silently
    // pointing at a different object.
    if (panel.currentIndex >= 0 && panel.currentIndex != state.followedSystem) {
        state.followedSystem = panel.currentIndex;
        state.selectedSystem = panel.currentIndex;
        state.selectedMarker = -1;
    }

    const float width = std::min(screen.x - 80.0f, 1180.0f);
    const float height = std::min(screen.y - 70.0f, 780.0f);
    const Rect frame = {{(screen.x - width) * 0.5f, (screen.y - height) * 0.5f},
                        {(screen.x + width) * 0.5f, (screen.y + height) * 0.5f}};
    ui.panel(frame);

    Column outer(frame, ui.theme().padding, ui.theme().spacing);

    // Header: where you are and how much of the galaxy you have seen. Its row
    // is claimed here so the layout below is unchanged, but it is drawn after
    // the tabs, because since Phase 8q the System tab names whatever system it
    // is showing and that must change on the same frame the tab does.
    const Rect header = outer.row(kHeaderHeight);

    int tab = state.tab;
    if (ui.tabs(outer.row(kTabHeight), std::span<const char* const>(kTabLabels), tab)) {
        state.tab = tab;
    }

    ui.label(header,
             state.tab == MapScreenState::System ? panel.viewSystemName : panel.currentSystem,
             ui.theme().textPrimary,
             ui.theme().headingStyle);
    ui.label(header, panel.knownSummary, ui.theme().textDim, ui.theme().bodyStyle, TextAlign::Right);

    // Footer first: the body is whatever the header, tabs, and footer leave,
    // and it has to be known before either view draws into it.
    const Rect bodyAll = outer.remaining();
    const Rect footer = {{bodyAll.min.x, bodyAll.max.y - kFooterHeight}, bodyAll.max};
    const Rect body = {bodyAll.min, {bodyAll.max.x, footer.min.y - ui.theme().spacing}};
    Row split(body, ui.theme().padding);
    const Rect listBounds = split.cell(kListWidth);
    const Rect mapBounds = split.remaining();

    // Zoom and pan are read before either view draws, since both need the
    // transform and the footer needs to know whether there is anything to
    // reset. The wheel only bites inside the map area, so it still scrolls
    // the list when the cursor is over the list.
    bool mapClicked = false;
    const MapView magnify = updateMapView(ui, mapBounds, state, input.commandMenuOpen, mapClicked);
    // Phase 28 stage D: the right button asks the same question of the same
    // surface. Only inside the map area - the phase does not put a menu on a
    // list row or a footer button - and only where the button went down, which
    // is the anchor rule the flight view already uses.
    const bool rightInMap = input.rightClicked && mapBounds.contains(input.rightCursor);
    // Phase 15: a click on the map selects the same way a click on a list row
    // does. Resolved inside whichever view draws, because that is where a
    // marker's screen position is worked out.
    //
    // ⚑ ONE pick per frame, from whichever button asked. Two buttons cannot
    // both come up as a click on one frame in any real hand, and the right one
    // is answered at its press point rather than at the cursor.
    sol::ui::NearestPick pick{};
    if (rightInMap) {
        pick = sol::ui::NearestPick(input.rightCursor, kMapGrabPixels);
    } else if (mapClicked) {
        pick = sol::ui::NearestPick(ui.input().mousePosition, kMapGrabPixels);
    }
    const bool zoomed = state.zoom[static_cast<std::size_t>(state.tab)] > 1.0f ||
                        state.pan[static_cast<std::size_t>(state.tab)].x != 0.0f ||
                        state.pan[static_cast<std::size_t>(state.tab)].y != 0.0f;
    bool resetView = false;

    bool closed = false;
    if (state.tab == MapScreenState::Galaxy) {
        if (state.selectedSystem < 0 || state.selectedSystem >= static_cast<int>(panel.systems.size())) {
            state.selectedSystem = panel.currentIndex;
        }
        // Trade overlay picker (Phase 8g), above the list. One cycling button
        // rather than a row of cells: five choices across a column this
        // narrow leaves no room for a name like "Refined Metal", and a
        // truncated legend is worse than an extra click.
        Column listColumn(listBounds, 0.0f, ui.theme().spacing);
        const Rect overlayRow = listColumn.row(kRowHeight);
        {
            const int commodityCount = static_cast<int>(panel.commodityNames.size());
            char label[64] = {};
            if (panel.tradeCommodity >= 0 && panel.tradeCommodity < commodityCount) {
                std::snprintf(label,
                              sizeof(label),
                              "Colour: %s",
                              panel.commodityNames[static_cast<std::size_t>(panel.tradeCommodity)]);
            } else {
                std::snprintf(label, sizeof(label), "Colour: owners");
            }
            if (ui.button(overlayRow, label, commodityCount > 0)) {
                // Owners -> each commodity -> back to owners.
                const int next = panel.tradeCommodity + 1;
                panel.action = {.kind = MapAction::Kind::SetTradeCommodity,
                                .index = next >= commodityCount ? -1 : next};
            }
        }
        drawSystemList(ui, panel, listColumn.remaining(), state);
        drawGalaxyMap(ui, panel, mapBounds, state.selectedSystem, magnify, pick);
        // Selecting only: a click on the map never plots, targets or engages.
        // The footer buttons stay the only things that act, so a stray click
        // on a crowded map cannot send the ship anywhere.
        if (pick.result() != sol::ui::kNoPick) {
            state.selectedSystem = static_cast<int>(pick.result());
        }

        // Footer: what the selection is, and what can be done with it.
        Row buttons(footer, ui.theme().spacing);
        const Rect closeCell = buttons.cellFromRight(kButtonWidth);
        const Rect resetCell = buttons.cellFromRight(kButtonWidth);
        const Rect clearCell = buttons.cellFromRight(kButtonWidth);
        const Rect plotCell = buttons.cellFromRight(kButtonWidth);
        const Rect detailCell = buttons.remaining();

        const MapSystemRow* selected =
            state.selectedSystem >= 0 && state.selectedSystem < static_cast<int>(panel.systems.size())
                ? &panel.systems[static_cast<std::size_t>(state.selectedSystem)]
                : nullptr;
        // With the overlay up the footer explains the color scale, which is
        // the thing a player needs to read the map at all.
        clipped(ui,
                detailCell,
                panel.tradeCommodity >= 0                            ? panel.tradeSummary
                : selected != nullptr && selected->detail[0] != '\0' ? selected->detail
                                                                     : panel.routeSummary,
                ui.theme().textDim);
        const bool plottable = selected != nullptr && !selected->current;
        if (ui.button(plotCell, "Plot Route", plottable)) {
            panel.action = {MapAction::Kind::PlotRoute, state.selectedSystem};
        }
        if (ui.button(clearCell, "Clear Route", panel.routeSummary[0] != '\0')) {
            panel.action = {MapAction::Kind::ClearRoute, -1};
        }
        if (ui.button(resetCell, "Reset View", zoomed)) {
            resetView = true;
        }
        if (ui.button(closeCell, "Close")) {
            closed = true;
        }
    } else {
        if (state.selectedMarker < 0 || state.selectedMarker >= static_cast<int>(panel.markers.size())) {
            state.selectedMarker = panel.markers.empty() ? -1 : 0;
        }
        // Which system is on show and how far away it is (Phase 8q). It sits
        // above the list, where the galaxy tab keeps its overlay picker, so
        // the two tabs have the same shape - and it is drawn in the accent
        // colour when the answer is "not where you are", because everything
        // below it is then a report rather than a live readout.
        Column listColumn(listBounds, 0.0f, ui.theme().spacing);
        clipped(ui,
                listColumn.row(kRowHeight),
                panel.viewSummary,
                panel.viewIsCurrent ? ui.theme().textDim : ui.theme().accent,
                ui.theme().smallStyle);
        drawMarkerList(ui, panel, listColumn.remaining(), state);
        drawSystemMap(ui, panel, mapBounds, state.selectedMarker, magnify, pick);
        if (pick.result() != sol::ui::kNoPick) {
            state.selectedMarker = static_cast<int>(pick.result());
        }

        // ⚑⚑ THE MENU IS OFFERED ONLY WHERE THE COMMANDS MEAN SOMETHING, AND
        // THE MAP ALREADY KNOWS WHERE THAT IS. Every verb the menu carries -
        // dock, hail, orbit, follow - is about the system the ship is standing
        // in, so a remote view gets no menu, exactly as it gets Plot Route and
        // Show Current instead of Set Target and Autopilot. The Galaxy tab gets
        // none either: it is drawn from systems, not from nav slots, and
        // "Orbit" is not a thing you say to a star four jumps away.
        if (rightInMap && panel.viewIsCurrent) {
            const MapMarkerRow* hit =
                pick.result() != sol::ui::kNoPick ? &panel.markers[pick.result()] : nullptr;
            // Phase 15's rule inherited whole: the marker's own NAV SLOT, never
            // its row number, which counts only what the fog left visible.
            const int slot = hit != nullptr && hit->navTarget != sol::ui::kNoNavTarget
                                 ? static_cast<int>(hit->navTarget)
                                 : -1;
            panel.action = {MapAction::Kind::CommandMenu, slot};
        }

        // Footer computed above; the buttons sit on the last row of the frame.
        Row buttons(footer, ui.theme().spacing);
        const Rect closeCell = buttons.cellFromRight(kButtonWidth);
        const Rect resetCell = buttons.cellFromRight(kButtonWidth);
        const Rect deleteCell = buttons.cellFromRight(kButtonWidth);
        const Rect secondCell = buttons.cellFromRight(kButtonWidth);
        const Rect firstCell = buttons.cellFromRight(kButtonWidth);
        const Rect detailCell = buttons.remaining();

        const bool hasMarker = state.selectedMarker >= 0;
        const MapMarkerRow* marker =
            hasMarker ? &panel.markers[static_cast<std::size_t>(state.selectedMarker)] : nullptr;
        // Only the player's own marks can be deleted; a planet cannot.
        const bool deletable = marker != nullptr && marker->kind == MapMarkerRow::Kind::Bookmark;
        clipped(ui, detailCell, marker != nullptr ? marker->detail : "nothing in range", ui.theme().textDim);
        // Set Target and Autopilot are statements about a nav-target slot in
        // the system the player is standing in, so on a remote view their two
        // cells carry the two actions that *are* answerable from a distance.
        // Swapped rather than greyed: a route is the whole point of looking at
        // a system you are not in, and it would have no button otherwise.
        // Phase 15: both carry the marker's own nav slot, never its row number.
        // The row number counts only what the fog left visible, so on any map
        // with an undiscovered station it names a different target than the one
        // on screen - or names a hidden one, which selectTarget refuses without
        // saying so, which is what "the buttons don't work" actually was.
        const bool targetable = marker != nullptr && marker->navTarget != sol::ui::kNoNavTarget;
        if (panel.viewIsCurrent) {
            if (ui.button(firstCell, "Set Target", targetable)) {
                panel.action = {MapAction::Kind::SelectMarker, static_cast<int>(marker->navTarget)};
            }
            if (ui.button(secondCell, "Autopilot", targetable)) {
                panel.action = {MapAction::Kind::Autopilot, static_cast<int>(marker->navTarget)};
            }
        } else {
            if (ui.button(firstCell, "Plot Route")) {
                panel.action = {MapAction::Kind::PlotRoute, panel.viewSystem};
            }
            if (ui.button(secondCell, "Show Current")) {
                // Pure view state, like the trade picker: the fill reads the
                // galaxy selection back on the next frame.
                state.selectedSystem = panel.currentIndex;
                state.selectedMarker = -1;
            }
        }
        if (ui.button(deleteCell, "Delete", deletable)) {
            panel.action = {.kind = MapAction::Kind::DeleteBookmark,
                            .index = state.selectedMarker,
                            .bookmarkId = marker->bookmarkId};
        }
        if (ui.button(resetCell, "Reset View", zoomed)) {
            resetView = true;
        }
        if (ui.button(closeCell, "Close")) {
            closed = true;
        }
    }

    // Applied after drawing so the button reads against the frame the player
    // was actually looking at; the next frame draws the reset view.
    if (resetView) {
        state.zoom[static_cast<std::size_t>(state.tab)] = 1.0f;
        state.pan[static_cast<std::size_t>(state.tab)] = {};
        state.dragging = false;
    }

    ui.popId();
    if (ui.cancelRequested()) {
        closed = true;
    }
    if (closed) {
        panel.action = {MapAction::Kind::Close, -1};
    }
    return closed;
}

} // namespace game
