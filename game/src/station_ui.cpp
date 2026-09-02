#include "station_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace game {

using namespace sol;

namespace {

[[nodiscard]] int fittedCount(const OwnedShip& ship, const std::string& id)
{
    return static_cast<int>(std::count_if(
        ship.fittings.begin(), ship.fittings.end(), [&](const ShipFitting& f) { return f.defId == id; }));
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

void fillStationOutfitting(const SpaceWorld& world,
                           const assets::DefDatabase& defs,
                           std::deque<std::string>& text,
                           ui::StationPanel& panel,
                           std::vector<ui::MountRow>& mountRows,
                           std::vector<ui::OutfitRow>& componentRows,
                           std::vector<ui::OutfitRow>& blackMarketRows,
                           std::vector<ui::OutfitRow>& weaponRows,
                           std::vector<ui::OutfitRow>& crewCatalogRows,
                           std::vector<ui::OutfitRow>& crewAboardRows,
                           std::vector<ui::OutfitRow>& shipRows,
                           std::vector<ui::FleetRow>& fleetRows,
                           std::vector<ui::FactionRow>& factionRows)
{
    text.clear();
    mountRows.clear();
    componentRows.clear();
    blackMarketRows.clear();
    weaponRows.clear();
    crewCatalogRows.clear();
    crewAboardRows.clear();
    shipRows.clear();
    fleetRows.clear();
    factionRows.clear();

    const OwnedShip& active = world.activeShip();
    const assets::ShipDef* base = defs.findShip(active.defId.c_str());

    // ⚑ WHERE A FIT LANDS, AND A SELECTION IS A FILTER RATHER THAN A HINT.
    // With a mount selected, a row that mount does not accept gets NO target
    // and the screen greys its button; it does not quietly land somewhere
    // else. The alternative - fall back to auto-placement - was tried and
    // rejected on the drive: it makes the greying unreachable while any mount
    // anywhere is free, so the screen never once says "not here", and a player
    // who aimed at the drive and got a shield in the shield mount has been
    // answered by a screen that ignored them.
    //
    // With nothing selected the catalog behaves exactly as it always did.
    const assets::ShipMount* selected =
        base != nullptr && panel.selectedMount != nullptr ? base->findMount(panel.selectedMount) : nullptr;
    const bool filtering = selected != nullptr;
    const auto targetFor =
        [&](const std::string& id, assets::MountKind kind, assets::MountSize size) -> std::string {
        if (filtering) {
            return assets::mountAccepts(*selected, kind, size) ? selected->id : std::string();
        }
        return world.firstFreeMountFor(id.c_str());
    };

    // Fit summary: mounts filled and power budget used on the active ship.
    // ⚑ "mounts 3/8" replaced the four s:/e:/c:/u: fractions, and it says
    // strictly less on purpose - WHICH mounts are free is the list below's
    // job now, and repeating it in a header line was a summary that had to be
    // re-read against the list to be believed.
    float powerUsed = 0.0f;
    for (const ShipFitting& fitting : active.fittings) {
        if (const assets::ComponentDef* component = defs.findComponent(fitting.defId.c_str())) {
            powerUsed += component->powerDraw;
        }
    }
    if (base != nullptr) {
        std::string summary =
            base->name + " | power " + formatNumber(powerUsed) + "/" + formatNumber(base->powerOutput) +
            " | mounts " + std::to_string(active.fittings.size()) + "/" + std::to_string(base->mounts.size());
        summary +=
            " | berths " + std::to_string(active.crewIds.size()) + "/" + std::to_string(base->crewBerths);
        panel.fitSummary = store(text, std::move(summary));
    } else {
        panel.fitSummary = store(text, "ship def '" + active.defId + "' missing");
    }
    panel.deductible = world.insuranceDeductible();

    // The hull's mounts, in AUTHORED order - the order the def file reads and
    // the order the Forge's list will show, so the screen, the file and the
    // tool all describe the ship the same way round.
    if (base != nullptr) {
        for (const assets::ShipMount& mount : base->mounts) {
            ui::MountRow row{.id = mount.id.c_str(),
                             .kind = assets::mountKindName(mount.kind),
                             .size = assets::mountSizeName(mount.size),
                             .external = mount.external};
            const ShipFitting* fitted = active.fittingAt(mount.id);
            if (fitted == nullptr) {
                // An empty mount says what it WOULD take. A row that only said
                // "empty" makes the player carry the accept rule in their head
                // and check it against a catalog one section down.
                row.detail = store(text,
                                   std::string("empty - takes a ") + assets::mountSizeName(mount.size) + " " +
                                       assets::mountKindName(mount.kind) + " fitting or smaller");
            } else if (const assets::ComponentDef* component = defs.findComponent(fitted->defId.c_str())) {
                row.fitted = component->name.c_str();
                row.detail = store(text,
                                   formatNumber(component->powerDraw) + " pwr, " +
                                       formatNumber(component->mass) + " kg");
                row.resale = static_cast<float>(SpaceWorld::kResaleRate) * component->price;
            } else if (const assets::WeaponDef* weapon = defs.findWeapon(fitted->defId.c_str())) {
                row.fitted = weapon->name.c_str();
                std::string detail = weapon->kind + ", dmg " + formatNumber(weapon->damage) + " @ " +
                                     formatNumber(weapon->rateOfFire) + "/s";
                if (mount.arc > 0.0f) {
                    detail += ", traverse " + formatNumber(mount.arc) + " deg";
                }
                row.detail = store(text, std::move(detail));
                row.resale = static_cast<float>(SpaceWorld::kResaleRate) * weapon->price;
            } else {
                // The def is gone from under a save. Named, not hidden: the
                // mount is occupied and the player has to be told by what.
                row.fitted = fitted->defId.c_str();
                row.detail = "def missing - remove to free the mount";
            }
            mountRows.push_back(row);
        }
    }

    for (const assets::ComponentDef& def : defs.components()) {
        // ⚑⚑⚑ TWO SHELVES SINCE PHASE 37 STAGE C, AND THE FENCE'S IS CHECKED
        // FIRST. `stationSells` is the OR over both counters, so an item the
        // black market carries would appear on the lawful list too the moment
        // the player's standing with them cleared its gate - the shop window
        // bug that function's own comment was written against, arriving from
        // the other direction. The fence's line is never on the lawful shelf.
        const bool fence = world.stationFenceCarries(def.gate);
        if (!fence && !world.stationSells(def.gate)) {
            continue; // owner faction doesn't stock it (Phase 8b catalogs)
        }
        sol::ui::OutfitRow row{
            .id = def.id.c_str(),
            .name = def.name.c_str(),
            .detail =
                store(text,
                      std::string(assets::mountSizeName(def.size)) + " " + assets::mountKindName(def.mount) +
                          ", " + formatNumber(def.powerDraw) + " pwr, " + formatNumber(def.mass) + " kg"),
            .price = def.price,
            .fitted = fittedCount(active, def.id),
            .targetMount = store(text, targetFor(def.id, def.mount, def.size))};
        if (!fence) {
            componentRows.push_back(row);
            continue;
        }
        // ⚑⚑ THE ROW STAYS AND NAMES ITS PRICE. Every other catalog in this
        // file answers a gate by leaving the row off; this one keeps it and
        // writes what it would take, because the fence's whole stock today is
        // one thing nobody can buy and an empty shelf says the feature is
        // broken rather than that the door is locked.
        if (!world.stationSellsAtFence(def.gate)) {
            const std::uint32_t fenceFaction = world.dockedFenceFaction();
            const char* who =
                fenceFaction < world.factions().size() ? world.factions()[fenceFaction].name.c_str() : "them";
            row.lockedReason =
                store(text, "Needs " + formatNumber(def.gate.minRep) + " with " + std::string(who));
            row.targetMount = "";
        }
        blackMarketRows.push_back(row);
    }
    for (const assets::WeaponDef& def : defs.weapons()) {
        if (!world.stationSells(def.gate)) {
            continue;
        }
        weaponRows.push_back(
            {.id = def.id.c_str(),
             .name = def.name.c_str(),
             .detail =
                 store(text,
                       std::string(assets::mountSizeName(def.size)) + " " + assets::mountKindName(def.mount) +
                           ", dmg " + formatNumber(def.damage) + " @ " + formatNumber(def.rateOfFire) + "/s"),
             .price = def.price,
             .fitted = fittedCount(active, def.id),
             .targetMount = store(text, targetFor(def.id, def.mount, def.size))});
    }
    for (const assets::CrewDef& def : defs.crew()) {
        if (!world.stationSells(def.gate)) {
            continue;
        }
        crewCatalogRows.push_back(
            {.id = def.id.c_str(),
             .name = def.name.c_str(),
             .detail = def.role.c_str(),
             .price = def.price,
             .fitted = static_cast<int>(std::count(active.crewIds.begin(), active.crewIds.end(), def.id))});
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
        shipRows.push_back({.id = def.id.c_str(),
                            .name = def.name.c_str(),
                            .detail = store(text,
                                            "cargo " + formatNumber(def.cargoCapacity) + ", pwr " +
                                                formatNumber(def.powerOutput) + ", berths " +
                                                std::to_string(def.crewBerths)),
                            .price = def.price,
                            .fitted = 0});
    }
    for (std::size_t i = 0; i < world.fleet().size(); ++i) {
        const OwnedShip& ship = world.fleet()[i];
        const assets::ShipDef* def = defs.findShip(ship.defId.c_str());
        fleetRows.push_back({.name = def != nullptr ? def->name.c_str() : ship.defId.c_str(),
                             .active = i == world.activeShipIndex(),
                             .storedHere = ship.storedSystem == world.currentSystemIndex() &&
                                           ship.storedStation == world.dockedStationIndex(),
                             .value = static_cast<float>(world.shipValue(ship))});
    }

    // Factions tab (Phase 8b): standings plus each faction's wars.
    const sol::sim::FactionSim& factionSim = world.factionSim();
    for (std::size_t i = 0; i < world.factions().size(); ++i) {
        const std::uint32_t faction = static_cast<std::uint32_t>(i);
        // ⚑ The word comes from `factionKindLabel` since Phase 37 stage B rather
        // than from a ternary here, because there are three answers now and the
        // console's `sol.factions` prints the same three.
        std::string detail = game::factionKindLabel(world.factions()[i]);
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
        return faction < world.factions().size() ? world.factions()[faction].name : std::string("nobody");
    };
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        if (!factionSim.contested(s)) {
            continue;
        }
        if (!notes.empty()) {
            notes += "\n";
        }
        notes += world.galaxy().systems[s].name +
                 " CONTESTED: " + factionName(factionSim.contestOf(s).attacker) + " vs " +
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
        notes += world.galaxy().systems[s].name + " now held by " + factionName(owner) + ", taken from " +
                 factionName(factionSim.foundingClaim(s));
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

    panel.mounts = mountRows;
    panel.components = componentRows;
    panel.blackMarketCatalog = blackMarketRows;
    // Whose back room, for the heading that is the whole point of the tab.
    const std::uint32_t fenceFaction = world.dockedFenceFaction();
    panel.fenceOperator =
        fenceFaction < world.factions().size() ? world.factions()[fenceFaction].name.c_str() : "";
    panel.weapons = weaponRows;
    panel.crewCatalog = crewCatalogRows;
    panel.crewAboard = crewAboardRows;
    panel.shipCatalog = shipRows;
    panel.fleet = fleetRows;
    panel.factions = factionRows;
}

namespace {

// Poster + current objective (+ progress/deadline) for a board or journal row.
[[nodiscard]] std::string missionDetail(const SpaceWorld& world, const sol::sim::Mission& mission)
{
    const sol::sim::MissionObjective& objective = mission.objectives[mission.currentObjective];
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
        const int percent =
            static_cast<int>(world.factionSim().contestOf(objective.system).pressure * 100.0f + 0.5f);
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
        detail += route.leg == sol::sim::TraderLeg::Jump ? std::string(" (jumping)")
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

void fillStationMissions(const SpaceWorld& world,
                         std::deque<std::string>& text,
                         sol::ui::StationPanel& panel,
                         std::vector<sol::ui::MissionRow>& offerRows,
                         std::vector<sol::ui::MissionRow>& journalRows)
{
    offerRows.clear();
    journalRows.clear();
    const sol::sim::MissionSim& missions = world.missionSim();
    for (const sol::sim::Mission& offer : missions.offers()) {
        const float standing =
            offer.poster < world.factions().size() ? world.factionSim().standing(offer.poster) : 0.0f;
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

// ⚑⚑⚑ THE ROOM IS THE RECREATION MODULE WITH THE LARGEST `power_draw`, AND
// THAT IS DELIBERATELY NOT A NEW FIELD. The family already ships a size ladder -
// bar 2, restaurant 3, concourse 4, casino 5, resort 8 - which is authored, is
// already load-bearing for the composer's power rule, and orders exactly the way
// a room list should. A second number saying the same thing in a different unit
// is how two tables start disagreeing, and this project has paid for that twice.
const assets::ModuleDef* stationRoom(const SpaceWorld& world,
                                     const assets::DefDatabase& defs,
                                     std::uint32_t system,
                                     std::uint32_t station)
{
    const assets::ModuleDef* best = nullptr;
    for (const std::uint32_t index : world.stationModules(system, station)) {
        if (index >= defs.modules().size()) {
            continue;
        }
        const assets::ModuleDef& def = defs.modules()[index];
        if (def.family != assets::ModuleFamily::Recreation) {
            continue;
        }
        if (best == nullptr || def.powerDraw > best->powerDraw) {
            best = &def;
        }
    }
    return best;
}

int roomTalkLines(const assets::ModuleDef& room)
{
    // ⚑ One constant, and the ladder is read from the authored number rather
    // than from a table repeating it. bar 2 and restaurant 3 give one line,
    // concourse 4 and casino 5 give two, resort 8 gives three.
    return std::clamp(1 + static_cast<int>(room.powerDraw) / 4, 1, 4);
}

void composeRoomLine(const SpaceWorld& world,
                     const assets::DefDatabase& defs,
                     std::uint32_t system,
                     std::uint32_t station,
                     std::vector<BarLine>& out)
{
    const assets::ModuleDef* room = stationRoom(world, defs, system, station);
    if (room == nullptr) {
        return;
    }
    // What else is in here is a fact about the dock's size, and it is the only
    // line that is about the station rather than about the galaxy around it.
    std::string line = room->name;
    int others = 0;
    for (const std::uint32_t index : world.stationModules(system, station)) {
        if (index < defs.modules().size() &&
            defs.modules()[index].family == assets::ModuleFamily::Recreation &&
            &defs.modules()[index] != room) {
            ++others;
        }
    }
    if (others == 1) {
        line += ", and one other room on this dock";
    } else if (others > 1) {
        line += ", and " + std::to_string(others) + " other rooms on this dock";
    } else {
        line += ", and it is the only room on this dock";
    }
    out.push_back({.topic = "The room", .text = std::move(line)});
}

void composeHouseTalk(const SpaceWorld& world,
                      const assets::DefDatabase& defs,
                      std::uint32_t system,
                      std::uint32_t station,
                      std::vector<BarLine>& out)
{
    // --- Whose law reaches here, and how far. The SIGN is who polices the
    // place and the magnitude is how much, which is the distinction
    // `systemSecurity` exists to keep - so the sentence is built from both.
    {
        const float security = world.systemSecurity(system);
        const std::uint32_t owner = world.systemOwnerFaction(system);
        const char* held = owner < world.factions().size() ? world.factions()[owner].name.c_str() : nullptr;
        char number[32] = {};
        std::snprintf(number, sizeof(number), "%+.2f", static_cast<double>(security));
        std::string line;
        if (security > 0.0f && held != nullptr) {
            line = std::string(held) + " polices this system (" + number + ").";
        } else if (security < 0.0f && held != nullptr) {
            line = std::string(held) + " runs this system (" + number + "), and there is no other law here.";
        } else {
            line = "Nobody's law reaches this system. Whatever happens out there stays out there.";
        }
        out.push_back({.topic = "The law", .text = std::move(line)});
    }

    // --- What the house cannot take (Phase 34 stage D). A REFUSAL rather than
    // an empty shelf: the station has no hold for it, so the trade board leaves
    // the row off entirely and a player who hauled it here has no way at all to
    // find out why. This is that answer.
    {
        std::string refused;
        int count = 0;
        for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
            if (world.stationStocks(system, station, c)) {
                continue;
            }
            // ⚑⚑⚑ A LAWFUL DOCK DOES NOT ADVERTISE CONTRABAND BY NAME (Phase 37
            // stage A). Every illicit good is unstockable at all 117 docks with
            // no shadow module, so without this the warehouse line at nearly
            // every station in the galaxy would read "no hold here for Combat
            // Stims, Stripped Components" - the Freight Guild's own counter
            // volunteering the black market's catalogue to a stranger.
            //
            // ⚑⚑ IT IS NOT A LIE AND THAT IS WHY IT IS SAFE: the line answers
            // "why did my cargo not appear on the board", and a player cannot
            // be holding an illicit good they have nowhere to have bought yet.
            // The eight docks that CAN take it are exactly the eight that do
            // not reach this branch for those goods, so nothing that is
            // tradeable anywhere is hidden from the place it trades.
            if (world.commodityClass(c) == assets::GoodsClass::Illicit) {
                continue;
            }
            ++count;
            const assets::CommodityDef* def = defs.findCommodity(world.commodityIds()[c].c_str());
            const std::string name = def != nullptr ? def->name : world.commodityIds()[c];
            if (count <= 3) {
                refused += (refused.empty() ? "" : ", ") + name;
            }
        }
        std::string line;
        if (count == 0) {
            line = "This dock has a hold for everything anybody hauls.";
        } else {
            line = "No hold here for " + refused;
            if (count > 3) {
                line += " and " + std::to_string(count - 3) + " more";
            }
            line += " - do not bring it, they cannot take it.";
        }
        out.push_back({.topic = "The warehouse", .text = std::move(line)});
    }

    // --- What it runs on (Phase 34 stage B). The composer FITS the smallest
    // plant that covers the draw rather than rolling one, so the plant is a
    // consequence of the station and reads as one: an outpost got an array and
    // a shipyard got a fusion plant, and nobody authored either.
    {
        const assets::ModuleDef* plant = nullptr;
        for (const std::uint32_t index : world.stationModules(system, station)) {
            if (index >= defs.modules().size()) {
                continue;
            }
            const assets::ModuleDef& def = defs.modules()[index];
            if (def.family == assets::ModuleFamily::Power &&
                (plant == nullptr || def.powerOutput > plant->powerOutput)) {
                plant = &def;
            }
        }
        if (plant != nullptr) {
            out.push_back(
                {.topic = "The lights", .text = "Everything in here runs off the " + plant->name + "."});
        }
    }

    // --- And who else is trading on this dock (Phase 34 stage E). DERIVED, not
    // stored: `stationHasShadowPresence` compares the operator against the LIVE
    // holder, so the day a clan takes this system its own fence stops being a
    // shadow presence and this line stops being said. Nothing has to notice.
    if (world.stationHasShadowPresence(system, station)) {
        const std::uint32_t operator_ = world.stationShadowOwner(system, station);
        if (operator_ < world.factions().size()) {
            out.push_back({.topic = "Word is",
                           .text = world.factions()[operator_].name +
                                   " keep a back room on this dock, and the people who "
                                   "hold this system are not asking about it."});
        }
    }
}

std::string composeKeeperLine(const char* name, const char* trade, std::uint32_t visits)
{
    if (name == nullptr || name[0] == 0) {
        return {};
    }
    std::string line = name;
    if (trade != nullptr && trade[0] != 0) {
        line += " - ";
        line += trade;
    }
    // ⚑ THREE STATES AND NOT A COUNTER, because "your fourth visit" is a number
    // a person would never say and a save file obviously would. The save holds
    // the count; the room holds an impression of it.
    if (visits == 1) {
        line += " (you have met)";
    } else if (visits > 1) {
        line += " (a familiar face)";
    }
    return line;
}

void fillStationBar(std::span<const BarLine> talk,
                    const char* room,
                    const char* keeper,
                    std::deque<std::string>& text,
                    ui::StationPanel& panel,
                    std::vector<ui::InfoRow>& talkRows)
{
    talkRows.clear();
    panel.barRoom = room != nullptr ? room : "";
    panel.barKeeper = keeper != nullptr ? keeper : "";
    panel.barTalk = {};
    char action[16] = {};
    for (const BarLine& line : talk) {
        // ⚑ THE ACTION IS THE LEAD'S INDEX AS A STRING, WHICH IS `InfoRow`'s OWN
        // CONTRACT AND NOT A SHORTCUT: "an opaque id the screen writes back when
        // it is clicked ... what a row's action means is the filler's business".
        // The Bar tab never parses it; `stationAction` does, on the way back.
        if (line.lead >= 0) {
            std::snprintf(action, sizeof(action), "%d", line.lead);
        }
        talkRows.push_back({.label = store(text, line.topic),
                            .value = store(text, line.text),
                            .detail = "",
                            .button = line.lead >= 0 ? "Take it" : "",
                            .action = line.lead >= 0 ? store(text, action) : ""});
    }
    panel.barTalk = talkRows;
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
    case Kind::BuyFitting:
        (void)world.buyFitting(action.id, action.mount);
        break;
    case Kind::SellFitting:
        (void)world.sellFitting(action.mount);
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
    case Kind::AcceptLead:
        // The row handed its action back untouched (Phase 35 stage D); this is
        // the filler's half of `InfoRow`'s bargain, where the string means
        // something again.
        (void)world.acceptLead(static_cast<std::uint32_t>(std::atoi(action.id)));
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
