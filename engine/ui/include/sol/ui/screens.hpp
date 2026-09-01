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
    Objective, // Phase 8i: where the tracked mission says to go
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
    // The game's own index for this contact, carried through the fill's
    // nearest-first sort so a click on the disc can name what it hit (Phase
    // 8j). Opaque here: the disc never interprets it.
    std::uint32_t selection = 0;
};

// One line of radio traffic (Phase 8r). `fade` is 0..1 and 1 while the line is
// fresh, so an old line dims out instead of vanishing mid-read. The HUD does
// not know who anybody is: the game supplies both strings already composed.
struct CommsLine
{
    const char* from = "";
    const char* text = "";
    float fade = 1.0f;
};

// --- The surface the HUD is drawn on (Phase 8m) -------------------------------

// One projected mount point on the cockpit. `visible` is false once the head
// has turned far enough that the anchor is behind the eye, where the
// projection would mirror it onto the wrong side of the screen.
struct HudAnchorPoint
{
    core::Vec2 position;
    bool visible = false;
};

// Where the HUD hangs. In the cockpit these are points on the frame, projected
// fresh every frame by cockpit_frame.hpp; in chase and free cameras they are
// the screen corners the HUD used before there was a cockpit, so one layout
// path serves both and the external views stay pixel-identical.
struct HudFrame
{
    // The HUD reads this only to decide whether anything can be occluded,
    // never to pick a layout.
    bool cockpit = false;

    HudAnchorPoint leftConsole;   // flight panel, bottom-left corner
    HudAnchorPoint rightConsole;  // power block, bottom-right corner
    HudAnchorPoint centreConsole; // radar disc, centre

    // The glass, as an axis-aligned rect. World-referenced HUD elements clip
    // to it and the off-screen target arrow rides its rim. Axis-aligned is an
    // approximation of an opening that is not - DrawList::pushClip takes a
    // rect - and it errs inward, so a marker stops short of the frame rather
    // than crossing it. Empty means there is no glass in front of the player.
    core::Vec2 apertureMin;
    core::Vec2 apertureMax;

    [[nodiscard]] constexpr bool apertureEmpty() const
    {
        return apertureMax.x <= apertureMin.x || apertureMax.y <= apertureMin.y;
    }

    [[nodiscard]] constexpr bool insideAperture(core::Vec2 point) const
    {
        return point.x >= apertureMin.x && point.x < apertureMax.x && point.y >= apertureMin.y &&
               point.y < apertureMax.y;
    }
};

// Everything the flight HUD draws.
struct FlightHud
{
    bool active = false;
    float speedMetersPerSecond = 0.0f;
    bool assist = true;
    bool boost = false;
    bool cruise = false;
    // Phase 36 stage A. The negative is the state worth drawing: a transponder
    // is on for the whole of ordinary play, so a lamp that is lit 99% of the
    // time is furniture. This lights only when the ship is running dark.
    bool runningDark = false;
    // What the ship has been told to do for itself, or "" when it is being
    // flown by hand (Phase 28). Was `bool autopilot`, and the widening is the
    // same one the game layer made: with seven modes instead of one, a lit
    // lamp can no longer say WHICH. The chip prints this verbatim.
    //
    // ⚑ A label rather than an enum on purpose. CommandMode is a game-layer
    // noun and the engine has no business knowing what "orbit" means — the same
    // split input_actions.hpp draws for Action, and the reason `cameraMode` and
    // `targetName` beside it are strings too.
    const char* commandLabel = "";
    const char* cameraMode = "";
    const char* targetName = "";
    double targetDistanceMeters = 0.0;
    // Signed and RELATIVE since Phase 11: the target's own velocity is in it,
    // so a trader leaving while the player sits still reads negative. Negative
    // means the gap is opening.
    float closingSpeedMetersPerSecond = 0.0f;
    // Seconds to the target at the current closing rate, over the same surface
    // distance the panel prints (Phase 11). **Negative means there is no ETA**
    // - the gap is not closing - which is a state the readout says out loud
    // rather than hiding, so the row never appears and vanishes.
    double etaSeconds = -1.0;
    core::Vec3 targetDirectionCamera; // unit, camera space (-Z forward)
    float tanHalfFovY = 1.0f;

    // Power pips (decisions/003); pipMax caps each bar, charge is 0..1.
    int pipsWeapons = 2;
    int pipsEngines = 2;
    int pipsShields = 2;
    int pipMax = 4;
    float weaponCharge = 1.0f;

    // Fire groups (Phase 31 stage C3). `fireGroupsUsed` is a bit per group -
    // bit (n-1) set when at least one fitted gun answers to trigger n - and
    // `fireGroup` is the one the trigger is on.
    //
    // ⚑ THE ROW IS DRAWN ONLY WHEN MORE THAN ONE BIT IS SET, which is the
    // whole reason the mask is here rather than a count and a selection. A
    // ship whose guns all sit in one group has no choice to make, and a
    // permanent readout of a control that cannot do anything is the kind of
    // clutter a cockpit HUD can least afford. It appears the moment the player
    // splits their guns on the ship readout, which is where they learn the key
    // exists at all.
    unsigned fireGroupsUsed = 0;
    int fireGroup = 1;

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
    // The gate being approached (Phase 8v): where it leads and how far off it
    // is, so the chip can say "GATE: QUEIS 4.2 km" instead of naming a key
    // that no longer exists.
    const char* gateDestination = "";
    double gateDistanceMeters = -1.0;
    bool gateInRange = false;
    bool dockInRange = false;
    bool docked = false;
    const char* dockedStationName = "";
    // Mid-jump (Phase 8v). Every interact prompt is refused for the length of
    // the transition, so the row is suppressed rather than left advertising
    // four things that will not happen. Before 8v this window did not exist:
    // pressing jump put you in the next system the same frame.
    bool jumping = false;

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

    // Prompt keys (Phase 8k). The HUD used to print "[J] JUMP" as a literal,
    // which a rebind would turn into a confident lie. The fill supplies the
    // name of whatever chord currently drives each prompt; empty means the
    // action is unbound, and the chip then names the action alone rather than
    // instructing the player to press nothing.
    // No jumpKey since Phase 8v: a gate is flown through, so its chip is an
    // approach readout rather than a key legend.
    const char* interactKey = ""; // dock and salvage share it
    const char* scanKey = "";
    const char* hailKey = ""; // Phase 8s: talking to a ship is its own verb

    // Comms (Phase 8r): the last few things said to the player, newest last.
    // Built for docking clearance but deliberately not named after it — the
    // pilot-info half of the same playtest note inherits a channel rather than
    // starting from nothing. The span outlives the frame that fills it.
    std::span<const CommsLine> comms;
    // Docking clearance, for the prompt chip: which berth was assigned and how
    // far away it is. `dockInRange` above still means "close enough to use the
    // shortcut"; these mean "you have been told where to park".
    bool cleared = false;
    int clearedBerth = 0; // 1-based, for display
    double clearedBerthDistanceMeters = 0.0;
    bool stationInHailRange = false;
    // Phase 8s: a ship contact is selected and close enough to talk to. The
    // fill answers it, because "is the selection a ship" is the world's
    // question and the HUD has never known what a contact is.
    bool shipInHailRange = false;

    // The cockpit (Phase 8m). `frame` says where the panels mount and how much
    // glass there is; the HUD reads nothing else about the view.
    HudFrame frame;
    // Where the ship's nose points, in camera space. Until free-look existed
    // this was always (0,0,-1) and the crosshair could simply be drawn at the
    // centre of the screen; with the head turned it is not, and a crosshair
    // that stayed centred would be telling the player they are aiming
    // somewhere they are not.
    core::Vec3 boresightDirectionCamera = {0.0f, 0.0f, -1.0f};
};

// One commodity line on the station's Trade tab. The game fills rows from the
// docked market; a clicked button reports back through `action`.
// What the jurisdiction the player is standing in says about a good on the
// market floor (Phase 33 stage D). ⚑ There is no `Unpoliced` here on purpose:
// "nobody holds this place" is a fact about the SYSTEM and belongs on the
// panel, not repeated down every row - and repeating it per row would say the
// station has an opinion about each good, which is the opposite of the truth.
enum class TradeLegality : std::uint32_t
{
    Legal = 0,
    Restricted, // licensed: carriable, papers wanted
    Contraband, // forbidden here
};

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
    // Under whose law you are standing, not what the good IS (Phase 33 stage
    // D). The same crate is `Legal` one jump away.
    TradeLegality legality = TradeLegality::Legal;
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
    // Whose law applies here (Phase 33 stage D), or empty where nobody holds
    // the system. ⚑ EMPTY IS NOT "NO LAW WORTH MENTIONING" - it is the whole
    // second half of the feature. A jurisdiction that lists nothing and a
    // system with no jurisdiction both leave every row unmarked, and this line
    // is the only thing that tells them apart on the screen.
    const char* jurisdiction = "";
};

// Catalog/inventory row for the outfitting, shipyard, and crew tabs (Phase 8a).
struct OutfitRow
{
    const char* id = "";
    const char* name = "";
    const char* detail = ""; // mount kind/size, role + stat summary, prebuilt
    float price = 0.0f;
    int fitted = 0; // instances currently on the active ship (catalogs)
    // Where a Fit would put it, empty when there is nowhere for it (Phase 31
    // stage B). The GAME decides this - honouring `StationPanel::selectedMount`
    // and otherwise running the same `firstFreeMountFor` the purchase uses - so
    // the screen never re-implements the accept rule. Empty greys the button,
    // which is the only reason a refusal would otherwise be invisible: the
    // station action has no channel to report one back through.
    const char* targetMount = "";
};

// One place on the active hull and what is in it (Phase 31 stage B). This is
// the outfitting screen's spine: a ship is its mounts, so the screen is a list
// of them rather than a list of things owned.
struct MountRow
{
    const char* id = "";     // the def's mount id, and what an action names
    const char* kind = "";   // "turret", "engine", ... (gdd.md 11.5)
    const char* size = "";   // "small" | "medium" | "large" | "xlarge"
    const char* fitted = ""; // display name of what is in it, "" = empty
    const char* detail = ""; // stat summary of the fitting, or the mount's own
    bool external = false;   // `at` present: drawn on the hull, shootable
    float resale = 0.0f;     // what Remove refunds
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
    const char* detail = ""; // poster, objective/progress, deadline
    float reward = 0.0f;     // credits on completion
    bool acceptable = true;  // offers: standing clears the min_rep tier
    bool campaign = false;
    bool tracked = false; // journal: shown on the HUD
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
    // An optional trailing button on the row (Phase 31 stage C3). `button` is
    // what it says; `action` is the opaque id the screen writes back when it is
    // clicked, and an EMPTY action is a row that is only text - which is every
    // row every info section had before this. The screen never interprets
    // either string: what a row's action means is the filler's business.
    const char* button = "";
    const char* action = "";
};

// Everything the ship screen draws.
//
// ⚑ IT WAS READ-ONLY UNTIL PHASE 31 STAGE C3, and the design statement that
// made it so still stands: REFITTING is a station activity, because unbolting a
// gun in flight is not a thing. A FIRE GROUP is not a refit - it is which
// trigger an already-bolted gun answers to, which is a console setting of
// exactly the kind as the power pips this screen has always shown - so it is
// the one thing here the player can change, and it is changed from the screen
// that already lists their guns in mount order.
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
    std::span<const InfoRow> fitted;  // weapon, components, crew
    std::span<const InfoRow> cargo;   // manifest, one line per commodity held
    // Out: the `action` of whatever row button the player clicked this frame,
    // empty otherwise - the same fill-then-execute seam `StationPanel::action`
    // draws, and for the same reason: no gameplay logic moves into the UI.
    const char* action = "";
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
    // Phase 8h: the player has written a place down in this system. The
    // galaxy map marks it so a bookmark made three systems ago is findable
    // without remembering which system it was in - and once the system is
    // selected, Plot Route already gets you back there.
    std::uint32_t bookmarkCount = 0;
    // Phase 8i: the tracked mission's current objective is in this system.
    // An objective in another system has no marker to draw anywhere else, so
    // without this flag the galaxy map is silent about the one thing the
    // player has actually been told to do; Plot Route then already gets there.
    bool hasObjective = false;
    // Phase 8u: a live contest over this system. Drawn as a ring in the
    // attacker's colour, so the player sees a FRONT rather than only its
    // outcome - a border that moved with no warning is not a feature. Gated
    // behind the same "visited" rule ownership itself is: a war you have
    // only heard of from a gate is not yours to know about.
    bool contested = false;
    core::Vec3 contestColor{0.9f, 0.4f, 0.2f};
    // Trade overlay (Phase 8g), meaningful only when MapPanel::tradeCommodity
    // is set. `tradeLevel` is where this price sits between the cheapest and
    // dearest the player has seen, which is what makes a route legible at a
    // glance instead of a table to read.
    bool hasTrade = false;
    float tradePrice = 0.0f;
    float tradeLevel = 0.0f; // 0 = cheapest known, 1 = dearest known
    bool tradeStale = false;
    // System security (Phase 30 stage D, decisions/019): the LIVE rating -
    // the generated baseline eroded by how dangerous the place is right now -
    // on the signed scale whose sign names WHO polices it. Positive is a
    // major, negative is a clan, zero is nobody.
    //
    // ⚑ Knowledge on exactly the terms ownership is: a system the player has
    // only heard of from a gate does not tell you how well it is policed, and
    // `hasSecurity` is false there. That is not a nicety - the number is the
    // one thing a route is planned around, so being able to LEARN it is what
    // makes going somewhere worth anything.
    bool hasSecurity = false;
    float security = 0.0f;
    // ⚑⚑ Whether a call here is answered AT ALL, decided by the same
    // `securityAnswers` band the dispatcher itself refuses on. Carried on the
    // row rather than recomputed by the screen so the constant lives in one
    // file: the game layer knows the rule, the UI layer only draws it.
    bool securityAnswers = false;
};

// A lane between two rows of the system list; drawn only when both ends are
// known, which is what makes the map grow along the routes actually flown.
struct MapLaneRow
{
    int from = 0;
    int to = 0;
    bool onRoute = false;
};

// A row that answers to no nav-target slot: every row of a system the player
// is not standing in, because a remote system has no live nav list at all.
inline constexpr std::uint32_t kNoNavTarget = 0xffff'ffffu;

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
        Bookmark,  // Phase 8h: a place the player wrote down
        Objective, // Phase 8i: where the tracked mission says to go
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
    // From the ship in the system the player is in; from that system's own
    // star when the map is showing somewhere else (Phase 8q), because
    // "412 Mkm from your ship" is not a number about a system four jumps away.
    double distanceMeters = 0.0;
    bool scanned = false; // bodies: surveyed; signals: identified
    bool targeted = false;
    // Phase 8h/8q: which bookmark this row is, for a Bookmark marker; 0
    // otherwise. The Delete action carries the id rather than the row because
    // a remote row is not a nav-target slot and must never be used as one -
    // handing it to navTargetBookmark() would delete whatever bookmark
    // happened to occupy that slot in the system the player is standing in.
    std::uint32_t bookmarkId = 0;
    // Phase 15: which nav-target slot this row was built from, or kNoNavTarget
    // on a remote view. Set Target and Autopilot carry THIS and never the row
    // index, for the same reason `bookmarkId` exists one field up: the local
    // fill walks every nav slot and skips the fogged ones, so its loop counter
    // and the row number stop agreeing the moment anything is undiscovered -
    // and a shifted index either selects the wrong thing or, if it lands on a
    // fogged slot, is refused and silently does nothing at all.
    std::uint32_t navTarget = kNoNavTarget;
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
        PlotRoute, // index = system row
        ClearRoute,
        SelectMarker, // Phase 15: index = NAV-TARGET SLOT, not the marker row
        Autopilot,    // Phase 15: index = nav-target slot: target it and engage
        Close,
        SetTradeCommodity, // Phase 8g: index = commodity, or -1 to turn it off
        // Phase 30 stage D: index != 0 turns the security gradient on. It and
        // the trade overlay are ONE colour channel, so whoever executes these
        // turns the other off - which is why the picker cannot express the
        // whole cycle with a single action kind.
        SetSecurityOverlay,
        DeleteBookmark, // Phase 8h/8q: bookmarkId names the bookmark
        // Phase 28 stage D: a right-click inside the system map. index = the
        // NAV-TARGET SLOT under the cursor, or -1 for a click that hit nothing
        // - which still opens a menu, about whatever is already selected, for
        // the same reason a right-click on empty space does in flight.
        CommandMenu,
    };
    Kind kind = Kind::None;
    int index = -1;
    // DeleteBookmark only. A never-reused id rather than a row, so the action
    // is answerable for a system the player is not in - see MapMarkerRow.
    std::uint32_t bookmarkId = 0;
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
    // Which system the System tab is showing (Phase 8q). IN: set from the
    // screen's own galaxy selection, the way tradeCommodity is, so picking a
    // system on the galaxy map and switching tabs looks inside it. -1 means
    // "wherever the player is", which is what the tab did before 8q.
    int viewSystem = -1;
    const char* viewSystemName = ""; // header, when the System tab is up
    // "Coriolis Reach - visited, 3 jumps away", prebuilt. Says the rung and
    // the distance in words, because the one thing a remote map must never do
    // is imply it is as current as the local one.
    const char* viewSummary = "";
    // False when the System tab is showing somewhere the player is not: the
    // ship glyph, the target ring and the two actions that need a nav-target
    // slot are all statements about being there.
    bool viewIsCurrent = true;
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
    // Security overlay (Phase 30 stage D): the galaxy map colors every visited
    // system by how well it is policed. Shares the colour channel with the
    // trade overlay and the same one cycling button picks between them, so at
    // most one of the two is ever set.
    bool securityOverlay = false;
    const char* securitySummary = ""; // the legend, prebuilt
    // "Policed by Solar Navy - security +0.85", for whichever system the System
    // tab is showing. Names WHO polices it, which the galaxy row does not: the
    // row already carries the owner, and this one is about the law rather than
    // the flag. Refuses for a system the player has only heard of.
    const char* viewSecurity = "";
    MapAction action; // out
};

// What the player clicked this frame; the game executes it.
struct StationAction
{
    enum class Kind : std::uint32_t
    {
        None = 0,
        BuyFitting,  // id = component/weapon def, mount = where (empty = anywhere)
        SellFitting, // mount = which place to strip
        BuyShip,
        SellShip,
        SwitchShip,
        HireCrew,
        FireCrew,
        AcceptMission,
        // Phase 35 stage D: work heard in the room rather than read off the
        // board. A separate kind and not an `AcceptMission` with a flag,
        // because it indexes a different list - see `MissionSim::leads()`.
        AcceptLead,
        AbandonMission,
        TrackMission,
        SellSurveyData, // Phase 8e: the whole ledger, at any station
        OrderRefine,    // Phase 8f: units carries the order size
        CollectRefined,
        BuyMarketIntel, // Phase 8g: price lists for the markets in reach
    };
    Kind kind = Kind::None;
    const char* id = "";    // def id (component/weapon/ship/crew actions)
    const char* mount = ""; // mount id (fitting actions, Phase 31 stage B)
    int index = -1;         // fleet index, or mission offer/journal index
    float units = 0.0f;     // refinery order size
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
    float readyUnits = 0.0f;   // finished output waiting here
    double waitSeconds = -1.0; // on the soonest unfinished order; < 0 = none
    float cargoSpace = 0.0f;   // room left in the hold
};

// The docked-station screen: Trade plus the Phase 8a Outfitting, Shipyard,
// and Crew tabs, the Phase 8b Factions tab, and the Phase 8c Missions tab.
struct StationPanel
{
    TradePanel trade;
    const char* fitSummary = "";      // active ship fit + budgets, prebuilt
    double deductible = 0.0;          // current insurance quote
    std::span<const MountRow> mounts; // the active hull's places (31B)
    // IN, not out: which mount the player has aimed the catalogs at, or empty
    // for "wherever it goes". The screen owns the selection because it is a
    // thing the player is holding rather than a thing the world knows, and it
    // is handed back here so that ONE place - the fill - decides where a Fit
    // would land.
    const char* selectedMount = "";
    std::span<const OutfitRow> components; // catalog
    std::span<const OutfitRow> weapons;    // catalog
    std::span<const OutfitRow> crewCatalog;
    std::span<const OutfitRow> crewAboard;
    std::span<const OutfitRow> shipCatalog;
    std::span<const FleetRow> fleet;
    std::span<const FactionRow> factions;       // standings (Phase 8b)
    const char* factionNotes = "";              // recent raids summary, prebuilt
    std::span<const MissionRow> missionOffers;  // the board (Phase 8c)
    std::span<const MissionRow> missionJournal; // active missions
    std::span<const SurveyRow> surveyData;      // unsold ledger (Phase 8e)
    double surveyValue = 0.0;                   // what the ledger pays today
    RefinePanel refinery;                       // refining service (Phase 8f)
    // The room the player is standing in, and what it has to say (Phase 35
    // stage A). `barRoom` is the largest recreation module the station has -
    // "Bar", "Concourse", "Resort" - because the ROOM is what the heading of a
    // screen about a room should name.
    //
    // ⚑ `InfoRow` rather than a row type of its own, for the reason that
    // struct was written: "a dozen small tables and inventing a row type per
    // table would be a dozen structs that all say the same thing". It already
    // carries the optional trailing button and the opaque action string that
    // Phase 35 stage D needs for a lead you can take, so nothing here has to
    // move when talk stops being only about this dock.
    const char* barRoom = "";
    std::span<const InfoRow> barTalk;
    // Who is doing the talking (Phase 35 stage C), already worded - a name, a
    // trade, and whether they have seen the player before. Every room in the
    // galaxy has somebody in it, so this is empty only where there is no room
    // at all and the tab is not on the strip either.
    //
    // ⚑⚑ A HEADING RATHER THAN A `BarLine`, AND THAT IS WHAT KEEPS THE TOPIC
    // COLUMN A CLOSED VOCABULARY. The rows below draw their topic into a fixed
    // 150px cell, which is the reason stage B refused to let a hook write one;
    // a person's name is authored content of no controlled length and would
    // have broken exactly that rule. The speaker is not a topic - they are who
    // the whole screen is - so they belong beside the room's name.
    const char* barKeeper = "";
    // Which tabs this station offers (Phase 34 stage C): bit `i` set means the
    // screen the GAME layer numbers `i` belongs on the strip.
    //
    // ⚑⚑ OPAQUE HERE ON PURPOSE, AND THAT IS THE SEAM RATHER THAN A SHORTCUT.
    // `sol::ui` must not learn what a dock tab is - it lays out however many
    // labels it is handed and has no opinion about which - and the game layer
    // must not learn what a `[[module]]` is. So the bit ORDER belongs to
    // exactly one file, `game/src/station_screen.cpp`, where static assertions
    // pin it to both `game::StationScreenState::Tab` and
    // `sol::assets::StationScreen`. Nothing in this library reads it.
    //
    // ⚑ All bits set is what a station with no composition gets, which is every
    // tab: the behaviour that shipped before this phase, kept as the default so
    // that a filler which has never heard of modules still draws a whole screen.
    std::uint32_t screens = ~0u;
    StationAction action; // out
};

} // namespace sol::ui
