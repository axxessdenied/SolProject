#pragma once

#include "sol/core/math/vec.hpp"

#include <cstdint>
#include <span>

namespace sol::ui {

// The data contracts between the game and its screens (engine plan Phase 8d).
// The game fills one of these per frame from the world, a screen in
// `game/src/*_screen.cpp` draws it, and what the player did comes back in the
// `action` fields for the game to execute - the fill-then-execute seam that
// survived the move off the provisional Dear ImGui screens.

// Everything the flight HUD draws.
struct FlightHud
{
    bool active = false;
    float speedMetersPerSecond = 0.0f;
    bool assist = true;
    bool boost = false;
    bool cruise = false;
    bool autopilot = false;
    const char* cameraMode = "";
    const char* targetName = "";
    double targetDistanceMeters = 0.0;
    float closingSpeedMetersPerSecond = 0.0f;
    core::Vec3 targetDirectionCamera; // unit, camera space (-Z forward)
    float tanHalfFovY = 1.0f;

    // Power pips (decisions/003); pipMax caps each bar, charge is 0..1.
    int pipsWeapons = 2;
    int pipsEngines = 2;
    int pipsShields = 2;
    int pipMax = 4;
    float weaponCharge = 1.0f;

    // Defenses (decisions/002), all 0..1: fore/aft shield arcs around the
    // crosshair plus hull in the flight panel.
    float shieldFore = 1.0f;
    float shieldAft = 1.0f;
    float hull = 1.0f;

    // Combat feedback: targeted-ship readout, projectile lead marker, and a
    // crosshair flash while the player is taking hits.
    bool targetIsShip = false;
    float targetShieldFore = 0.0f;
    float targetShieldAft = 0.0f;
    float targetHull = 0.0f;
    bool hasLead = false;
    core::Vec3 leadDirectionCamera; // unit, camera space
    float damageFlash = 0.0f;       // 0..1

    // Target allegiance (Phase 8b): faction display name and the player's
    // attitude tag ("hostile"/"neutral"/"friendly"); empty for unaffiliated.
    const char* targetFaction = "";
    const char* targetAttitude = "";

    // Tracked mission (Phase 8c): title + current objective, empty = none.
    const char* missionTitle = "";
    const char* missionObjective = "";
    double missionDeadline = 0.0; // seconds left; 0 = no deadline

    // Universe context (Phase 7): current system, and jump/dock prompts
    // while the ship is within activation range.
    const char* systemName = "";
    bool gateInRange = false;
    bool dockInRange = false;
    bool docked = false;
    const char* dockedStationName = "";

    // Scanning (Phase 8e). The pulse recharges, a target scan runs while the
    // contact is held in the reticle, and salvage shares the prompt slot with
    // jump and dock.
    float pulseCharge = 1.0f;    // 0..1; 1 = ready to fire
    float scanProgress = 0.0f;   // 0..1 of the scan in flight
    const char* scanTarget = ""; // what that scan is resolving; empty = none
    int contactsUnresolved = 0;  // found by a pulse, not yet identified
    int sitesOpen = 0;           // identified and still holding something
    bool salvageInRange = false;
    const char* routeNextHop = ""; // next system on the plotted route
};

// One commodity line on the station's Trade tab. The game fills rows from the
// docked market; a clicked button reports back through `action`.
struct TradeRow
{
    const char* name = "";
    float price = 0.0f; // credits/unit right now
    float stock = 0.0f; // station stock, units
    float cargo = 0.0f; // in the player's hold, units
};

struct TradeAction
{
    int row = -1; // index into rows; -1 = no click this frame
    float units = 0.0f;
    bool isBuy = false;
};

struct TradePanel
{
    const char* stationName = "";
    double credits = 0.0;
    float cargoUsed = 0.0f;
    float cargoCapacity = 0.0f;
    std::span<const TradeRow> rows;
    TradeAction action; // out
};

// Catalog/inventory row for the outfitting, shipyard, and crew tabs (Phase 8a).
struct OutfitRow
{
    const char* id = "";
    const char* name = "";
    const char* detail = ""; // slot/kind/role + stat summary, prebuilt
    float price = 0.0f;
    int fitted = 0; // instances currently on the active ship (catalogs)
};

struct FleetRow
{
    const char* name = "";
    bool active = false;
    bool storedHere = false; // stored at THIS station (switch/sell allowed)
    float value = 0.0f;      // hull + fit at list price
};

// One faction's standing line for the Factions tab (Phase 8b).
struct FactionRow
{
    const char* name = "";
    const char* detail = "";   // kind + war list, prebuilt
    float standing = 0.0f;     // -100..100
    const char* attitude = ""; // "hostile"/"neutral"/"friendly"
};

// One mission line for the Missions tab (Phase 8c): a board offer or a
// journal entry, detail prebuilt by the game.
struct MissionRow
{
    const char* title = "";
    const char* detail = "";  // poster, objective/progress, deadline
    float reward = 0.0f;      // credits on completion
    bool acceptable = true;   // offers: standing clears the min_rep tier
    bool campaign = false;
    bool tracked = false;     // journal: shown on the HUD
};

// One line of unsold survey data on the station's Survey tab (Phase 8e).
struct SurveyRow
{
    const char* system = "";
    const char* detail = ""; // kind + region + first-discovery, prebuilt
    float value = 0.0f;      // credits this line pays
};

// --- Map screens (Phase 8e; deferred here out of Phase 8d) -------------------

// How much the player knows about a system, in the order the ladder runs.
enum class MapKnowledge : std::uint32_t
{
    Unknown = 0,
    Charted, // a gate named it: position and name, nothing else
    Visited,
    Surveyed,
};

// One system on the galaxy map. The game fills these from the galaxy and what
// SurveySim says is known, so the screen never sees the galaxy itself and
// cannot leak what the player has not earned.
struct MapSystemRow
{
    const char* name = "";
    core::Vec2 position; // light-years, galaxy map space
    MapKnowledge knowledge = MapKnowledge::Unknown;
    core::Vec3 ownerColor{0.6f, 0.6f, 0.6f};
    const char* detail = ""; // owner, region, stations, sites - prebuilt
    bool hasOwner = false;
    bool current = false;
    bool onRoute = false;
};

// A lane between two rows of the system list; drawn only when both ends are
// known, which is what makes the map grow along the routes actually flown.
struct MapLaneRow
{
    int from = 0;
    int to = 0;
    bool onRoute = false;
};

// One thing in the current system, for the system map.
struct MapMarkerRow
{
    enum class Kind : std::uint32_t
    {
        Star = 0,
        Planet,
        Station,
        Gate,
        Signal,
    };
    Kind kind = Kind::Star;
    const char* name = "";
    const char* detail = "";
    core::Vec2 position;       // meters, playfield plane relative to the hub
    double distanceMeters = 0.0; // from the ship
    bool scanned = false;      // bodies: surveyed; signals: identified
    bool targeted = false;
};

struct MapAction
{
    enum class Kind : std::uint32_t
    {
        None = 0,
        PlotRoute,    // index = system row
        ClearRoute,
        SelectMarker, // index = marker row: becomes the nav target
        Autopilot,    // index = marker row: target it and engage
        Close,
    };
    Kind kind = Kind::None;
    int index = -1;
};

// The map screen: a galaxy view over the lane graph and a system view of the
// playfield. Both are filled per frame like every other screen here.
struct MapPanel
{
    const char* currentSystem = "";
    const char* routeSummary = ""; // "4 jumps: A > B > C", prebuilt
    const char* knownSummary = ""; // "31 of 80 systems known", prebuilt
    std::span<const MapSystemRow> systems;
    std::span<const MapLaneRow> lanes;
    std::span<const MapMarkerRow> markers;
    int currentIndex = -1; // row of the system the player is in
    MapAction action;      // out
};

// What the player clicked this frame; the game executes it.
struct StationAction
{
    enum class Kind : std::uint32_t
    {
        None = 0,
        BuyModule,
        SellModule,
        BuyWeapon,
        BuyShip,
        SellShip,
        SwitchShip,
        HireCrew,
        FireCrew,
        AcceptMission,
        AbandonMission,
        TrackMission,
        SellSurveyData, // Phase 8e: the whole ledger, at any station
    };
    Kind kind = Kind::None;
    const char* id = ""; // def id (module/weapon/ship/crew actions)
    int index = -1;      // fleet index, or mission offer/journal index
};

// The docked-station screen: Trade plus the Phase 8a Outfitting, Shipyard,
// and Crew tabs, the Phase 8b Factions tab, and the Phase 8c Missions tab.
struct StationPanel
{
    TradePanel trade;
    const char* fitSummary = "";  // active ship fit + budgets, prebuilt
    double deductible = 0.0;      // current insurance quote
    std::span<const OutfitRow> modules; // catalog
    std::span<const OutfitRow> weapons; // catalog ("fitted" flags the mount)
    std::span<const OutfitRow> crewCatalog;
    std::span<const OutfitRow> crewAboard;
    std::span<const OutfitRow> shipCatalog;
    std::span<const FleetRow> fleet;
    std::span<const FactionRow> factions; // standings (Phase 8b)
    const char* factionNotes = "";        // recent raids summary, prebuilt
    std::span<const MissionRow> missionOffers;  // the board (Phase 8c)
    std::span<const MissionRow> missionJournal; // active missions
    std::span<const SurveyRow> surveyData;      // unsold ledger (Phase 8e)
    double surveyValue = 0.0;                   // what the ledger pays today
    StationAction action; // out
};

} // namespace sol::ui
