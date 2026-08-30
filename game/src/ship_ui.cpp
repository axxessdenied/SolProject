#include "ship_ui.hpp"

#include "sol/core/math/vec.hpp"

#include <cstdio>

namespace game {

using sol::assets::DefDatabase;
namespace ui = sol::ui;

namespace {

constexpr const char* kSlotNames[sol::assets::kComponentSlotCount] = {"shield", "engine", "cargo", "utility"};

[[nodiscard]] const char* store(std::deque<std::string>& text, std::string value)
{
    text.push_back(std::move(value));
    return text.back().c_str();
}

[[nodiscard]] std::string number(double value, int decimals = 1)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

[[nodiscard]] std::string withUnit(double value, const char* unit, int decimals = 1)
{
    return number(value, decimals) + " " + unit;
}

// Distances span metres to hundreds of thousands of kilometres, so the scan
// and collector ranges get the same treatment the HUD gives them.
[[nodiscard]] std::string range(double meters)
{
    if (meters < 10'000.0) {
        return number(meters, 0) + " m";
    }
    if (meters < 1.0e9) {
        return number(meters / 1000.0, 0) + " km";
    }
    return number(meters / 1.0e9, 2) + " Mkm";
}

} // namespace

void fillShipInfoPanel(const SpaceWorld& world,
                       const DefDatabase& defs,
                       std::deque<std::string>& text,
                       ui::ShipInfoPanel& panel,
                       std::vector<ui::InfoRow>& flightRows,
                       std::vector<ui::InfoRow>& defenceRows,
                       std::vector<ui::InfoRow>& utilityRows,
                       std::vector<ui::InfoRow>& fittedRows,
                       std::vector<ui::InfoRow>& cargoRows)
{
    text.clear();
    flightRows.clear();
    defenceRows.clear();
    utilityRows.clear();
    fittedRows.clear();
    cargoRows.clear();

    const OwnedShip& active = world.activeShip();
    const sol::assets::ShipDef* base = defs.findShip(active.defId.c_str());
    const sol::sim::ShipTuning& tuning = world.shipTuning();
    const ShipDefense& defense = world.playerDefense();
    const sol::sim::PowerTuning& power = world.powerTuning();
    const sol::sim::PowerState& pips = world.playerPower();

    panel.shipName = base != nullptr ? base->name.c_str() : active.defId.c_str();
    // There is no hull-class field on a ShipDef, so the subtitle carries what
    // the player actually wants to know about the hull: what it cost new and
    // how much the whole ship is worth as it stands.
    panel.shipClass = store(text,
                            base != nullptr ? "hull " + number(base->price, 0) + " cr, ship value " +
                                                  number(world.shipValue(active), 0) + " cr"
                                            : std::string("ship def missing"));
    panel.hull = defense.tuning.hull > 0.0f ? defense.state.hull / defense.tuning.hull : 0.0f;
    panel.shieldFore = defense.tuning.shieldStrength > 0.0f
                           ? defense.state.shieldFore / defense.tuning.shieldStrength
                           : 0.0f;
    panel.shieldAft =
        defense.tuning.shieldStrength > 0.0f ? defense.state.shieldAft / defense.tuning.shieldStrength : 0.0f;
    panel.credits = world.playerCredits();
    panel.cargoUsed = world.playerCargoTotal();
    panel.cargoCapacity = world.playerCargoCapacity();
    panel.pipsWeapons = pips.pips.weapons;
    panel.pipsEngines = pips.pips.engines;
    panel.pipsShields = pips.pips.shields;
    panel.pipMax = power.maxPerSystem;

    // Power and slot budget, in the same shape the Outfitting tab prints it so
    // the two screens are recognisably describing one ship.
    std::uint32_t slotsUsed[sol::assets::kComponentSlotCount] = {};
    float powerUsed = 0.0f;
    float componentMass = 0.0f;
    for (const std::string& id : active.componentIds) {
        if (const sol::assets::ComponentDef* component = defs.findComponent(id.c_str())) {
            ++slotsUsed[static_cast<std::size_t>(component->slot)];
            powerUsed += component->powerDraw;
            componentMass += component->mass;
        }
    }
    if (base != nullptr) {
        const std::uint32_t limits[sol::assets::kComponentSlotCount] = {
            base->slotsShield, base->slotsEngine, base->slotsCargo, base->slotsUtility};
        std::string summary = "power " + number(powerUsed) + "/" + number(base->powerOutput) + " | slots";
        for (std::size_t i = 0; i < sol::assets::kComponentSlotCount; ++i) {
            summary += std::string(" ") + kSlotNames[i][0] + ":" + std::to_string(slotsUsed[i]) + "/" +
                       std::to_string(limits[i]);
        }
        summary +=
            " | berths " + std::to_string(active.crewIds.size()) + "/" + std::to_string(base->crewBerths);
        panel.fitSummary = store(text, std::move(summary));
    } else {
        panel.fitSummary = store(text, "ship def '" + active.defId + "' missing");
    }

    // --- Flight. The pip note on the engine lines is the point of showing
    // these in flight at all: they are what ENG is currently doing.
    const float engineScale =
        power.engineAtZero + (power.engineAtMax - power.engineAtZero) *
                                 (power.maxPerSystem > 0 ? static_cast<float>(pips.pips.engines) /
                                                               static_cast<float>(power.maxPerSystem)
                                                         : 0.0f);
    flightRows.push_back({"Main drive",
                          store(text, withUnit(tuning.forwardAccel, "m/s2")),
                          store(text, "x" + number(engineScale, 2) + " from ENG pips")});
    flightRows.push_back({"Reverse", store(text, withUnit(tuning.reverseAccel, "m/s2"))});
    flightRows.push_back({"Lateral", store(text, withUnit(tuning.lateralAccel, "m/s2"))});
    flightRows.push_back({"Vertical", store(text, withUnit(tuning.verticalAccel, "m/s2"))});
    flightRows.push_back({"Assist cap",
                          store(text, withUnit(tuning.maxSpeed, "m/s", 0)),
                          store(text, "x" + number(engineScale, 2) + " from ENG pips")});
    flightRows.push_back({"Turn (P/Y/R)",
                          store(text,
                                number(tuning.maxTurnRate.x, 2) + " / " + number(tuning.maxTurnRate.y, 2) +
                                    " / " + number(tuning.maxTurnRate.z, 2)),
                          "rad/s"});
    if (base != nullptr) {
        flightRows.push_back(
            {"Mass",
             store(text, withUnit(base->mass + componentMass, "kg", 0)),
             store(text, "hull " + number(base->mass, 0) + " + fit " + number(componentMass, 0))});
    }

    // --- Defence.
    const float shieldScale =
        power.shieldRegenAtZero + (power.shieldRegenAtMax - power.shieldRegenAtZero) *
                                      (power.maxPerSystem > 0 ? static_cast<float>(pips.pips.shields) /
                                                                    static_cast<float>(power.maxPerSystem)
                                                              : 0.0f);
    defenceRows.push_back({"Shield per facing",
                           store(text,
                                 number(defense.state.shieldFore, 0) + " / " +
                                     number(defense.tuning.shieldStrength, 0) + " fore"),
                           store(text, number(defense.state.shieldAft, 0) + " aft")});
    defenceRows.push_back({"Shield regen",
                           store(text, withUnit(defense.tuning.shieldRegenRate, "hp/s")),
                           store(text,
                                 "x" + number(shieldScale, 2) + " from SYS pips, " +
                                     number(defense.tuning.shieldRegenDelay, 1) + " s delay")});
    defenceRows.push_back({"Armor", store(text, number(defense.tuning.armor, 0)), "ablative"});
    defenceRows.push_back(
        {"Hull", store(text, number(defense.state.hull, 0) + " / " + number(defense.tuning.hull, 0))});

    const float weaponScale =
        power.weaponRechargeAtZero + (power.weaponRechargeAtMax - power.weaponRechargeAtZero) *
                                         (power.maxPerSystem > 0 ? static_cast<float>(pips.pips.weapons) /
                                                                       static_cast<float>(power.maxPerSystem)
                                                                 : 0.0f);
    defenceRows.push_back(
        {"Capacitor",
         store(text, number(world.playerPower().weaponCharge, 0) + " / " + number(power.weaponCapacitor, 0)),
         store(text,
               withUnit(power.weaponRechargeRate, "u/s") + ", x" + number(weaponScale, 2) +
                   " from WEP pips")});

    // --- Utility.
    utilityRows.push_back(
        {"Cargo hold",
         store(text,
               number(world.playerCargoTotal()) + " / " + number(world.playerCargoCapacity()) + " units")});
    utilityRows.push_back({"Scanner range", store(text, range(world.scanRange()))});
    utilityRows.push_back({"Collector range", store(text, range(world.collectorRange()))});

    // --- Fit. Weapon first, because it is the one hardpoint.
    if (const sol::assets::WeaponDef* weapon = defs.findWeapon(active.weaponId.c_str())) {
        std::string detail = weapon->kind + ", " + number(weapon->damage, 0) + " dmg @ " +
                             number(weapon->rateOfFire, 1) + "/s, " + range(weapon->range);
        if (weapon->miningPower > 0.0f) {
            detail += ", mining " + number(weapon->miningPower, 1);
        }
        fittedRows.push_back({"Weapon", weapon->name.c_str(), store(text, std::move(detail))});
    } else {
        fittedRows.push_back({"Weapon", "none", "unarmed mount"});
    }
    for (const std::string& id : active.componentIds) {
        if (const sol::assets::ComponentDef* component = defs.findComponent(id.c_str())) {
            fittedRows.push_back(
                {store(text, std::string(kSlotNames[static_cast<std::size_t>(component->slot)])),
                 component->name.c_str(),
                 store(text,
                       number(component->powerDraw, 1) + " pwr, " + number(component->mass, 0) + " kg")});
        }
    }
    for (const std::string& id : active.crewIds) {
        if (const sol::assets::CrewDef* crew = defs.findCrew(id.c_str())) {
            fittedRows.push_back({"Crew", crew->name.c_str(), crew->role.c_str()});
        }
    }

    // --- Cargo manifest. Only what is actually aboard: a list of zeroes is
    // not a manifest.
    const std::vector<std::string>& commodities = world.commodityIds();
    for (std::uint32_t i = 0; i < commodities.size(); ++i) {
        const float units = world.playerCargo(i);
        if (units <= 0.0f) {
            continue;
        }
        const sol::assets::CommodityDef* def = defs.findCommodity(commodities[i].c_str());
        cargoRows.push_back({def != nullptr ? def->name.c_str() : commodities[i].c_str(),
                             store(text, withUnit(units, "units"))});
    }

    panel.flight = flightRows;
    panel.defence = defenceRows;
    panel.utility = utilityRows;
    panel.fitted = fittedRows;
    panel.cargo = cargoRows;
}

std::string shipInfoReport(const SpaceWorld& world, const DefDatabase& defs)
{
    std::deque<std::string> text;
    sol::ui::ShipInfoPanel panel;
    std::vector<ui::InfoRow> flightRows;
    std::vector<ui::InfoRow> defenceRows;
    std::vector<ui::InfoRow> utilityRows;
    std::vector<ui::InfoRow> fittedRows;
    std::vector<ui::InfoRow> cargoRows;
    fillShipInfoPanel(world, defs, text, panel, flightRows, defenceRows, utilityRows, fittedRows, cargoRows);

    std::string out = std::string(panel.shipName) + " (" + panel.shipClass + ")\n" + panel.fitSummary + "\n";
    const auto section = [&out](const char* title, const std::vector<ui::InfoRow>& rows) {
        out += std::string("-- ") + title + "\n";
        for (const ui::InfoRow& row : rows) {
            out += std::string(row.label) + ": " + row.value;
            if (row.detail[0] != '\0') {
                out += std::string(" (") + row.detail + ")";
            }
            out += "\n";
        }
    };
    section("flight", flightRows);
    section("defence", defenceRows);
    section("utility", utilityRows);
    section("fit", fittedRows);
    section("cargo", cargoRows);
    char buffer[96];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "credits %.0f, pips %d/%d/%d of %d",
                  panel.credits,
                  panel.pipsWeapons,
                  panel.pipsEngines,
                  panel.pipsShields,
                  panel.pipMax);
    out += buffer;
    return out;
}

} // namespace game
