#include "target_pick.hpp"

#include "game_ui.hpp"
#include "space_world.hpp"

#include "sol/core/log.hpp"
#include "sol/ui/pick.hpp"
#include "sol/ui/radar_projection.hpp"

#include <algorithm>
#include <cstring>

namespace game {
namespace {

using sol::core::DVec3;
using sol::core::Vec2;
using sol::core::Vec3;

// The disc shows at most this many contacts, nearest first. A system can hold
// dozens of nav points and a fight can add more ships; past this the disc
// stops being a glance and starts being a puzzle.
constexpr std::size_t kRadarMaxContacts = 32;

sol::ui::RadarKind radarKindOf(SpaceWorld::NavKind kind)
{
    switch (kind) {
    case SpaceWorld::NavKind::Station: return sol::ui::RadarKind::Station;
    case SpaceWorld::NavKind::Gate: return sol::ui::RadarKind::Gate;
    case SpaceWorld::NavKind::Planet: return sol::ui::RadarKind::Planet;
    case SpaceWorld::NavKind::Star: return sol::ui::RadarKind::Star;
    case SpaceWorld::NavKind::Signal: return sol::ui::RadarKind::Signal;
    case SpaceWorld::NavKind::Field: return sol::ui::RadarKind::Field;
    case SpaceWorld::NavKind::Wreck: return sol::ui::RadarKind::Wreck;
    case SpaceWorld::NavKind::Bookmark: return sol::ui::RadarKind::Bookmark;
    case SpaceWorld::NavKind::Objective: return sol::ui::RadarKind::Objective;
    }
    return sol::ui::RadarKind::Signal;
}

sol::ui::RadarAttitude attitudeOf(const char* attitude)
{
    if (std::strcmp(attitude, "friendly") == 0) {
        return sol::ui::RadarAttitude::Friendly;
    }
    if (std::strcmp(attitude, "neutral") == 0) {
        return sol::ui::RadarAttitude::Neutral;
    }
    // An unaffiliated spawn has no faction to consult and Lua treats it as
    // player-hostile, so the empty attitude falls through to Hostile.
    return sol::ui::RadarAttitude::Hostile;
}

// How big a nav target is in the world, for the click box. A planet and a star
// carry their own radius already; a station and a gate are structures the
// player can see filling the view, and clicking the hull they are looking at
// has to select them. Everything else is a point and falls back to the grab
// floor: a signal or a bookmark has no body to click.
[[nodiscard]] double pickRadiusOf(const SpaceWorld& world, std::size_t navIndex)
{
    switch (world.navTargetKind(navIndex)) {
    case SpaceWorld::NavKind::Station: return kStationRadiusMeters;
    case SpaceWorld::NavKind::Gate: return kGateRadiusMeters;
    case SpaceWorld::NavKind::Planet:
    case SpaceWorld::NavKind::Star: return world.navTargets()[navIndex].surfaceRadius;
    case SpaceWorld::NavKind::Signal:
    case SpaceWorld::NavKind::Field:
    case SpaceWorld::NavKind::Wreck:
    case SpaceWorld::NavKind::Bookmark:
    case SpaceWorld::NavKind::Objective: break;
    }
    return 0.0;
}

// Everything selectable, as camera-space directions. Same list and same order
// as the radar fill's, because both are "what the player can point at" - the
// difference is only which projection answers the question.
void fillPickCandidates(const SpaceWorld& world, const ViewFrame& view,
                        std::vector<sol::ui::PickCandidate>& out)
{
    out.clear();
    const sol::core::Quat toCamera = conjugate(view.cameraOrientation);
    const float focal = sol::ui::focalLength(view.screenSize.y, view.tanHalfFovY);

    const auto push = [&](const DVec3& position, double surfaceRadius, std::size_t selection) {
        const DVec3 offset = position - view.cameraPosition;
        const double range = length(offset);
        if (range <= 0.0) {
            return; // sitting inside it; nothing sensible to project
        }
        sol::ui::PickCandidate candidate;
        candidate.directionCamera = rotate(toCamera, toVec3(normalize(offset)));
        // How big the thing is on screen, so a station is picked anywhere
        // across its disc rather than only at the point its centre projects
        // to. Small-angle: the error at anything the player can click past is
        // far under a pixel, and at point-blank the grab floor covers it.
        candidate.screenRadius = static_cast<float>(surfaceRadius / range) * focal;
        candidate.rangeMeters = range;
        candidate.selection = static_cast<std::uint32_t>(selection);
        out.push_back(candidate);
    };

    const std::span<const NavTarget> navTargets = world.navTargets();
    for (std::size_t i = 0; i < navTargets.size(); ++i) {
        push(navTargets[i].position, pickRadiusOf(world, i), i);
    }
    for (std::size_t i = 0; i < world.contactCount(); ++i) {
        // A ship carries no radius on its TargetInfo, so it is picked on the
        // grab floor - which at any range a ship is a fightable distance away
        // is already wider than the ship is drawn.
        const TargetInfo contact = world.contactInfo(i);
        push(contact.nav.position, 0.0, navTargets.size() + i);
    }
}

} // namespace

void fillRadarContacts(const SpaceWorld& world, std::vector<sol::ui::RadarContact>& out)
{
    out.clear();
    const sol::sim::ShipState ship = world.shipState();
    // body -> sim becomes sim -> body, which is what puts the disc in the
    // ship's frame and makes it turn when the ship turns.
    const sol::core::Quat toLocal = conjugate(ship.orientation);
    const std::size_t selected = world.currentTargetIndex();

    const auto push = [&](const DVec3& position, sol::ui::RadarKind kind,
                          sol::ui::RadarAttitude attitude, std::size_t selection) {
        const DVec3 offset = position - ship.position;
        out.push_back({.offset = rotate(toLocal, toVec3(offset)),
                       .kind = kind,
                       .attitude = attitude,
                       .isTarget = selection == selected,
                       .selection = static_cast<std::uint32_t>(selection)});
    };

    const std::span<const NavTarget> navTargets = world.navTargets();
    for (std::size_t i = 0; i < navTargets.size(); ++i) {
        push(navTargets[i].position, radarKindOf(world.navTargetKind(i)),
             sol::ui::RadarAttitude::Neutral, i);
    }
    for (std::size_t i = 0; i < world.contactCount(); ++i) {
        const TargetInfo contact = world.contactInfo(i);
        push(contact.nav.position, sol::ui::RadarKind::Ship, attitudeOf(contact.attitude),
             navTargets.size() + i);
    }

    // Nearest first, then truncate: a full disc should be the things closest
    // to the ship, not whichever happened to be generated first. Each contact
    // carries its own selection index, so the sort costs nothing (Phase 8j) -
    // before that, the sort was what made a blip un-clickable in principle.
    std::sort(out.begin(), out.end(),
              [](const sol::ui::RadarContact& a, const sol::ui::RadarContact& b) {
                  return lengthSquared(a.offset) < lengthSquared(b.offset);
              });
    if (out.size() > kRadarMaxContacts) {
        out.resize(kRadarMaxContacts);
    }
}

PickResult pickTarget(const SpaceWorld& world, Vec2 cursor)
{
    const ViewFrame& view = world.viewFrame();
    if (!view.valid || view.screenSize.x <= 0.0f || view.screenSize.y <= 0.0f) {
        return {};
    }

    // The disc first: it is drawn over the world, so a click inside it is a
    // click on the radar even when something is behind it in space.
    const Vec2 discCenter = sol::ui::radarCenter(view.screenSize, kHudMargin);
    if (sol::ui::insideDisc(cursor, discCenter, sol::ui::kRadarRadius)) {
        std::vector<sol::ui::RadarContact> contacts;
        fillRadarContacts(world, contacts);
        const float range = sol::ui::radarRange(sol::ui::kRadarRangeMeters);
        std::vector<Vec2> dots;
        dots.reserve(contacts.size());
        for (const sol::ui::RadarContact& contact : contacts) {
            dots.push_back(sol::ui::radarDot(contact.offset, discCenter,
                                             sol::ui::kRadarPlotRadius, range,
                                             sol::ui::kRadarStalkLimit));
        }
        const std::size_t hit = sol::ui::pickNearestPoint(dots, cursor);
        if (hit == sol::ui::kNoPick) {
            // A miss inside the disc stays a radar miss rather than falling
            // through to the world behind it: the disc is opaque, and picking
            // whatever happened to be under it would be a mystery selection.
            return {};
        }
        return {PickRoute::Radar, contacts[hit].selection};
    }

    std::vector<sol::ui::PickCandidate> candidates;
    fillPickCandidates(world, view, candidates);
    const Vec2 center = {view.screenSize.x * 0.5f, view.screenSize.y * 0.5f};
    const float focal = sol::ui::focalLength(view.screenSize.y, view.tanHalfFovY);
    const std::size_t hit = sol::ui::pickNearest(candidates, cursor, center, focal);
    if (hit == sol::ui::kNoPick) {
        return {};
    }
    return {PickRoute::Space, candidates[hit].selection};
}

PickResult pickBoresight(const SpaceWorld& world)
{
    const ViewFrame& view = world.viewFrame();
    if (!view.valid) {
        return {};
    }
    return pickTarget(world, {view.screenSize.x * 0.5f, view.screenSize.y * 0.5f});
}

bool selectPicked(SpaceWorld& world, const PickResult& result)
{
    if (result.route == PickRoute::Miss || !world.selectTarget(result.target)) {
        return false;
    }
    const TargetInfo info = world.currentTargetInfo();
    if (info.isShip) {
        SOL_LOG_INFO("Target: %s [%s]", info.nav.name.c_str(),
                     info.attitude[0] != '\0' ? info.attitude : "unaffiliated");
    } else {
        SOL_LOG_INFO("Target: %s", info.nav.name.c_str());
    }
    return true;
}

} // namespace game
