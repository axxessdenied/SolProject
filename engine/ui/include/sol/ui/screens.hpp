#pragma once

#include "sol/core/math/vec.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace sol::ui {

// The data contracts between the game and its screens (engine plan Phase 8d).
// The game fills one of these per frame from the world, a screen in
// `game/src/*_screen.cpp` draws it, and what the player did comes back in the
// `action` fields for the game to execute - the fill-then-execute seam that
// survived the move off the provisional Dear ImGui screens.

// --- Contact radar (Phase 8h) ------------------------------------------------

// What a contact is, so the disc can pick a glyph and a colour. The nav kinds
// mirror SpaceWorld::NavKind; Ship is the combat class the C key cycles.
enum class RadarKind : std::uint32_t
{
    Station = 0,
    Gate,
    Planet,
    Star,
    Signal,
    Field,
    Wreck,
    Bookmark,
    Ship,
};

// How the player stands toward a contact. Static scenery is Neutral; only
// ships carry a real attitude (Phase 8b's faction standing).
enum class RadarAttitude : std::uint32_t
{
    Neutral = 0,
    Friendly,
    Hostile,
};

// One thing on the disc. `offset` is in SHIP-LOCAL meters: +x right, +y up,
// -z forward, so the disc rotates with the ship and a dot at the top of it
// is dead ahead. The game fills these; the projection reaches into nothing.
struct RadarContact
{
    core::Vec3 offset;
    RadarKind kind = RadarKind::Ship;
    RadarAttitude attitude = RadarAttitude::Neutral;
    bool isTarget = false; // the current selection, drawn emphasized
};

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

    // Mining (Phase 8f). Prospecting reads whatever the boresight is on — a
    // field holds dozens of rocks, so a rock is read off the crosshair rather
    // than through the target cycle — and the collector ticks over as chunks
    // arrive in the hold.
    const char* prospectName = ""; // ore (or wreck) under the boresight
    bool prospectIsWreck = false;
    bool prospectInRange = false;  // inside the fitted beam's reach
    float prospectLeft = 0.0f;     // units still in it
    float prospectTotal = 0.0f;    // units when it was whole
    double prospectDistance = 0.0; // meters
    float collectedUnits = 0.0f;   // gathered just now; 0 = idle
    const char* collectedName = "";

    // Contact radar (Phase 8h): everything around the ship, not just what is
    // targeted. Nearest-first and capped by the fill, so the disc stays
    // readable in a system holding dozens of things.
    std::span<const RadarContact> radarContacts;
    double radarRangeMeters = 0.0; // what the outer ring stands for; 0 = auto
};

// One commodity line on the station's Trade tab. The game fills rows from the
// docked market; a clicked button reports back through `action`.
struct TradeRow
{
    const char* name = "";
    float price = 0.0f; // credits/unit right now
    float stock = 0.0f; // station stock, units
    float cargo = 0.0f; // in the player's hold, units
    // Best price the player has seen anywhere else, and how old that reading
    // is (Phase 8g). This column is what makes a trade route plannable from
    // the pad instead of from memory — and standing on a refinery it is what
    // finally says out loud that the metal is worth more three jumps away.
    bool hasElsewhere = false;
    float elsewherePrice = 0.0f;
    const char* elsewhereName = ""; // system the reading came from
    const char* elsewhereAge = "";  // prebuilt, e.g. "12m ago"
    bool elsewhereStale = false;    // past the staleness threshold: shown dim
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
    // The market-report offer (Phase 8g): what it costs, how many markets it
    // covers, and whether the player can afford it.
    double intelPrice = 0.0;
    std::uint32_t intelMarkets = 0;
    bool canBuyIntel = false;
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

// --- Ship information screen (Phase 8h) --------------------------------------

// One "label: value" line, with an optional dim note after it. Deliberately
// generic: the ship screen is a dozen small tables and inventing a row type
// per table would be a dozen structs that all say the same thing.
struct InfoRow
{
    const char* label = "";
    const char* value = "";
    const char* detail = ""; // dim, optional
};

// Everything the ship screen draws. Read-only: refitting stays a station
// activity, which is a design statement rather than a shortfall. What it adds
// is being able to read the outfitting numbers WHILE their consequences are
// happening, which the station pad by definition cannot do.
struct ShipInfoPanel
{
    const char* shipName = "";
    const char* shipClass = "";
    const char* fitSummary = ""; // power / slots / berths, prebuilt
    float hull = 1.0f;
    float shieldFore = 1.0f;
    float shieldAft = 1.0f;
    double credits = 0.0;
    float cargoUsed = 0.0f;
    float cargoCapacity = 0.0f;
    // Pips are shown here too, because half the stats below are what the pip
    // allocation is currently doing to the ship.
    int pipsWeapons = 2;
    int pipsEngines = 2;
    int pipsShields = 2;
    int pipMax = 4;
    std::span<const InfoRow> flight;  // thrust, speed, turn rates
    std::span<const InfoRow> defence; // shields, armor, hull, weapon
    std::span<const InfoRow> utility; // scan, collector, cargo
    std::span<const InfoRow> fitted;  // weapon, modules, crew
    std::span<const InfoRow> cargo;   // manifest, one line per commodity held
};

// --- Bookmark naming overlay (Phase 8h) --------------------------------------

// The little prompt B opens over the flight view. The name arrives prefilled
// from context, so accepting immediately is the common case and typing is the
// override; the screen edits `name` in place through the text field.
struct BookmarkPrompt
{
    bool open = false;
    std::string name;
    const char* whereSummary = ""; // "Lyrioa, 3.4 Mm from Ceres", prebuilt
    bool full = false;             // this system is at its bookmark cap
    // Set by the game when it opens the prompt; the screen consumes it to put
    // keyboard focus in the field, so the player can type without clicking.
    bool focusRequested = false;
    // True while `name` is still the untouched suggestion. The first typed
    // character then REPLACES it rather than appending, which is what makes a
    // prefilled name usable without selection ranges to delete it with.
    bool nameIsSuggestion = false;
    // Out: exactly one of these, on the frame the player decided.
    bool accepted = false;
    bool cancelled = false;
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
    // Trade overlay (Phase 8g), meaningful only when MapPanel::tradeCommodity
    // is set. `tradeLevel` is where this price sits between the cheapest and
    // dearest the player has seen, which is what makes a route legible at a
    // glance instead of a table to read.
    bool hasTrade = false;
    float tradePrice = 0.0f;
    float tradeLevel = 0.0f; // 0 = cheapest known, 1 = dearest known
    bool tradeStale = false;
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
        Field, // Phase 8f: an asteroid field
        Wreck,
        Bookmark, // Phase 8h: a place the player wrote down
    };
    Kind kind = Kind::Star;
    const char* name = "";
    const char* detail = "";
    // Meters in the playfield plane. What it is measured *from* depends on
    // which tier the marker belongs to (see `inPlayfield`), because a system
    // spans two wildly different scales: planets orbit 40-400 million km out,
    // while everything you actually fly to sits within a few hundred thousand
    // km of one planet. Drawing both against one origin makes the second group
    // a single dot.
    core::Vec2 position;
    double distanceMeters = 0.0; // from the ship
    bool scanned = false;        // bodies: surveyed; signals: identified
    bool targeted = false;
    // False: an orbital body, positioned from the star. True: something in
    // the playfield around the primary planet, positioned from that planet
    // and drawn in the expanded bubble the system map puts there.
    bool inPlayfield = false;
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
        SetTradeCommodity, // Phase 8g: index = commodity, or -1 to turn it off
        DeleteBookmark,    // Phase 8h: index = marker row (a Bookmark marker)
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
    // Where the playfield bubble is pinned: the primary planet's offset from
    // the star, in meters. Everything with `inPlayfield` is drawn around it.
    core::Vec2 hubPosition;
    core::Vec2 shipPosition; // the ship, meters from the hub
    bool hasShip = false;
    // Trade overlay (Phase 8g): pick a commodity and the galaxy map colors
    // every system the player has price data for. -1 is off.
    std::span<const char* const> commodityNames;
    int tradeCommodity = -1;
    const char* tradeSummary = ""; // "Refined Metal: 12 markets known", prebuilt
    MapAction action;              // out
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
        OrderRefine,    // Phase 8f: units carries the order size
        CollectRefined,
        BuyMarketIntel, // Phase 8g: price lists for the markets in reach
    };
    Kind kind = Kind::None;
    const char* id = ""; // def id (module/weapon/ship/crew actions)
    int index = -1;      // fleet index, or mission offer/journal index
    float units = 0.0f;  // refinery order size
};

// The docked station's refinery service (Phase 8f). Absent — refines false —
// at every station whose archetype does not refine anything.
struct RefinePanel
{
    bool refines = false;
    const char* inputName = "";  // what it takes
    const char* outputName = ""; // what it gives back
    float inputHeld = 0.0f;      // units of input in the hold
    float ratio = 0.0f;          // output units per input unit
    float feePerUnit = 0.0f;
    float readyUnits = 0.0f;  // finished output waiting here
    double waitSeconds = -1.0; // on the soonest unfinished order; < 0 = none
    float cargoSpace = 0.0f;   // room left in the hold
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
    RefinePanel refinery;                       // refining service (Phase 8f)
    StationAction action; // out
};

} // namespace sol::ui
