#include "map_ui.hpp"

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
    }
    return ui::MapMarkerRow::Kind::Station;
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

        // Ownership is knowledge too: a system you have only heard of from a
        // gate does not tell you whose space it is.
        const std::uint32_t owner = world.systemOwnerFaction(i);
        if (visited && owner < world.factions().size()) {
            row.hasOwner = true;
            row.ownerColor = world.factions()[owner].color;
        }
        if (!visited) {
            row.detail = store(text, spec.name + ": charted only - fly there to survey it");
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

    // System view: the nav targets, in the order the T key cycles them, so a
    // map selection and a target cycle land on exactly the same thing.
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
        }
        markerRows.push_back({.kind = toMarkerKind(kind),
                              .name = targets[i].name.c_str(),
                              .detail = store(text, std::move(detail)),
                              .position = {static_cast<float>(offset.x),
                                           static_cast<float>(offset.z)},
                              .distanceMeters = distance,
                              .scanned = scanned,
                              .targeted = i == targeted,
                              .inPlayfield = !orbital});
    }
    panel.hubPosition = {static_cast<float>(hub.x), static_cast<float>(hub.z)};
    const core::DVec3 shipOffset = shipPosition - hub;
    panel.shipPosition = {static_cast<float>(shipOffset.x), static_cast<float>(shipOffset.z)};
    panel.hasShip = true;

    std::string routeSummary;
    if (route.size() >= 2) {
        routeSummary = std::to_string(route.size() - 1) + " jump(s): ";
        for (std::size_t i = 0; i < route.size(); ++i) {
            routeSummary += (i == 0 ? "" : " > ") + galaxy.systems[route[i]].name;
        }
    }
    panel.routeSummary = store(text, std::move(routeSummary));

    std::uint32_t known = survey.knownSystemCount();
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
    }
    return false;
}

} // namespace game
