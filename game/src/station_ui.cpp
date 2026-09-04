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

// The two ends of a haul order, in the words the player picked them by
// (Phase 39 stage B). A station name alone is ambiguous across eighty systems,
// so both carry their system - the same shape `sol.captains` prints, because
// the screen and the console disagreeing about where somebody is would be the
// exact failure this stage's seam can produce.
[[nodiscard]] std::string haulEnds(const SpaceWorld& world, const game::CaptainOrder& order)
{
    const auto name = [&world](std::uint32_t market) -> std::string {
        if (market >= world.economy().markets().size()) {
            return "nowhere";
        }
        const sol::sim::StationMarket& row = world.economy().markets()[market];
        if (row.systemIndex >= world.galaxy().systems.size()) {
            return "nowhere";
        }
        const sol::sim::SystemSpec& spec = world.galaxy().systems[row.systemIndex];
        return row.stationIndex < spec.stations.size()
                   ? spec.stations[row.stationIndex].name + " (" + spec.name + ")"
                   : spec.name;
    };
    // ⚑ A MINING ORDER HAS ONE END AND SAYS SO, rather than printing
    // "<-> nowhere" off an unset `marketB`. Same function because it answers
    // the same question - where is this order pointed - and the heading is the
    // one place either answer is allowed to be as long as it needs to be.
    if (order.kind == game::OrderKind::Mine) {
        return "working the rock, selling at " + name(order.marketA);
    }
    // ⚑⚑⚑⚑ AND THE TWO COMBAT ORDERS HAVE ONE END AND NONE, WHICH IS THE
    // SENTENCE ABOVE ARRIVING A STAGE LATE (stage E, fixing stage D). The
    // clause above was written to stop a one-ended order printing
    // "<-> nowhere" - and then stage D added two more order kinds and every one
    // of them fell into the `return` below, so a patrol read
    // "Solaris Alpha (Solaris) <-> nowhere" and an escort, which names no
    // market at all and says so where it is created, read "nowhere <-> nowhere"
    // in the heading of its own section.
    //
    // This is stage D's own headline finding one layer out: a default that was
    // correct for the two cases that existed silently acquires every case added
    // afterwards. The guard against a sixth is that this switch is now
    // exhaustive on the kinds that name places, so an order kind added later
    // has to be answered here rather than defaulting into the haul's shape.
    if (order.kind == game::OrderKind::Patrol) {
        return "patrolling " + name(order.marketA);
    }
    if (order.kind == game::OrderKind::Escort) {
        return "flying your wing";
    }
    return name(order.marketA) + " <-> " + name(order.marketB);
}

void fillStationOutfitting(const SpaceWorld& world,
                           const assets::DefDatabase& defs,
                           std::deque<std::string>& text,
                           ui::StationPanel& panel,
                           std::vector<ui::MountRow>& mountRows,
                           std::vector<ui::OutfitRow>& componentRows,
                           std::vector<ui::OutfitRow>& blackMarketRows,
                           std::vector<ui::OutfitRow>& blackMarketShipRows,
                           std::vector<ui::OutfitRow>& weaponRows,
                           std::vector<ui::OutfitRow>& crewCatalogRows,
                           std::vector<ui::OutfitRow>& crewAboardRows,
                           std::vector<ui::OutfitRow>& shipRows,
                           std::vector<ui::FleetRow>& fleetRows,
                           std::vector<ui::CaptainRow>& captainRows,
                           std::vector<ui::CaptainRow>& captainHireRows,
                           std::vector<ui::CaptainRow>& haulRows,
                           std::vector<ui::CaptainRow>& fleetOptionRows,
                           std::vector<ui::FactionRow>& factionRows)
{
    text.clear();
    mountRows.clear();
    componentRows.clear();
    blackMarketRows.clear();
    blackMarketShipRows.clear();
    weaponRows.clear();
    crewCatalogRows.clear();
    crewAboardRows.clear();
    shipRows.clear();
    fleetRows.clear();
    captainRows.clear();
    captainHireRows.clear();
    haulRows.clear();
    fleetOptionRows.clear();
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
        //
        // ⚑⚑⚑⚑ AND IT NAMES WHERE YOU ARE, NOT ONLY WHERE THE DOOR IS
        // (Phase 37 stage E). Stage C wrote "Needs 25.0 with The Ninth Shift"
        // against a number NOTHING IN THE GAME COULD MOVE, so one figure was the
        // whole truth. This stage is the one that moves it - and a player who
        // has just paid twelve points of Solar Navy goodwill for twelve points
        // here would have come back to a row reading exactly what it read
        // before. *A threshold with no progress beside it is indistinguishable
        // from a threshold nobody is approaching*, which is the same failure as
        // the empty shelf two paragraphs up, one stage later. ⚑ It fits the
        // same cell: 32 characters against the 40 `station_fence_tests.cpp`
        // holds this line to, and the faction name it also requires is still in
        // it.
        if (!world.stationSellsAtFence(def.gate)) {
            const std::uint32_t fenceFaction = world.dockedFenceFaction();
            const char* who =
                fenceFaction < world.factions().size() ? world.factions()[fenceFaction].name.c_str() : "them";
            const float have = world.factionSim().standing(fenceFaction);
            row.lockedReason = store(text,
                                     formatNumber(have) + " / " + formatNumber(def.gate.minRep) + " with " +
                                         std::string(who));
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
    const auto ownedCount = [&world](const std::string& id) {
        int owned = 0;
        for (const OwnedShip& ship : world.fleet()) {
            owned += ship.defId == id ? 1 : 0;
        }
        return owned;
    };
    for (const assets::ShipDef& def : defs.ships()) {
        // Two shelves, the fence's checked first - `fillStationOutfitting`'s
        // component loop above says why at length, and a hull is the same rule.
        const bool fenced = world.stationFenceCarries(def.gate);
        if (!fenced && !world.stationSells(def.gate)) {
            continue;
        }
        (fenced ? blackMarketShipRows : shipRows)
            .push_back({.id = def.id.c_str(),
                        .name = def.name.c_str(),
                        .detail = store(text,
                                        "cargo " + formatNumber(def.cargoCapacity) + ", pwr " +
                                            formatNumber(def.powerOutput) + ", berths " +
                                            std::to_string(def.crewBerths)),
                        .price = def.price,
                        // ⚑⚑ HOW MANY OF THESE THE PLAYER ALREADY OWNS, AND IT WAS A
                        // HARDCODED 0 UNTIL PHASE 37 STAGE D. The Shipyard tab
                        // never drew this field so nothing was wrong; the black
                        // market's Hulls shelf DOES draw it, and a row that said
                        // "owned 0" the frame after a purchase was a lie the
                        // moment it had a reader. A field nobody displays is a
                        // field nobody checks.
                        .fitted = ownedCount(def.id)});
    }
    for (std::size_t i = 0; i < world.fleet().size(); ++i) {
        const OwnedShip& ship = world.fleet()[i];
        const assets::ShipDef* def = defs.findShip(ship.defId.c_str());
        const game::Captain* holder = world.captainOf(i);
        fleetRows.push_back({.name = def != nullptr ? def->name.c_str() : ship.defId.c_str(),
                             .active = i == world.activeShipIndex(),
                             .storedHere = ship.storedSystem == world.currentSystemIndex() &&
                                           ship.storedStation == world.dockedStationIndex(),
                             .value = static_cast<float>(world.shipValue(ship)),
                             .captain = holder != nullptr ? holder->name.c_str() : ""});
    }

    // Captains (Phase 39 stage A). The employed list first, then whoever is
    // standing in this dock's crew hall - which is empty at a dock with no
    // `crew` screen, and the tab says so rather than showing an empty box.
    //
    // ⚑⚑ THE DETAIL LINE NAMES THE HULL AND THE SYSTEM, not just "assigned".
    // A captain the player cannot find is the failure this stage can produce,
    // and the fleet list is on a different tab.
    //
    // ⚑⚑⚑⚑ GROUPED BY FLEET SINCE PHASE 40 STAGE A, WHICH IS WHY THE ROW
    // CARRIES ITS OWN INDEX. A commander is emitted with the people who answer
    // to them immediately underneath, so a three-role fleet reads as one thing
    // rather than as three unrelated rows spread through the roster. The order
    // of `m_captains` is untouched - this is a view, and the world goes on
    // being the flat list every test and `sol.captains()` read.
    std::vector<std::size_t> order;
    order.reserve(world.captains().size());
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < world.captains().size(); ++i) {
        // Subordinates are emitted by their commander, below. A captain whose
        // commander is not on the roster cannot happen - the loader refuses it
        // and `releaseSubordinatesOf` prevents it - but emitting by COMMANDER
        // rather than by subordinate means such a person would simply vanish
        // off the tab, so the fallback is to treat them as their own row.
        if (world.captainCommanderIndex(i) < world.captains().size()) {
            continue;
        }
        order.push_back(i);
        world.captainSubordinates(i, members);
        for (const std::size_t member : members) {
            order.push_back(member);
        }
    }
    for (const std::size_t i : order) {
        const game::Captain& captain = world.captains()[i];
        std::string detail = captain.trade + ", " + formatNumber(captain.cut * 100.0f) + "% of takings";
        if (captain.ship < world.fleet().size()) {
            const OwnedShip& held = world.fleet()[captain.ship];
            const assets::ShipDef* def = defs.findShip(held.defId.c_str());
            detail += " - flying ";
            detail += def != nullptr ? def->name : held.defId;
            // ⚑ WHERE THEY ARE, NOT WHERE THE HULL IS PARKED. `storedSystem` is
            // `kNoIndex` for the whole of a leg, so reading it here left a
            // captain on a route with no location at all - which is exactly the
            // failure the comment above this loop names. `captainSystem` answers
            // parked and flying alike, and only goes quiet between gates, where
            // "nowhere" is the honest answer rather than a gap.
            if (const std::uint32_t at = world.captainSystem(i); at < world.galaxy().systems.size()) {
                detail += " at " + world.galaxy().systems[at].name;
            } else {
                detail += " (between gates)";
            }
        } else {
            detail += " - no ship";
        }
        // ⚑⚑ THE NAME CARRIES THE INDENT AND THE DETAIL CARRIES THE RELATION,
        // and the split is deliberate: the indent is what makes the group
        // readable at a glance, and the words are what survive a screenshot
        // being described to somebody. ⚑ A commander is NOT labelled here -
        // "commands N" would be a fifth thing in a cell that has already
        // clipped five times this arc, and the rows underneath say it.
        const std::size_t boss = world.captainCommanderIndex(i);
        std::string name = captain.name;
        if (boss < world.captains().size()) {
            name = "- " + name;
            detail = "under " + world.captains()[boss].name + " - " + detail;
        }
        captainRows.push_back({.name = store(text, name),
                               .detail = store(text, detail),
                               .assigned = captain.ship < world.fleet().size(),
                               .selected = static_cast<int>(i) == panel.selectedCaptain,
                               .index = static_cast<int>(i)});
    }
    std::vector<game::CaptainCandidate> hall;
    world.captainCandidates(hall);
    for (const game::CaptainCandidate& who : hall) {
        captainHireRows.push_back(
            {.name = store(text, who.name),
             .detail = store(text, who.trade + ", " + formatNumber(who.cut * 100.0f) + "% of takings")});
    }

    // The standing order (Phase 39 stage B), for whoever the player has aimed
    // the tab at. ⚑⚑ EVERY ONE OF THESE THREE IS THE SAME PREDICATE THE WORLD
    // WOULD REFUSE ON, asked here so the section can say WHY rather than
    // offering a button that would silently do nothing - `firstFreeMountFor`'s
    // own bargain, which this file already states: duplicating the rule is how
    // the button and the transaction drift apart, so these ask the same fields
    // the refusals read.
    if (panel.selectedCaptain >= 0 &&
        static_cast<std::size_t>(panel.selectedCaptain) < world.captains().size()) {
        const auto index = static_cast<std::size_t>(panel.selectedCaptain);
        const game::Captain& captain = world.captains()[index];
        const bool hasShip = captain.ship < world.fleet().size();
        const bool parkedHere = hasShip &&
                                world.fleet()[captain.ship].storedSystem == world.currentSystemIndex() &&
                                world.fleet()[captain.ship].storedStation == world.dockedStationIndex();
        const bool ordered = captain.order.kind != game::OrderKind::None;
        // ⚑⚑ ONE PLACE, BECAUSE TWO READERS OF ONE RULE IS HOW THEY DRIFT. The
        // row says "holding out" and the strip's caption says it at length, and
        // they are the same fact about the same captain - this file's own
        // `firstFreeMountFor` bargain, which it states a few lines down about
        // the order buttons.
        const bool holdingOut = ordered && captain.order.kind == game::OrderKind::Haul &&
                                captain.order.floor > 0.0f && captain.haul.leg.cargo > 0.0f &&
                                captain.haul.outlay > 0.0;

        // ⚑⚑⚑ THE ROUTE GOES IN THE HEADING AND THE STATE STAYS IN THE ROW,
        // AND THE DRIVE IS WHAT SORTED THEM. Two station names with their
        // systems is a string of uncontrolled length - "Lyrioa Gamma (Lyrioa)
        // <-> Hammerfall Alpha (Hammerfall)" is 52 characters before anything
        // else is said - and the detail cell is a fixed ~600 px, so the first
        // photograph of this tab showed it clipped mid-word at "(Hamm". That is
        // the Bar tab's lesson restated (a person's name is not a topic): a
        // heading spans the width and a cell does not, so anything the CONTENT
        // sizes belongs in the heading. What is left in the row is bounded by
        // construction - a word, a percentage and two numbers.
        std::string status;
        if (!hasShip) {
            status = "no ship - give them one from the list above";
        } else if (!ordered) {
            status = parkedHere ? "parked here, no orders" : "parked elsewhere, no orders";
        } else if (captain.order.kind == game::OrderKind::Mine) {
            // ⚑ THE ROCKS, WHICH IS THE COUNT AND NOT THE STATE - stage A's
            // rule, on the screen this time. "At the rock" reads identically
            // whether the system is being ticked or was rebuilt around a
            // sleeping captain; a number that has gone up since the player
            // last looked cannot.
            // ⚑⚑⚑ A STALLED MINER IS ITS OWN STATE AND NOT A THIRD SHADE OF
            // "taking a load in" (stage E, ruling 18). A captain whose market
            // is full stands at the counter indefinitely, and for two hours of
            // measured play this row said "taking a load in" the whole time
            // while the ledger never moved again. The rock count was the only
            // tell and it reads as a number that has simply stopped - which is
            // what a captain between trips looks like too.
            status = captain.mine.stalledSeconds > 0.0                ? "hold full - nowhere to sell"
                     : captain.mine.phase == game::MinePhase::Selling ? "taking a load in"
                                                                      : "at the rock";
            // ⚑ THE GOOD'S NAME AND NOT ITS DEF ID, WHICH THE FLIGHT CAUGHT AND
            // NO TEST WOULD HAVE. The console prints `sol.ore_ferrous` because a
            // console is talking about defs; the Trade tab three tabs away says
            // "Ferrous Ore", and a screen that says both about the same cargo is
            // a screen that looks half-finished. `defs` is already in hand here.
            const std::string* oreId = captain.mine.commodity < world.commodityIds().size()
                                           ? &world.commodityIds()[captain.mine.commodity]
                                           : nullptr;
            const assets::CommodityDef* ore = oreId != nullptr ? defs.findCommodity(oreId->c_str()) : nullptr;
            status += ", " + formatNumber(captain.mine.units) + " " +
                      (ore != nullptr ? ore->name : std::string("ore")) + " aboard, " +
                      std::to_string(captain.mine.rockStep) + " rock(s)";
            if (captain.order.stopping) {
                status += ", standing down";
            }
        } else if (game::fighting(captain.order.kind)) {
            // ⚑⚑⚑⚑ THE ARM THE SCREEN WAS MISSING, AND THE PREDICATE FOR IT WAS
            // ALREADY WRITTEN (stage E, fixing stage D). This chain was
            // `!hasShip` / `!ordered` / `Mine` / else, and the `else` meant "a
            // haul" for exactly as long as Haul and Mine were the only orders.
            // Stage D added two, so a captain out on a beat reached
            // `captainRoute`, whose haul leg is empty, and the row read "at the
            // dock" while the hull was 40,000 km away crossing the system.
            //
            // The sharp half is that `game::fighting()` exists and the CONSOLE
            // asks it - `listCaptains` has had this arm since stage D. The
            // screen and the console were two readers of one question and only
            // one of them was taught the answer, which is this file's own
            // "the screen knew and the world did not" with the halves swapped.
            // ⚑ THE BEAT LEG IS THE COUNT, NOT THE STATE - the mining arm's own
            // rule, and it earns its place for the same reason: "on the beat"
            // reads identically whether the system is being ticked or was
            // rebuilt around a sleeping captain, and a number that has moved
            // since the player last looked cannot. `captainBeatLeg` is the
            // const probe that can see it, and unlike `captainPuppetInfo` it
            // looks across EVERY open bubble - which is the half that matters
            // here, because a patrol is posted in a system the player is not
            // standing in far more often than not.
            //
            // ⚑ An escort has no body while the player is docked, and this tab
            // is only ever drawn while they are - so the two orders get two
            // sentences rather than one with a hole in it.
            if (captain.order.kind == game::OrderKind::Escort) {
                status = "on your wing when you undock";
            } else {
                status = "on the beat, leg " + std::to_string(world.captainBeatLeg(index) + 1u);
            }
            if (captain.order.stopping) {
                status += ", standing down";
            }
        } else {
            const sol::sim::TraderRoute route = world.captainRoute(index);
            const char* word = route.leg == sol::sim::TraderLeg::Depart   ? "outbound"
                               : route.leg == sol::sim::TraderLeg::Arrive ? "inbound"
                               : route.leg == sol::sim::TraderLeg::Jump   ? "between gates"
                                                                          : "at the dock";
            status = word;
            if (route.leg != sol::sim::TraderLeg::None) {
                status += ", " + formatNumber(route.progress * 100.0f) + "%";
            }
            // ⚑⚑⚑⚑ THE ONE SENTENCE THAT STOPS THIS FEATURE READING AS A BUG
            // (stage E). A captain under a floor flies the route with a full
            // hold and banks nothing, indefinitely - which is exactly what a
            // broken captain looks like, and `captainThink` said so in the
            // comment it left against this stage. Two words in the row is the
            // whole difference between "it is deciding" and "it is stuck", and
            // it is bounded, so it belongs in the row rather than the heading.
            if (holdingOut) {
                status += ", holding out";
            }
            if (captain.order.stopping) {
                status += ", standing down";
            }
        }
        // The floor strip's inputs. ⚑ The live order's value, not the strip's:
        // a player who walks away and comes back sees what their captain is
        // actually holding out for, and the strip re-seats itself onto it.
        panel.captainOnHaul = ordered && captain.order.kind == game::OrderKind::Haul;
        panel.captainSellFloor = captain.order.floor;
        panel.captainHoldingOut = holdingOut;
        panel.captainRoute = ordered ? store(text, haulEnds(world, captain.order)) : "";
        // ⚑⚑⚑ AND WHAT THE ROUTE HAS ACTUALLY MADE, WHICH IS RULING 3's OWN
        // PROMISE MADE VISIBLE: "a bad route is visibly worse rather than
        // silently expensive". It is not decoration - the stage's measurement
        // found routes that lose money for a reason no estimate can see (the
        // far market moves while you fly), so the number a player needs is what
        // came back, not what was projected. Shown from the first haul, because
        // a captain who has run one leg and made nothing is exactly the case
        // this is for.
        //
        // ⚑⚑⚑⚑ AND IT IS ITS OWN ROW SINCE THE PHASE EXIT, BECAUSE A SENTENCE
        // THAT GROWS WITH ITS NUMBERS OUTGROWS ITS CELL. Appended to `status`
        // it read "at the rock, 34.7 Raw Ore aboard, 18 rock(s) - 17073 cr to
        // you, 38..." and clipped mid-number - the fifth cell-width bug of this
        // phase, and the only one to eat the figure the phase exists to show.
        // The four before it were fixed by shortening a label; this one cannot
        // be, because the length is the player's balance rather than a word.
        // ⚑⚑ It is drawn UNCONDITIONALLY, which fixes the second half: a patrol
        // earns nothing, so the old `!= 0.0` guard meant the screen printed no
        // money for it at all and could not answer "what has this one made".
        // Zero is an answer; silence is not.
        std::string earned;
        if (hasShip) {
            char money[96] = {};
            std::snprintf(money,
                          sizeof(money),
                          "%.0f cr to you, %.0f to them",
                          captain.ledger.earned,
                          captain.ledger.paid);
            earned = money;
            if (captain.ledger.losses > 0) {
                earned += ", " + std::to_string(captain.ledger.losses) + " lost";
            }
        }
        panel.captainEarned = store(text, earned);
        panel.captainStatus = store(text, status);
        panel.captainCanStandDown = ordered;
        panel.captainCanRecall = parkedHere && !ordered;

        // ⚑⚑ THE SAME THREE PREDICATES `orderMine` REFUSES ON, ASKED HERE SO
        // THE SECTION CAN SAY WHICH ONE. This is the section's own bargain
        // restated for a second order kind - and the note matters more here
        // than it did for a haul, because "no rock in this system" and "no
        // beam on that hull" are both things a player fixes by flying or by
        // buying, and neither is guessable from a greyed button.
        const bool rockHere = world.mining().fieldCount(world.currentSystemIndex()) > 0;
        const float beam = hasShip ? world.shipMiningPower(world.fleet()[captain.ship]) : 0.0f;
        panel.captainCanMine = hasShip && parkedHere && !ordered && rockHere && beam > 0.0f;
        panel.captainMineNote = !hasShip       ? "give them a hull first"
                                : !rockHere    ? "no asteroid field in this system"
                                : beam <= 0.0f ? "that hull carries no mining beam"
                                : !parkedHere  ? "their ship is not on this dock"
                                : ordered ? "cancel their orders first"
                                          : store(text, "cuts " + formatNumber(beam) + " units a second");

        // ⚑⚑ THE TWO COMBAT ORDERS, ON "WORK THIS SYSTEM"'S OWN SHAPE (stage
        // D): a row rather than a destination, because neither names a place
        // the player could pick out of a list. The note carries the refusal for
        // the reason the mining note does - "no guns on that hull" is a thing a
        // player fixes at the outfitter, and it is not guessable from a greyed
        // button.
        const float guns = hasShip ? world.shipGunPower(world.fleet()[captain.ship]) : 0.0f;
        panel.captainCanPatrol = hasShip && parkedHere && !ordered && guns > 0.0f;
        panel.captainPatrolNote =
            !hasShip       ? "give them a hull first"
            : guns <= 0.0f ? "that hull carries no guns"
            : !parkedHere  ? "their ship is not on this dock"
            : ordered      ? "cancel their orders first"
                           : store(text, "holds the dock and the gates, " + formatNumber(guns) + " dps");
        // ⚑ AND THE ESCORT HAS ONE REFUSAL THE OTHERS DO NOT: THERE CAN ONLY BE
        // ONE. That is ruling 4's fence - a second hull holding station on the
        // same point is a FLEET, which is Phase 40 - and it is said here rather
        // than only in `orderEscort`, because a player who has one escort and
        // presses the button on a second captain is owed the reason.
        std::size_t escorts = 0;
        for (const game::Captain& other : world.captains()) {
            if (game::escorting(other.order.kind)) {
                ++escorts;
            }
        }
        const bool escortTaken = escorts > 0 && !game::escorting(captain.order.kind);
        panel.captainCanEscort = hasShip && parkedHere && !ordered && guns > 0.0f && !escortTaken;
        panel.captainEscortNote =
            !hasShip       ? "give them a hull first"
            : guns <= 0.0f ? "that hull carries no guns"
            : escortTaken  ? "somebody already flies as your escort"
            : !parkedHere  ? "their ship is not on this dock"
            : ordered      ? "cancel their orders first"
                           : store(text, "flies your wing wherever you go, " + formatNumber(guns) + " dps");

        // ⚑⚑⚑⚑ THE FLEET ORDER (Phase 40 stage B), AND IT IS THE ONE ROW ON
        // THIS TAB WHOSE RULE THE SCREEN DOES NOT RE-DERIVE. Every note above
        // is three or four fields of one captain read back here, which is
        // cheap and checkable; this one turns on the fits of every hull in a
        // fleet and on where the player has seen a price, and a second copy of
        // that would be a second answer. `fleetWorkPlan` is the world's own
        // resolution asked as a QUESTION - const, silent, and the same function
        // `orderFleetWork` issues from - so the button and the order cannot
        // drift, which is this file's standing bargain paid properly for once.
        std::vector<SpaceWorld::FleetAssignment> plan;
        std::string fleetWhy;
        const bool canOrderFleet = world.fleetWorkPlan(index, plan, &fleetWhy);
        // How much of this fleet is already at work, which is what decides
        // whether the second button does anything. ⚑ Asked of the COMMANDER
        // only: a subordinate is refused above by name and told whose door to
        // knock on, and offering them a Stand here would be a second answer to
        // the question the plan just answered.
        std::vector<std::size_t> fleetCrew;
        world.captainSubordinates(index, fleetCrew);
        std::size_t fleetWorking = 0;
        if (!fleetCrew.empty() && world.captainCommanderIndex(index) >= world.captains().size()) {
            fleetCrew.push_back(index);
            for (const std::size_t member : fleetCrew) {
                fleetWorking += world.captains()[member].order.kind != game::OrderKind::None ? 1u : 0u;
            }
        }
        panel.captainCanOrderFleet = canOrderFleet;
        panel.captainCanStandFleetDown = fleetWorking > 0;
        if (canOrderFleet) {
            // What the fits come to, counted off the world's own plan rather
            // than recomputed - so the sentence under the button is the order
            // the button gives, to the ship.
            std::size_t mining = 0;
            std::size_t guarding = 0;
            std::size_t hauling = 0;
            for (const SpaceWorld::FleetAssignment& job : plan) {
                mining += job.kind == game::OrderKind::Mine ? 1u : 0u;
                guarding += job.kind == game::OrderKind::Patrol ? 1u : 0u;
                hauling += job.kind == game::OrderKind::Haul ? 1u : 0u;
            }
            panel.captainFleetNote =
                store(text,
                      "mining " + std::to_string(mining) + ", guarding " + std::to_string(guarding) +
                          ", hauling " + std::to_string(hauling));
        } else if (fleetWorking > 0) {
            // ⚑ THE ONE REFUSAL WORTH REPHRASING. The plan says "'Tarek'
            // already has orders", which is true and reads as a fault; a fleet
            // that is out working is the feature succeeding, and the player
            // wants the number and the way back rather than a name.
            panel.captainFleetNote = store(text, std::to_string(fleetWorking) + " of this fleet at work");
        } else {
            panel.captainFleetNote = store(text, fleetWhy);
        }

        // ⚑ The destination list is offered only when an order would actually
        // be taken. `orderHaul` refuses a hull that is not on this dock and one
        // already on a run; both are read above rather than discovered by the
        // player pressing a row and watching nothing happen.
        if (hasShip && parkedHere && !ordered) {
            std::vector<SpaceWorld::HaulDestination> places;
            world.haulDestinations(places);
            for (const SpaceWorld::HaulDestination& place : places) {
                haulRows.push_back({.name = store(text, place.station),
                                    .detail = store(text,
                                                    place.system + ", " + std::to_string(place.hops) +
                                                        " jump" + (place.hops == 1 ? "" : "s"))});
            }
        }
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

    // ⚑⚑⚑⚑ THE FLEET SECTION (Phase 40 stage A). Three questions, one list,
    // and which one is being answered is decided HERE rather than on the
    // screen - see `StationPanel::fleetOptions`. Every branch below asks the
    // same fields `setCaptainCommander` refuses on, which is this file's
    // standing bargain: a screen that re-derives a rule is a screen that
    // eventually disagrees with the world about it, and the Crew tab has
    // already been the half that was right once this arc.
    if (panel.selectedCaptain >= 0 &&
        static_cast<std::size_t>(panel.selectedCaptain) < world.captains().size()) {
        const auto self = static_cast<std::size_t>(panel.selectedCaptain);
        const std::size_t boss = world.captainCommanderIndex(self);
        std::vector<std::size_t> mine;
        world.captainSubordinates(self, mine);
        const auto rowFor = [&](std::size_t who, std::string detail) {
            const game::Captain& person = world.captains()[who];
            fleetOptionRows.push_back({.name = person.name.c_str(),
                                       .detail = store(text, std::move(detail)),
                                       .index = static_cast<int>(who)});
        };
        if (boss < world.captains().size()) {
            // ⚑⚑⚑⚑ THE ROW IS THE CAPTAIN BEING RELEASED, NOT THE COMMANDER
            // THEY ARE LEAVING - and the first cut had it the other way, which
            // would have sent `LeaveFleet` the boss's index and released
            // nobody. The invariant that catches it is worth stating: A FLEET
            // ROW ALWAYS DRAWS THE CAPTAIN ITS INDEX NAMES. "Under" points at a
            // commander and the action carries the selection; "Leave" and
            // "Free" point at the person leaving. A row whose name said one
            // person while its index meant another is the exact class of defect
            // the roster's own `index` field exists to prevent, and it very
            // nearly arrived through the door that was built to stop it.
            panel.fleetVerb = "Leave";
            rowFor(self, std::string("answers to ") + world.captains()[boss].name);
        } else if (!mine.empty()) {
            panel.fleetVerb = "Free";
            for (const std::size_t member : mine) {
                rowFor(member, std::string("answers to ") + world.captains()[self].name);
            }
        } else {
            panel.fleetVerb = "Under";
            for (std::size_t i = 0; i < world.captains().size(); ++i) {
                // The one-level rule, asked the way the world asks it: somebody
                // who already answers to another captain cannot take people on.
                if (i == self || world.captains()[i].commander != game::kNoCommander) {
                    continue;
                }
                std::vector<std::size_t> theirs;
                world.captainSubordinates(i, theirs);
                rowFor(i,
                       theirs.empty() ? std::string("commands nobody yet")
                                      : std::to_string(theirs.size()) + " under them");
            }
            if (fleetOptionRows.empty()) {
                panel.fleetNote = "(nobody else here could take them on)";
            }
        }
    } else {
        panel.fleetNote = "(select a captain above)";
    }

    panel.mounts = mountRows;
    panel.components = componentRows;
    panel.blackMarketCatalog = blackMarketRows;
    panel.blackMarketShips = blackMarketShipRows;
    // Whose back room, for the heading that is the whole point of the tab.
    const std::uint32_t fenceFaction = world.dockedFenceFaction();
    panel.fenceOperator =
        fenceFaction < world.factions().size() ? world.factions()[fenceFaction].name.c_str() : "";
    panel.weapons = weaponRows;
    panel.crewCatalog = crewCatalogRows;
    panel.crewAboard = crewAboardRows;
    panel.shipCatalog = shipRows;
    panel.fleet = fleetRows;
    panel.captains = captainRows;
    panel.captainHires = captainHireRows;
    panel.haulDestinations = haulRows;
    panel.fleetOptions = fleetOptionRows;
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

void executeStationAction(SpaceWorld& world, const ui::StationAction& action, int& selectedCaptain)
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
    case Kind::SelectCaptain:
        // Clicking the selected row again clears it, which is `mountList`'s
        // rule and the only way to un-aim the Give buttons.
        selectedCaptain = selectedCaptain == action.index ? -1 : action.index;
        break;
    case Kind::HireCaptain:
        (void)world.hireCaptain(static_cast<std::size_t>(action.index));
        break;
    case Kind::DismissCaptain:
        (void)world.dismissCaptain(static_cast<std::size_t>(action.index));
        // ⚑ The list just shrank, so any selection into it names somebody
        // else. Cleared rather than fixed up: a Give aimed at whoever slid into
        // the slot is a ship handed to the wrong person.
        selectedCaptain = -1;
        break;
    case Kind::AssignCaptain:
        if (selectedCaptain >= 0) {
            // ⚑ THE SELECTION SURVIVES THE GIVE SINCE STAGE B, and it used to
            // be cleared here. Handing somebody a hull is now the FIRST half of
            // a two-step - the second is the route - and dropping the selection
            // in between meant re-finding the same person in the list to finish
            // the job you were plainly in the middle of.
            (void)world.assignCaptain(static_cast<std::size_t>(selectedCaptain),
                                      static_cast<std::size_t>(action.index));
        }
        break;
    case Kind::RecallCaptain:
        (void)world.recallCaptain(static_cast<std::size_t>(action.index));
        break;
    case Kind::SetCommander:
        // ⚑ THE ROW IS THE COMMANDER AND THE SELECTION IS THE SUBORDINATE,
        // which is `AssignCaptain`'s argument order pointed at a person instead
        // of a hull. The selection survives, for that action's reason: forming
        // a fleet is one of several things the player is doing to the captain
        // they are holding, and dropping the aim in the middle means finding
        // them in the list again.
        if (selectedCaptain >= 0) {
            (void)world.setCaptainCommander(static_cast<std::size_t>(selectedCaptain),
                                            static_cast<std::size_t>(action.index));
        }
        break;
    case Kind::LeaveFleet:
        // Carries its own subject, because "Leave" on a commander's row and
        // "Free" on a member's row are the same act said from the two ends.
        (void)world.clearCaptainCommander(static_cast<std::size_t>(action.index));
        break;
    case Kind::OrderHaul:
        // The row index becomes a market HERE and nowhere else, which is the
        // same bargain `AssignCaptain` strikes with a fleet slot: the screen
        // names rows and one place turns a row into a world id. Rebuilt rather
        // than remembered, because the list is rebuilt every frame.
        if (selectedCaptain >= 0) {
            std::vector<SpaceWorld::HaulDestination> places;
            world.haulDestinations(places);
            if (static_cast<std::size_t>(action.index) < places.size()) {
                // ⚑ THE STRIP'S VALUE RIDES IN ON THE ACTION (stage E), so the
                // floor the player set before pressing Haul is the floor the
                // run starts with, rather than a second click they have to
                // remember after the captain has already bought a load.
                (void)world.orderHaul(static_cast<std::size_t>(selectedCaptain),
                                      places[static_cast<std::size_t>(action.index)].market,
                                      action.units);
            }
        }
        break;
    case Kind::SetSellFloor:
        // ⚑ Re-aims a run already in flight, which is the only action here that
        // changes an order instead of giving or ending one. It refuses on any
        // other order kind in the world, so a strip left on screen by a stale
        // frame cannot put a floor on a patrol.
        if (selectedCaptain >= 0) {
            (void)world.setSellFloor(static_cast<std::size_t>(selectedCaptain), action.units);
        }
        break;
    case Kind::OrderMine:
        // No row to resolve: the order names this system and this dock, so the
        // selection is the whole of it.
        if (selectedCaptain >= 0) {
            (void)world.orderMine(static_cast<std::size_t>(selectedCaptain));
        }
        break;
    case Kind::OrderPatrol:
        if (selectedCaptain >= 0) {
            (void)world.orderPatrol(static_cast<std::size_t>(selectedCaptain));
        }
        break;
    case Kind::OrderEscort:
        if (selectedCaptain >= 0) {
            (void)world.orderEscort(static_cast<std::size_t>(selectedCaptain));
        }
        break;
    case Kind::CancelOrder:
        (void)world.cancelOrder(static_cast<std::size_t>(action.index));
        break;
    case Kind::OrderFleet:
        // No row to resolve and no ship to name: the order is said to the
        // commander and the fleet's fits decide the rest, which is
        // `OrderMine`'s "the selection is the whole of it" one level up.
        if (selectedCaptain >= 0) {
            (void)world.orderFleetWork(static_cast<std::size_t>(selectedCaptain));
        }
        break;
    case Kind::StandFleetDown:
        if (selectedCaptain >= 0) {
            (void)world.standFleetDown(static_cast<std::size_t>(selectedCaptain));
        }
        break;
    }
}

} // namespace game
