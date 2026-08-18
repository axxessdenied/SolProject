#include "map_ui.hpp"

#include "sol/ui/map_projection.hpp"

#include <algorithm>
#include <cstdio>

namespace game {

using namespace sol;

namespace {

[[nodiscard]] const char* store(std::deque<std::string>& text, std::string value)
{
    text.push_back(std::move(value));
    return text.back().c_str();
}

[[nodiscard]] std::string formatNumber(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
    return buffer;
}

[[nodiscard]] std::string formatDistance(double meters)
{
    char buffer[48];
    if (meters < 10'000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f m", meters);
    } else if (meters < 1.0e9) {
        std::snprintf(buffer, sizeof(buffer), "%.0f km", meters / 1000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f Mkm", meters / 1.0e9);
    }
    return buffer;
}

[[nodiscard]] const char* regionName(sim::Region region)
{
    switch (region) {
    case sim::Region::Core:
        return "core";
    case sim::Region::Frontier:
        return "frontier";
    case sim::Region::Fringe:
        return "fringe";
    }
    return "?";
}

[[nodiscard]] ui::MapKnowledge toMapKnowledge(sim::KnowledgeState state)
{
    switch (state) {
    case sim::KnowledgeState::Unknown:
        return ui::MapKnowledge::Unknown;
    case sim::KnowledgeState::Charted:
        return ui::MapKnowledge::Charted;
    case sim::KnowledgeState::Visited:
        return ui::MapKnowledge::Visited;
    case sim::KnowledgeState::Surveyed:
        return ui::MapKnowledge::Surveyed;
    }
    return ui::MapKnowledge::Unknown;
}

[[nodiscard]] const char* knowledgeName(sim::KnowledgeState state)
{
    switch (state) {
    case sim::KnowledgeState::Unknown:
        return "unknown";
    case sim::KnowledgeState::Charted:
        return "charted";
    case sim::KnowledgeState::Visited:
        return "visited";
    case sim::KnowledgeState::Surveyed:
        return "surveyed";
    }
    return "?";
}

[[nodiscard]] ui::MapMarkerRow::Kind toMarkerKind(SpaceWorld::NavKind kind)
{
    switch (kind) {
    case SpaceWorld::NavKind::Station:
        return ui::MapMarkerRow::Kind::Station;
    case SpaceWorld::NavKind::Gate:
        return ui::MapMarkerRow::Kind::Gate;
    case SpaceWorld::NavKind::Planet:
        return ui::MapMarkerRow::Kind::Planet;
    case SpaceWorld::NavKind::Star:
        return ui::MapMarkerRow::Kind::Star;
    case SpaceWorld::NavKind::Signal:
        return ui::MapMarkerRow::Kind::Signal;
    case SpaceWorld::NavKind::Field:
        return ui::MapMarkerRow::Kind::Field;
    case SpaceWorld::NavKind::Wreck:
        return ui::MapMarkerRow::Kind::Wreck;
    case SpaceWorld::NavKind::Bookmark:
        return ui::MapMarkerRow::Kind::Bookmark;
    case SpaceWorld::NavKind::Objective:
        return ui::MapMarkerRow::Kind::Objective;
    }
    return ui::MapMarkerRow::Kind::Station;
}

// --- Remote systems (Phase 8q) ----------------------------------------------

// The markers for a system the player is NOT standing in. Everything here is
// already resident: SystemSpec carries the bodies, stations and gates with
// their positions, and the two content sims answer signalsFor/fieldsFor/
// wrecksIn/bookmarksIn for any system index. So nothing is regenerated and no
// state is invented - this reads the same facts the local path reads, minus
// the ones that only exist because the player is there (rock counts, wreck
// cargo, distance from the ship, the target ring).
//
// What may appear at all goes through ui::markerVisible, which holds the fog
// rule beside systemVisible/laneVisible; the per-signal discovery bit is
// applied here on top of it, because the rung cannot answer for an individual
// site. Order matches the target cycle - stations, gates, planets, star, then
// the dynamic tail - so a player who can read one system map can read both.
void fillRemoteMarkers(const SpaceWorld& world, std::uint32_t system,
                       std::deque<std::string>& text, std::vector<ui::MapMarkerRow>& markerRows)
{
    using Kind = ui::MapMarkerRow::Kind;
    const sim::Galaxy& galaxy = world.galaxy();
    const sim::SurveySim& survey = world.survey();
    const sim::SystemSpec& spec = galaxy.systems[system];
    const ui::MapKnowledge rung = toMapKnowledge(survey.knowledge(system));
    const core::DVec3 hub = sim::playfieldHub(spec);

    // "412 Mkm from your ship" would be a number about a place three jumps
    // behind the player, so each marker is measured from the origin of its own
    // tier instead - planets from the star they orbit, everything in the
    // playfield from the planet it clusters around. That is the same split the
    // map itself draws, and measuring the whole system from the star would
    // instead report the hub's orbital radius nine times over: at Lyrioa every
    // station and gate would read ~51 Mkm and none of them would be
    // distinguishable from the planet they are parked beside.
    const auto add = [&](Kind kind, std::string name, const core::DVec3& position,
                         std::string detail, bool scanned, std::uint32_t bookmarkId) {
        if (!ui::markerVisible(rung, kind)) {
            return;
        }
        const bool orbital = kind == Kind::Star || kind == Kind::Planet;
        const core::DVec3 offset = orbital ? position : position - hub;
        const double distance = length(offset);
        // The star is the origin of the orbital tier, so its distance is zero
        // by construction and saying "0 m" is noise rather than information.
        std::string full = kind == Kind::Star ? std::string() : formatDistance(distance);
        if (!detail.empty()) {
            full += full.empty() ? detail : " - " + detail;
        }
        markerRows.push_back({.kind = kind,
                              .name = store(text, std::move(name)),
                              .detail = store(text, std::move(full)),
                              .position = {static_cast<float>(offset.x),
                                           static_cast<float>(offset.z)},
                              .distanceMeters = distance,
                              .scanned = scanned,
                              .targeted = false,
                              .bookmarkId = bookmarkId,
                              .inPlayfield = !orbital});
    };

    for (const sim::StationSpec& station : spec.stations) {
        add(Kind::Station, station.name, station.position, {}, false, 0);
    }
    for (const sim::GateSpec& gate : spec.gates) {
        add(Kind::Gate, "Gate: " + galaxy.systems[gate.toSystem].name, gate.position, {}, false, 0);
    }
    // Body index 0 is the star and 1.. are the planets in spec order, which is
    // the indexing SurveySim::bodyScanned answers against.
    for (std::uint32_t i = 0; i < spec.planets.size(); ++i) {
        const sim::PlanetSpec& planet = spec.planets[i];
        const bool scanned = survey.bodyScanned(system, i + 1);
        add(Kind::Planet, planet.name, planet.position, scanned ? "scanned" : "unscanned", scanned,
            0);
    }
    {
        // At Charted the star is the only marker there is, so its detail is
        // where the reason for an otherwise empty map belongs.
        const bool scanned = survey.bodyScanned(system, 0);
        // Short, because the detail column is 96 px and clips from the left:
        // a sentence here survives as a mid-word tail. The galaxy tab's footer
        // already carries the "fly there to survey it" wording.
        const char* detail = rung < ui::MapKnowledge::Visited ? "charted only"
                             : scanned                        ? "scanned"
                                                              : "unscanned";
        add(Kind::Star, spec.name, {}, detail, scanned, 0);
    }

    // Sites are gated per signal rather than by the rung: an unswept system
    // shows none even once it is Visited, because none has been found.
    std::vector<sim::SignalSpec> signals;
    survey.signalsFor(galaxy, system, signals);
    std::size_t slot = 0;
    for (std::uint32_t i = 0; i < signals.size(); ++i) {
        if (!survey.signalDiscovered(system, i)) {
            continue;
        }
        const bool resolved = survey.signalResolved(system, i);
        add(Kind::Signal,
            signalTargetName(signals[i].kind, resolved, survey.signalEmptied(system, i), slot++),
            signals[i].position, resolved ? "scanned" : "unscanned", resolved, 0);
    }
    // Fields need no discovery bit - a field is a visible thing in the
    // playfield, which is the same rule the local nav list follows. Rock
    // counts are deliberately not read: that is a rocksFor walk per field for
    // a number the player has not been there to earn.
    std::vector<sim::AsteroidFieldSpec> fields;
    world.mining().fieldsFor(galaxy, system, fields);
    for (std::uint32_t i = 0; i < fields.size(); ++i) {
        add(Kind::Field, "Asteroid Field " + std::to_string(i + 1), fields[i].center, {}, false, 0);
    }
    std::vector<std::uint32_t> wreckIds;
    world.mining().wrecksIn(system, wreckIds);
    for (const std::uint32_t id : wreckIds) {
        const sim::WreckRecord* wreck = world.mining().wreck(id);
        if (wreck != nullptr) {
            add(Kind::Wreck, "Wreck: " + wreck->name, wreck->position, {}, false, 0);
        }
    }
    std::vector<std::uint32_t> bookmarkIds;
    survey.bookmarksIn(system, bookmarkIds);
    for (const std::uint32_t id : bookmarkIds) {
        const sim::Bookmark* mark = survey.bookmark(id);
        if (mark != nullptr) {
            add(Kind::Bookmark, "* " + mark->name, mark->position, "bookmark", false, id);
        }
    }
    // The tracked objective, when it is a FlyTo over there - which is exactly
    // the case a route-planning map exists to answer.
    const sim::MissionObjective* objective = world.trackedObjective();
    if (objective != nullptr && objective->kind == sim::ObjectiveKind::FlyTo
        && objective->system == system) {
        add(Kind::Objective, "> Objective", objective->position, objective->text, false, 0);
    }
}

// Everything that is true of the screen whichever system it is showing: the
// plotted route, the ledger line, where the player actually is, and the three
// spans. Shared by both marker paths so neither can forget half of it.
void fillMapTail(const SpaceWorld& world, std::deque<std::string>& text, ui::MapPanel& panel,
                 const std::vector<ui::MapSystemRow>& systemRows,
                 const std::vector<ui::MapLaneRow>& laneRows,
                 const std::vector<ui::MapMarkerRow>& markerRows)
{
    const sim::Galaxy& galaxy = world.galaxy();
    const sim::SurveySim& survey = world.survey();
    const std::vector<std::uint32_t>& route = survey.route();

    std::string routeSummary;
    if (route.size() >= 2) {
        routeSummary = std::to_string(route.size() - 1) + " jump(s): ";
        for (std::size_t i = 0; i < route.size(); ++i) {
            routeSummary += (i == 0 ? "" : " > ") + galaxy.systems[route[i]].name;
        }
    }
    panel.routeSummary = store(text, std::move(routeSummary));

    const std::uint32_t known = survey.knownSystemCount();
    panel.knownSummary =
        store(text, std::to_string(known) + " of " + std::to_string(galaxy.systems.size())
                        + " systems known - ledger "
                        + std::to_string(static_cast<int>(survey.ledgerValue())) + " cr");
    panel.currentSystem = world.currentSystemName();
    panel.currentIndex = static_cast<int>(world.currentSystemIndex());
    panel.systems = systemRows;
    panel.lanes = laneRows;
    panel.markers = markerRows;
}

} // namespace

void fillMapPanel(const SpaceWorld& world, std::deque<std::string>& text, ui::MapPanel& panel,
                  std::vector<ui::MapSystemRow>& systemRows, std::vector<ui::MapLaneRow>& laneRows,
                  std::vector<ui::MapMarkerRow>& markerRows)
{
    text.clear();
    systemRows.clear();
    laneRows.clear();
    markerRows.clear();

    const sim::Galaxy& galaxy = world.galaxy();
    const sim::SurveySim& survey = world.survey();
    const std::vector<std::uint32_t>& route = survey.route();

    // Trade overlay (Phase 8g): the best remembered price per system for the
    // selected commodity, and the range over the whole galaxy so each system
    // can be placed inside it. Cheapest and dearest are what make the map
    // readable — an absolute price means nothing without them.
    const int tradeCommodity = panel.tradeCommodity;
    std::vector<float> systemPrice(galaxy.systems.size(), 0.0f);
    std::vector<std::uint8_t> systemHasPrice(galaxy.systems.size(), 0);
    std::vector<std::uint8_t> systemStale(galaxy.systems.size(), 0);
    float cheapest = 0.0f;
    float dearest = 0.0f;
    std::uint32_t knownMarkets = 0;
    if (tradeCommodity >= 0) {
        const auto commodity = static_cast<std::uint32_t>(tradeCommodity);
        for (const sim::MarketMemory& memory : survey.marketMemory()) {
            if (memory.market >= world.economy().markets().size()
                || commodity >= memory.prices.size()) {
                continue;
            }
            const std::uint32_t system = world.economy().markets()[memory.market].systemIndex;
            const float price = memory.prices[commodity];
            ++knownMarkets;
            // A system can hold several markets; the map shows the best of
            // them, which is the one a trader would actually use.
            if (systemHasPrice[system] == 0 || price > systemPrice[system]) {
                systemPrice[system] = price;
                systemStale[system] =
                    survey.isStale(memory, world.worldSeconds()) ? 1u : 0u;
            }
            systemHasPrice[system] = 1;
            if (knownMarkets == 1 || price < cheapest) {
                cheapest = price;
            }
            if (knownMarkets == 1 || price > dearest) {
                dearest = price;
            }
        }
    }

    // Which system the tracked mission is pointing at (Phase 8i). An objective
    // in another system has no marker anywhere else, so without this the
    // galaxy map is silent about the one thing the player has been told to do.
    const sim::MissionObjective* objective = world.trackedObjective();
    const std::uint32_t objectiveSystem =
        objective != nullptr ? objective->system : sim::kAnySystem;

    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        const sim::SystemSpec& spec = galaxy.systems[i];
        const sim::KnowledgeState state = survey.knowledge(i);
        const bool visited = state >= sim::KnowledgeState::Visited;
        ui::MapSystemRow row;
        row.name = spec.name.c_str();
        row.position = {spec.mapPosition.x, spec.mapPosition.z};
        row.knowledge = toMapKnowledge(state);
        row.current = i == world.currentSystemIndex();
        row.onRoute = std::find(route.begin(), route.end(), i) != route.end();
        // A bookmark is the player's own knowledge, so it shows regardless of
        // how much of the system they have surveyed - they were standing there.
        row.bookmarkCount = survey.bookmarkCountIn(i);
        row.hasObjective = i == objectiveSystem;

        // Ownership is knowledge too: a system you have only heard of from a
        // gate does not tell you whose space it is.
        const std::uint32_t owner = world.systemOwnerFaction(i);
        if (visited && owner < world.factions().size()) {
            row.hasOwner = true;
            row.ownerColor = world.factions()[owner].color;
        }
        // Said the same way in both branches: being sent somewhere unsurveyed
        // is exactly the case where the player most needs to be told.
        const std::string objectiveNote = row.hasObjective ? " - MISSION OBJECTIVE" : "";
        if (!visited) {
            row.detail = store(
                text, spec.name + ": charted only - fly there to survey it" + objectiveNote);
        } else {
            std::uint32_t resolved = 0;
            const std::uint32_t signals = survey.signalCount(i);
            for (std::uint32_t s = 0; s < signals; ++s) {
                resolved += survey.signalResolved(i, s) ? 1u : 0u;
            }
            std::string detail = spec.name + ": " + regionName(spec.region);
            detail += row.hasOwner ? ", " + world.factions()[owner].name : ", unclaimed";
            detail += ", " + std::to_string(spec.stations.size()) + " station(s)";
            detail += ", sites " + std::to_string(resolved) + "/" + std::to_string(signals);
            if (state == sim::KnowledgeState::Surveyed) {
                detail += " - SURVEYED";
            }
            if (row.bookmarkCount > 0) {
                detail += ", " + std::to_string(row.bookmarkCount) + " bookmark(s)";
            }
            detail += objectiveNote;
            row.detail = store(text, std::move(detail));
        }
        if (tradeCommodity >= 0 && systemHasPrice[i] != 0) {
            row.hasTrade = true;
            row.tradePrice = systemPrice[i];
            row.tradeStale = systemStale[i] != 0;
            const float span = dearest - cheapest;
            row.tradeLevel = span > 1.0e-4f ? (systemPrice[i] - cheapest) / span : 0.5f;
        }
        systemRows.push_back(row);
    }
    if (tradeCommodity >= 0) {
        panel.tradeSummary = store(
            text, knownMarkets == 0
                      ? std::string("No price data yet - dock somewhere, or buy a market report")
                      : std::to_string(knownMarkets) + " markets known: "
                            + formatNumber(cheapest) + " - " + formatNumber(dearest) + " cr/unit");
    }

    for (const sim::GateLink& link : galaxy.links) {
        const bool onRoute = [&]() {
            for (std::size_t i = 0; i + 1 < route.size(); ++i) {
                if ((route[i] == link.a && route[i + 1] == link.b)
                    || (route[i] == link.b && route[i + 1] == link.a)) {
                    return true;
                }
            }
            return false;
        }();
        laneRows.push_back({.from = static_cast<int>(link.a),
                            .to = static_cast<int>(link.b),
                            .onRoute = onRoute});
    }

    // Which system the System tab is showing (Phase 8q). The screen keeps the
    // galaxy selection across frames and main.cpp hands it in exactly the way
    // it hands the trade commodity, so there is no second selection to keep in
    // step. -1, an out-of-range index, and a system the player has never heard
    // of all mean "wherever the player is", which is what the tab always did.
    const std::uint32_t currentSystem = world.currentSystemIndex();
    std::uint32_t viewSystem = currentSystem;
    if (panel.viewSystem >= 0
        && static_cast<std::size_t>(panel.viewSystem) < galaxy.systems.size()
        && survey.knowledge(static_cast<std::uint32_t>(panel.viewSystem))
               != sim::KnowledgeState::Unknown) {
        viewSystem = static_cast<std::uint32_t>(panel.viewSystem);
    }
    panel.viewIsCurrent = viewSystem == currentSystem;
    panel.viewSystemName = galaxy.systems[viewSystem].name.c_str();
    {
        // Said in words, because the one thing a remote map must never do is
        // imply it is as current as the local one.
        const sim::KnowledgeState rung = survey.knowledge(viewSystem);
        std::string summary = galaxy.systems[viewSystem].name;
        summary += " - ";
        summary += knowledgeName(rung);
        if (panel.viewIsCurrent) {
            summary += ", you are here";
        } else {
            const std::vector<std::uint32_t> hops =
                sim::routeBetween(galaxy, currentSystem, viewSystem);
            summary += hops.size() >= 2
                           ? ", " + std::to_string(hops.size() - 1) + " jump(s) away"
                           : ", no route known";
        }
        panel.viewSummary = store(text, std::move(summary));
    }

    // Somewhere else: the markers come from the galaxy plan and the two
    // content sims rather than from the live nav list, and nothing that means
    // "you are standing here" is filled in.
    if (!panel.viewIsCurrent) {
        fillRemoteMarkers(world, viewSystem, text, markerRows);
        // The hub is the primary planet's position, which is knowledge in its
        // own right: at Charted nothing is drawn in the playfield, so pinning
        // the bubble at the real hub would sketch where the inhabited part of
        // an unvisited system is. Zero until there is something there to pin.
        const core::DVec3 remoteHub =
            survey.knowledge(viewSystem) >= sim::KnowledgeState::Visited
                ? sim::playfieldHub(galaxy.systems[viewSystem])
                : core::DVec3{};
        panel.hubPosition = {static_cast<float>(remoteHub.x), static_cast<float>(remoteHub.z)};
        panel.shipPosition = {};
        panel.hasShip = false; // the ship is not there, so it is not drawn
        fillMapTail(world, text, panel, systemRows, laneRows, markerRows);
        return;
    }

    // System view: the nav targets, in the order the T key cycles them, so a
    // map selection and a target cycle land on exactly the same thing. This is
    // the local path and it stays exactly as it was: it knows things the
    // remote path cannot (live rock counts, wreck cargo, distance from the
    // ship, which slot is targeted), and giving those up to share one code
    // path would cost the current system information to buy the remote one a
    // consistency it does not need.
    const core::DVec3 hub = world.planets().empty()
                                ? core::DVec3{}
                                : world.planets()[std::min<std::size_t>(
                                      galaxy.systems[world.currentSystemIndex()].primaryPlanet,
                                      world.planets().size() - 1)]
                                      .position;
    const core::DVec3 shipPosition = world.shipState().position;
    const std::size_t targeted = world.currentTargetIndex();
    const std::span<const NavTarget> targets = world.navTargets();
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const SpaceWorld::NavKind kind = world.navTargetKind(i);
        // Two tiers, two origins. The star and the planets are measured from
        // the star, which is at the system origin, so the map reads the way a
        // system actually looks — star in the middle, worlds around it.
        // Everything else lives within a few hundred thousand km of the
        // primary planet, which at orbital scale is the same pixel, so it
        // keeps its offset from that planet and gets drawn in the expanded
        // bubble the map puts there.
        const bool orbital =
            kind == SpaceWorld::NavKind::Star || kind == SpaceWorld::NavKind::Planet;
        const core::DVec3 offset = orbital ? targets[i].position : targets[i].position - hub;
        const double distance = length(targets[i].position - shipPosition);
        bool scanned = false;
        if (kind == SpaceWorld::NavKind::Signal) {
            scanned = survey.signalResolved(world.currentSystemIndex(), world.navTargetSignal(i));
        } else if (kind == SpaceWorld::NavKind::Planet || kind == SpaceWorld::NavKind::Star) {
            scanned = survey.bodyScanned(world.currentSystemIndex(), world.navTargetBody(i));
        }
        std::string detail = formatDistance(distance);
        if (kind == SpaceWorld::NavKind::Signal || kind == SpaceWorld::NavKind::Planet
            || kind == SpaceWorld::NavKind::Star) {
            detail += scanned ? " - scanned" : " - unscanned";
        } else if (kind == SpaceWorld::NavKind::Field) {
            // What is still in the field, so a picked-over one is visibly not
            // worth the leg (Phase 8f).
            const std::uint32_t field = world.navTargetField(i);
            std::vector<sim::RockSpec> rocks;
            world.mining().rocksFor(galaxy, world.currentSystemIndex(), field, rocks);
            float left = 0.0f;
            std::uint32_t standing = 0;
            for (std::uint32_t r = 0; r < rocks.size(); ++r) {
                const float remaining =
                    world.mining().unitsLeft(world.currentSystemIndex(), field, r,
                                             rocks[r].yieldUnits);
                left += remaining;
                standing += remaining > 0.0f ? 1u : 0u;
            }
            detail += " - " + std::to_string(standing) + " rocks, "
                      + std::to_string(static_cast<int>(left)) + " units";
        } else if (kind == SpaceWorld::NavKind::Wreck) {
            const sim::WreckRecord* wreck = world.mining().wreck(world.navTargetWreck(i));
            float cargo = 0.0f;
            if (wreck != nullptr) {
                for (const sim::SignalCargo& stack : wreck->contents.cargo) {
                    cargo += stack.units;
                }
            }
            detail += " - " + std::to_string(static_cast<int>(cargo)) + " units aboard";
        } else if (kind == SpaceWorld::NavKind::Bookmark) {
            detail += " - bookmark";
        } else if (kind == SpaceWorld::NavKind::Objective) {
            // The marker's name is deliberately short, so the mission's own
            // wording rides in the detail. It goes *before* the distance
            // because the detail column is right-aligned and clips from the
            // left: the narrow row keeps the distance every other row shows,
            // and the wide footer cell reads out the whole sentence.
            if (objective != nullptr) {
                detail = objective->text + " - " + detail;
            }
        }
        markerRows.push_back(
            {.kind = toMarkerKind(kind),
             .name = targets[i].name.c_str(),
             .detail = store(text, std::move(detail)),
             .position = {static_cast<float>(offset.x), static_cast<float>(offset.z)},
             .distanceMeters = distance,
             .scanned = scanned,
             .targeted = i == targeted,
             .bookmarkId = kind == SpaceWorld::NavKind::Bookmark ? world.navTargetBookmark(i) : 0u,
             .inPlayfield = !orbital});
    }
    panel.hubPosition = {static_cast<float>(hub.x), static_cast<float>(hub.z)};
    const core::DVec3 shipOffset = shipPosition - hub;
    panel.shipPosition = {static_cast<float>(shipOffset.x), static_cast<float>(shipOffset.z)};
    panel.hasShip = true;

    fillMapTail(world, text, panel, systemRows, laneRows, markerRows);
}

bool executeMapAction(SpaceWorld& world, const ui::MapAction& action)
{
    using Kind = ui::MapAction::Kind;
    switch (action.kind) {
    case Kind::None:
    case Kind::Close:
    case Kind::SetTradeCommodity: // view state; main.cpp owns it
        break;
    case Kind::PlotRoute:
        if (action.index >= 0) {
            (void)world.plotRoute(static_cast<std::uint32_t>(action.index));
        }
        break;
    case Kind::ClearRoute:
        world.survey().clearRoute();
        break;
    case Kind::SelectMarker:
        if (action.index >= 0) {
            (void)world.selectTarget(static_cast<std::size_t>(action.index));
        }
        break;
    case Kind::Autopilot:
        if (action.index >= 0 && world.selectTarget(static_cast<std::size_t>(action.index))) {
            return world.engageAutopilot();
        }
        break;
    case Kind::DeleteBookmark:
        // The action carries the bookmark's own never-reused id rather than a
        // row (Phase 8q). It used to read the id back off the nav-target slot,
        // which is only answerable for the system the player is standing in -
        // and on a remote view that lookup would have deleted whichever
        // bookmark happened to occupy that slot back home.
        if (action.bookmarkId != 0) {
            (void)world.removeBookmark(action.bookmarkId);
        }
        break;
    }
    return false;
}

} // namespace game
