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
    std::string raids;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const float intensity = factionSim.raidIntensity(s);
        if (intensity < 0.05f) {
            continue;
        }
        if (!raids.empty()) {
            raids += "\n";
        }
        raids += world.galaxy().systems[s].name;
        const std::uint32_t raider = factionSim.lastRaider(s);
        if (raider < world.factions().size()) {
            raids += " raided by " + world.factions()[raider].name;
        }
        raids += " (" + formatNumber(intensity) + ")";
    }
    panel.factionNotes = store(text, std::move(raids));

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
    }
}

} // namespace game
