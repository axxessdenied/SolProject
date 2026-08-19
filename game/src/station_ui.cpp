#include "station_ui.hpp"

#include <algorithm>
#include <cstdio>

namespace game {

using namespace sol;

namespace {

constexpr const char* kSlotNames[assets::kModuleSlotCount] = {"shield", "engine", "cargo",
                                                              "utility"};

[[nodiscard]] int fittedCount(const OwnedShip& ship, const std::string& id)
{
    return static_cast<int>(std::count(ship.moduleIds.begin(), ship.moduleIds.end(), id));
}

[[nodiscard]] const char* store(std::deque<std::string>& text, std::string value)
{
    text.push_back(std::move(value));
    return text.back().c_str();
}

[[nodiscard]] std::string formatNumber(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

} // namespace

void fillStationOutfitting(const SpaceWorld& world, const assets::DefDatabase& defs,
                           std::deque<std::string>& text, ui::StationPanel& panel,
                           std::vector<ui::OutfitRow>& moduleRows,
                           std::vector<ui::OutfitRow>& weaponRows,
                           std::vector<ui::OutfitRow>& crewCatalogRows,
                           std::vector<ui::OutfitRow>& crewAboardRows,
                           std::vector<ui::OutfitRow>& shipRows,
                           std::vector<ui::FleetRow>& fleetRows,
                           std::vector<ui::FactionRow>& factionRows)
{
    text.clear();
    moduleRows.clear();
    weaponRows.clear();
    crewCatalogRows.clear();
    crewAboardRows.clear();
    shipRows.clear();
    fleetRows.clear();
    factionRows.clear();

    const OwnedShip& active = world.activeShip();
    const assets::ShipDef* base = defs.findShip(active.defId.c_str());

    // Fit summary: slot and power budget usage on the active ship.
    std::uint32_t slotsUsed[assets::kModuleSlotCount] = {};
    float powerUsed = 0.0f;
    for (const std::string& id : active.moduleIds) {
        if (const assets::ModuleDef* module = defs.findModule(id.c_str())) {
            ++slotsUsed[static_cast<std::size_t>(module->slot)];
            powerUsed += module->powerDraw;
        }
    }
    if (base != nullptr) {
        const std::uint32_t slotLimits[assets::kModuleSlotCount] = {
            base->slotsShield, base->slotsEngine, base->slotsCargo, base->slotsUtility};
        std::string summary = base->name + " | power " + formatNumber(powerUsed) + "/" +
                              formatNumber(base->powerOutput) + " | slots";
        for (std::size_t i = 0; i < assets::kModuleSlotCount; ++i) {
            summary += std::string(" ") + kSlotNames[i][0] + ":" +
                       std::to_string(slotsUsed[i]) + "/" + std::to_string(slotLimits[i]);
        }
        summary += " | berths " + std::to_string(active.crewIds.size()) + "/" +
                   std::to_string(base->crewBerths);
        panel.fitSummary = store(text, std::move(summary));
    } else {
        panel.fitSummary = store(text, "ship def '" + active.defId + "' missing");
    }
    panel.deductible = world.insuranceDeductible();

    for (const assets::ModuleDef& def : defs.modules()) {
        if (!world.stationSells(def.gate)) {
            continue; // owner faction doesn't stock it (Phase 8b catalogs)
        }
        moduleRows.push_back({.id = def.id.c_str(),
                              .name = def.name.c_str(),
                              .detail = store(text, std::string(kSlotNames[static_cast<std::size_t>(
                                                        def.slot)]) +
                                                        ", " + formatNumber(def.powerDraw) +
                                                        " pwr, " + formatNumber(def.mass) + " kg"),
                              .price = def.price,
                              .fitted = fittedCount(active, def.id)});
    }
    for (const assets::WeaponDef& def : defs.weapons()) {
        if (!world.stationSells(def.gate)) {
            continue;
        }
        weaponRows.push_back({.id = def.id.c_str(),
                              .name = def.name.c_str(),
                              .detail = store(text, def.kind + ", dmg " +
                                                        formatNumber(def.damage) + " @ " +
                                                        formatNumber(def.rateOfFire) + "/s"),
                              .price = def.price,
                              .fitted = active.weaponId == def.id ? 1 : 0});
    }
    for (const assets::CrewDef& def : defs.crew()) {
        if (!world.stationSells(def.gate)) {
            continue;
        }
        crewCatalogRows.push_back({.id = def.id.c_str(),
                                   .name = def.name.c_str(),
                                   .detail = def.role.c_str(),
                                   .price = def.price,
                                   .fitted = static_cast<int>(
                                       std::count(active.crewIds.begin(), active.crewIds.end(),
                                                  def.id))});
    }
    for (const std::string& id : active.crewIds) {
        const assets::CrewDef* def = defs.findCrew(id.c_str());
        crewAboardRows.push_back({.id = id.c_str(),
                                  .name = def != nullptr ? def->name.c_str() : id.c_str(),
                                  .detail = def != nullptr ? def->role.c_str() : "(missing def)",
                                  .price = 0.0f,
                                  .fitted = 1});
    }
    for (const assets::ShipDef& def : defs.ships()) {
        if (!world.stationSells(def.gate)) {
            continue;
        }
        shipRows.push_back(
            {.id = def.id.c_str(),
             .name = def.name.c_str(),
             .detail = store(text, "cargo " + formatNumber(def.cargoCapacity) + ", pwr " +
                                       formatNumber(def.powerOutput) + ", berths " +
                                       std::to_string(def.crewBerths)),
             .price = def.price,
             .fitted = 0});
    }
    for (std::size_t i = 0; i < world.fleet().size(); ++i) {
        const OwnedShip& ship = world.fleet()[i];
        const assets::ShipDef* def = defs.findShip(ship.defId.c_str());
        fleetRows.push_back(
            {.name = def != nullptr ? def->name.c_str() : ship.defId.c_str(),
             .active = i == world.activeShipIndex(),
             .storedHere = ship.storedSystem == world.currentSystemIndex() &&
                           ship.storedStation == world.dockedStationIndex(),
             .value = static_cast<float>(world.shipValue(ship))});
    }

    // Factions tab (Phase 8b): standings plus each faction's wars.
    const sol::sim::FactionSim& factionSim = world.factionSim();
    for (std::size_t i = 0; i < world.factions().size(); ++i) {
        const std::uint32_t faction = static_cast<std::uint32_t>(i);
        std::string detail = world.factions()[i].pirate ? "pirate clan" : "major";
        // ⚑ How much ground they hold goes BEFORE the war list (Phase 8u).
        // The war list is unbounded - a major at seed 1701 is at war with all
        // ten pirate clans - and it already overruns this column, so anything
        // appended after it is invisible. Short fixed-length facts first.
        std::uint32_t held = 0;
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            held += factionSim.systemOwner(s) == faction ? 1u : 0u;
        }
        detail += ", " + std::to_string(held) + " system(s)";
        std::string wars;
        for (std::size_t j = 0; j < world.factions().size(); ++j) {
            if (j != i && factionSim.atWar(faction, static_cast<std::uint32_t>(j))) {
                wars += wars.empty() ? "at war: " : ", ";
                wars += world.factions()[j].name;
            }
        }
        if (!wars.empty()) {
            detail += " - " + wars;
        }
        factionRows.push_back({.name = world.factions()[i].name.c_str(),
                               .detail = store(text, std::move(detail)),
                               .standing = factionSim.standing(faction),
                               .attitude = world.playerAttitudeName(faction)});
    }
    // War first, then raids (Phase 8u): a border that has moved is bigger news
    // than a market that got drained, and this tab is where a player who has
    // been away catches up. Both are current state rather than a history -
    // nothing stores when a system changed hands, and inventing a timestamp
    // store for one screen would be a worse trade than saying "now".
    std::string notes;
    const auto factionName = [&](std::uint32_t faction) -> std::string {
        return faction < world.factions().size() ? world.factions()[faction].name
                                                 : std::string("nobody");
    };
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        if (!factionSim.contested(s)) {
            continue;
        }
        if (!notes.empty()) {
            notes += "\n";
        }
        notes += world.galaxy().systems[s].name + " CONTESTED: " +
                 factionName(factionSim.contestOf(s).attacker) + " vs " +
                 factionName(factionSim.systemOwner(s)) + " (" +
                 formatNumber(factionSim.contestOf(s).pressure) + ")";
    }
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const std::uint32_t owner = factionSim.systemOwner(s);
        if (owner == factionSim.foundingClaim(s)) {
            continue;
        }
        if (!notes.empty()) {
            notes += "\n";
        }
        notes += world.galaxy().systems[s].name + " now held by " + factionName(owner) +
                 ", taken from " + factionName(factionSim.foundingClaim(s));
    }
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const float intensity = factionSim.raidIntensity(s);
        if (intensity < 0.05f) {
            continue;
        }
        if (!notes.empty()) {
            notes += "\n";
        }
        notes += world.galaxy().systems[s].name;
        const std::uint32_t raider = factionSim.lastRaider(s);
        if (raider < world.factions().size()) {
            notes += " raided by " + world.factions()[raider].name;
        }
        notes += " (" + formatNumber(intensity) + ")";
    }
    panel.factionNotes = store(text, std::move(notes));

    panel.modules = moduleRows;
    panel.weapons = weaponRows;
    panel.crewCatalog = crewCatalogRows;
    panel.crewAboard = crewAboardRows;
    panel.shipCatalog = shipRows;
    panel.fleet = fleetRows;
    panel.factions = factionRows;
}

namespace {

// Poster + current objective (+ progress/deadline) for a board or journal row.
[[nodiscard]] std::string missionDetail(const SpaceWorld& world,
                                        const sol::sim::Mission& mission)
{
    const sol::sim::MissionObjective& objective =
        mission.objectives[mission.currentObjective];
    std::string detail = mission.poster < world.factions().size()
                             ? world.factions()[mission.poster].name + ": "
                             : std::string();
    detail += objective.text;
    if (objective.kind == sol::sim::ObjectiveKind::Kill) {
        detail += " (" + std::to_string(objective.kills) + " left)";
    } else if (objective.kind == sol::sim::ObjectiveKind::Deliver) {
        detail += " (" + std::to_string(static_cast<int>(objective.units)) + " units)";
    } else if (objective.kind == sol::sim::ObjectiveKind::Hold) {
        // The contest meter, which is this objective's only progress (8u).
        const int percent = static_cast<int>(
            world.factionSim().contestOf(objective.system).pressure * 100.0f + 0.5f);
        detail += " (pressure " + std::to_string(percent) + "%)";
    } else if (objective.kind == sol::sim::ObjectiveKind::Escort) {
        // How far the hauler has got, which is this objective's progress and
        // is the record's to answer - the body may not even exist right now.
        //
        // ⚑ Kept to the same handful of characters the other three suffixes
        // take. The journal's detail column is narrower than the board's by
        // the width of its Track and Abandon buttons, and a drive caught
        // "(in the gate network)" cut off mid-word there while the same row
        // read fine on the board one screen earlier.
        const sol::sim::TraderRoute route = world.economy().route(objective.trader);
        const int percent = static_cast<int>(route.progress * 100.0f + 0.5f);
        detail += route.leg == sol::sim::TraderLeg::Jump
                      ? std::string(" (jumping)")
                      : " (leg " + std::to_string(percent) + "%)";
    }
    if (mission.objectives.size() > 1) {
        detail += " [" + std::to_string(mission.currentObjective + 1) + "/" +
                  std::to_string(mission.objectives.size()) + "]";
    }
    if (mission.deadline > 0.0) {
        detail += " - " + std::to_string(static_cast<int>(mission.deadline)) + "s";
    }
    if (mission.minRep > -100.0f) {
        detail += " - rep " + std::to_string(static_cast<int>(mission.minRep)) + "+";
    }
    return detail;
}

} // namespace

void fillStationMissions(const SpaceWorld& world, std::deque<std::string>& text,
                         sol::ui::StationPanel& panel,
                         std::vector<sol::ui::MissionRow>& offerRows,
                         std::vector<sol::ui::MissionRow>& journalRows)
{
    offerRows.clear();
    journalRows.clear();
    const sol::sim::MissionSim& missions = world.missionSim();
    for (const sol::sim::Mission& offer : missions.offers()) {
        const float standing = offer.poster < world.factions().size()
                                   ? world.factionSim().standing(offer.poster)
                                   : 0.0f;
        offerRows.push_back({.title = offer.title.c_str(),
                             .detail = store(text, missionDetail(world, offer)),
                             .reward = static_cast<float>(offer.rewardCredits),
                             .acceptable = standing >= offer.minRep,
                             .campaign = offer.campaign()});
    }
    for (std::size_t i = 0; i < missions.active().size(); ++i) {
        const sol::sim::Mission& mission = missions.active()[i];
        journalRows.push_back({.title = mission.title.c_str(),
                               .detail = store(text, missionDetail(world, mission)),
                               .reward = static_cast<float>(mission.rewardCredits),
                               .campaign = mission.campaign(),
                               .tracked = i == missions.tracked()});
    }
    panel.missionOffers = offerRows;
    panel.missionJournal = journalRows;
}

std::string formatAge(double seconds)
{
    char buffer[32];
    if (seconds < 60.0) {
        return "just now";
    }
    if (seconds < 3'600.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0fm ago", seconds / 60.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fh ago", seconds / 3'600.0);
    }
    return buffer;
}

void executeStationAction(SpaceWorld& world, const ui::StationAction& action)
{
    using Kind = ui::StationAction::Kind;
    switch (action.kind) {
    case Kind::None:
        break;
    case Kind::BuyModule:
        (void)world.buyModule(action.id);
        break;
    case Kind::SellModule:
        (void)world.sellModule(action.id);
        break;
    case Kind::BuyWeapon:
        (void)world.buyWeapon(action.id);
        break;
    case Kind::BuyShip:
        (void)world.buyShip(action.id);
        break;
    case Kind::SellShip:
        (void)world.sellShip(static_cast<std::size_t>(action.index));
        break;
    case Kind::SwitchShip:
        (void)world.switchShip(static_cast<std::size_t>(action.index));
        break;
    case Kind::HireCrew:
        (void)world.hireCrew(action.id);
        break;
    case Kind::FireCrew:
        (void)world.fireCrew(action.id);
        break;
    case Kind::AcceptMission:
        (void)world.acceptMission(static_cast<std::uint32_t>(action.index));
        break;
    case Kind::AbandonMission:
        (void)world.abandonMission(static_cast<std::uint32_t>(action.index));
        break;
    case Kind::TrackMission:
        world.missionSim().setTracked(static_cast<std::uint32_t>(action.index));
        break;
    case Kind::SellSurveyData:
        (void)world.sellSurveyData();
        break;
    case Kind::OrderRefine:
        (void)world.orderRefine(action.units);
        break;
    case Kind::CollectRefined:
        (void)world.collectRefined();
        break;
    case Kind::BuyMarketIntel:
        (void)world.buyMarketIntel();
        break;
    }
}

} // namespace game
