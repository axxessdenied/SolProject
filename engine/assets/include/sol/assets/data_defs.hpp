#pragma once

// Human-authored game data definitions (engine plan Phase 5): ships, weapons,
// factions, commodities, stations, components, and crew, parsed from TOML and
// validated against a strict schema (unknown or mistyped keys are errors, so typos die
// at load time, not silently at play time). Documents merge in layer order —
// a def re-using an earlier id replaces it wholesale, which is what gives mod
// directory layering (base game = mod zero) its override semantics.
//
// Defs use flat snake_case keys inside [[ship]] / [[weapon]] / [[faction]]
// array-of-table elements; see game/data/ for the base-game examples.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sol::assets {

// The stats outfitting modifiers may touch (engine plan Phase 8a). TurnRate
// applies uniformly to all three max_turn_rate axes.
enum class FitStat : std::uint32_t
{
    ForwardAccel = 0,
    ReverseAccel,
    LateralAccel,
    VerticalAccel,
    MaxSpeed,
    TurnRate,
    CruiseSpeedScale,
    ShieldStrength,
    ShieldRegen,
    Armor,
    Hull,
    WeaponCapacitor,
    WeaponRecharge,
    Cargo,
    ScanRange,      // Phase 8e: pulse/target-scan reach
    ScanSpeed,      // Phase 8e: target-scan progress rate
    CollectorRange, // Phase 8f: how far mined ore is drawn in from
    // Phase 36 stage E: how loud this ship is to somebody else's sensors. The
    // only stat here that is better LOW, and the first one that exists to be
    // read by an NPC rather than by the player - which is why it has no unit:
    // it is a multiplier on two things a patrol does, not a quantity the ship
    // has. See `SpaceWorld::signature` for what it moves and why the two things
    // it moves are those two and not the obvious third.
    Signature,
    Count,
};

inline constexpr std::size_t kFitStatCount = static_cast<std::size_t>(FitStat::Count);

// Shared modifier vocabulary for components and crew ("<stat>_add"/"<stat>_mul"
// def keys). Resolution is order-independent: adds sum onto the base stat,
// then muls multiply the result (see loadout.hpp).
struct StatModifiers
{
    static constexpr std::array<float, kFitStatCount> ones()
    {
        std::array<float, kFitStatCount> a{};
        a.fill(1.0f);
        return a;
    }

    std::array<float, kFitStatCount> add{};
    std::array<float, kFitStatCount> mul = ones();
};

// Mirrors sim::ShipTuning field-for-field as plain floats (assets sits below
// sim in the layering, so it cannot include the sim type; the game converts).
struct ShipFlightTuning
{
    float forwardAccel = 60.0f;
    float reverseAccel = 40.0f;
    float lateralAccel = 30.0f;
    float verticalAccel = 30.0f;
    float maxSpeed = 220.0f;
    float maxTurnRate[3] = {1.6f, 1.2f, 2.6f}; // pitch, yaw, roll (rad/s)
    float angularAccel[3] = {6.0f, 4.5f, 9.0f};
    float boostAccelScale = 3.0f;
    float boostSpeedScale = 1.75f;
    float cruiseSpeedScale = 25'000.0f;
    float cruiseAccelScale = 12'000.0f;
};

// Mirrors sim::DefenseTuning (directional shields per decisions/002).
struct ShipDefenseTuning
{
    float shieldStrength = 100.0f; // hp per facing
    float shieldRegen = 8.0f;      // hp/s before pips scaling
    float shieldRegenDelay = 4.0f; // seconds after a hit
    float armor = 50.0f;           // ablative
    float hull = 100.0f;
};

// Mirrors the def-driven part of sim::PowerTuning (pips per decisions/003).
struct ShipPowerTuning
{
    float weaponCapacitor = 100.0f;
    float weaponRecharge = 15.0f; // units/s at scale 1
};

// Catalog gating shared by sellable defs (Phase 8b): which faction's stations
// stock the item (empty = every faction) and the player standing the owner
// requires before selling it (pirate-kind owners fence past min_rep).
struct CatalogGate
{
    std::vector<std::string> factions; // seller allowlist of faction ids
    float minRep = -100.0f;            // owner-standing gate, -100..100
    // A commodity the docked station must actually have in stock before it will
    // sell this (Phase 33 stage B). Empty - the default - means no such
    // requirement, so every def written before this key behaves exactly as it
    // did.
    //
    // ⚑⚑⚑ THIS IS THE LAST HOP OF THE MATERIAL TREE AND THE ONLY REASON THE
    // TREE IS IN THE GAME RATHER THAN IN A SPREADSHEET. gdd.md 6 says what T2
    // components are for: "these are what a mount fitting is made of (11), which
    // is what finally ties outfitting to the economy - the gun you buy was
    // manufactured somewhere by somebody out of things somebody mined." Before
    // this key an economy could run its whole chain and a player would never
    // meet it; a fitting was for sale everywhere its faction and standing gates
    // allowed, whatever the galaxy had actually built.
    //
    // ⚑⚑ IT GATES AVAILABILITY, NOT PRICE, AND THAT IS DELIBERATE. A price
    // that moved with local stock would be the same idea expressed as a number
    // nobody reads; an outfitting list that is SHORTER at a frontier station
    // than at a Fabrication Works is the same idea expressed as an absence the
    // player can see. Price is stage D's business, where it says something
    // different (what a jurisdiction thinks of the crate).
    //
    // ⚑ It sits on the shared gate rather than on `ComponentDef` because a
    // weapon is made of the same things - `WeaponDef`, `ShipDef` and `CrewDef`
    // all carry a `CatalogGate` and all four are things a station sells. What a
    // crew member would require is a question for whoever wants one; nothing
    // shipped uses it outside components.
    std::string requiresCommodity;
};

// --- Mounts (engine plan Phase 31, decisions/014, gdd.md 11.5) ---
//
// A mount is a named, typed, sized place on a hull where exactly one fitting
// goes. The vocabulary below is gdd.md 11.5's list verbatim rather than a
// convenient subset, because a hull def and a faction roster and the Forge's
// mount tool all have to be describing the same thing, and a kind added later
// is a kind every one of them has to learn.
enum class MountKind : std::uint32_t
{
    Turret = 0, // a gun that traverses; `arc` is how far
    Fixed,      // a gun bolted to the hull, aimed by flying the ship
    Launcher,   // missiles
    Bay,        // ordnance too large to rail-launch: torpedoes, bombs
    Engine,     // main drive
    Thruster,   // manoeuvring
    Shield,
    Armor,
    Utility,   // external kit: scoops, collectors, pods
    Subsystem, // internal: sensors, fire control, drive tuning, covert suites
    Hangar,    // carries craft
    Dock,      // carries something that docks TO it
    Count,
};

inline constexpr std::size_t kMountKindCount = static_cast<std::size_t>(MountKind::Count);

enum class MountSize : std::uint32_t
{
    Small = 0,
    Medium,
    Large,
    XLarge,
    Count,
};

inline constexpr std::size_t kMountSizeCount = static_cast<std::size_t>(MountSize::Count);

// The def spellings, and the only place they live. Parse returns false on a
// word that is not one of them; the caller decides whether that is an error.
[[nodiscard]] const char* mountKindName(MountKind kind);
[[nodiscard]] bool parseMountKind(std::string_view text, MountKind& out);
[[nodiscard]] const char* mountSizeName(MountSize size);
[[nodiscard]] bool parseMountSize(std::string_view text, MountSize& out);

// decisions/014 rule 3: a mount accepts its own size or smaller. Fitting small
// kit to a large mount WASTES the mount, and that waste is the player's trade
// rather than an error - which is why this is an ordering and not an equality.
//
// The ordering is the enum's own, so `Small < Medium < Large < XLarge` is a
// fact about the declaration order above and moving a member reorders the fit
// rules with it.
[[nodiscard]] constexpr bool mountAccepts(MountSize mount, MountSize fitting)
{
    return static_cast<std::uint32_t>(fitting) <= static_cast<std::uint32_t>(mount);
}

// What it takes to knock a mount out (Phase 31 stage F, decisions/014's
// "each mount carries hit points"). It is a pool of its own beside the hull's,
// not a slice of it: a mount is a thing bolted to a ship that can be shot off
// while the ship keeps flying, which is the whole of GDD 5's systems-damage
// promise and the difference between disabling a freighter and killing it.
//
// ⚑⚑ IT IS DERIVED FROM `size` AND THERE IS DELIBERATELY NO AUTHORED KEY.
// Size is the only thing a mount already declares about how much kit it holds,
// and it is declared on every one of the twenty mounts in this game - so this
// rule needs no content edit, no parser key, no writer key, and no new field in
// the Forge's mount tool. The alternative is asking an author for a number per
// mount that nothing in the shape of a hull tells them how to pick. Phase 32
// authors the hull spine, and a hull that genuinely needs an armoured drive
// bell is where an `hp` override earns its key - against real content, rather
// than blind and in advance.
//
// The steps are ~2.5x, which prices a mount against the hull class that
// carries it: the shipped freighter is 460 of armour and hull and its drive is
// a `medium`, so taking its engine costs roughly a quarter of what killing it
// costs - and only from astern, where the drive actually is.
[[nodiscard]] constexpr float mountHitPoints(MountSize size)
{
    switch (size) {
    case MountSize::Small:
        return 60.0f;
    case MountSize::Medium:
        return 150.0f;
    case MountSize::Large:
        return 375.0f;
    case MountSize::XLarge:
        return 900.0f;
    case MountSize::Count:
        break;
    }
    return 60.0f;
}

// Whether a mount of one kind takes a fitting of another (Phase 31 stage B).
// The rule is EQUALITY plus exactly one asymmetry, and the asymmetry is the
// difference between `turret` and `fixed` said in one line:
//
//   A turret is a ring with a traverse motor. Bolting a fixed gun into one
//   gives you a gun that traverses, which is the whole of what the mount is
//   FOR - so the traverse belongs to the mount (`arc` is authored there,
//   never on the weapon) and a gun is just a gun. The reverse is refused: a
//   hardpoint with no ring cannot hold what needs one.
//
// `launcher` and `bay` are deliberately left strict. A bay is a magazine that
// opens rather than a bigger rail, and nothing in the game carries ordnance
// yet - so there is no content for a relaxation to be right or wrong about,
// and writing the rule now would be writing it blind.
[[nodiscard]] constexpr bool mountAcceptsKind(MountKind mount, MountKind fitting)
{
    return mount == fitting || (mount == MountKind::Turret && fitting == MountKind::Fixed);
}

// Which mount kinds hold a `[[weapon]]` rather than a `[[component]]`. This is
// what makes a fitting's def id unambiguous: the MOUNT decides which table an
// id is looked up in, so a component and a weapon that shared an id could
// still never be confused for one another.
[[nodiscard]] constexpr bool mountTakesWeapon(MountKind kind)
{
    return kind == MountKind::Turret || kind == MountKind::Fixed || kind == MountKind::Launcher ||
           kind == MountKind::Bay;
}

struct ShipMount
{
    // Unique within the hull and STABLE: a save names a fitting by the mount it
    // occupies, never by index, so that an author inserting a mount does not
    // silently rearrange every existing player's ship (decisions/014 rule 1).
    std::string id;
    MountKind kind = MountKind::Utility;
    MountSize size = MountSize::Small;
    // decisions/014 rule 2: `at` PRESENT means external - drawn on the hull and
    // shootable where it sits - and ABSENT means internal. One authored key
    // decides both, and nothing else needs a flag. An internal mount is still
    // destructible; it is simply not aimed at.
    bool external = false;
    float at[3] = {0.0f, 0.0f, 0.0f};   // metres, hull frame (+X right, +Y up, -Z fwd)
    float aim[3] = {0.0f, 0.0f, -1.0f}; // facing, hull frame; defaults to the ship's
    float arc = 0.0f;                   // degrees of traverse; 0 = points where `aim` says
    // What the hull SHIPS WITH (Phase 31 stage B): a component or weapon def
    // id, empty for a mount that comes bare. This is where `ShipDef::weaponId`
    // went - an NPC's gun, the starter shuttle's gun, and the fit a bought
    // hull arrives with are all one key now, and it is on the mount because
    // that is the only place that can say WHICH gun goes WHERE.
    //
    // It is a default, not a constraint: the player refits freely from here.
    std::string fit;
};

// The whole fit rule for one place: kind first, then size. Both halves live in
// their own function above so a test can turn one off and see which half the
// refusal came from.
[[nodiscard]] constexpr bool mountAccepts(const ShipMount& mount, MountKind kind, MountSize size)
{
    return mountAcceptsKind(mount.kind, kind) && mountAccepts(mount.size, size);
}

// --- the hull spine (engine plan Phase 32 stage A) ---------------------------

// What a hull is FOR (gdd.md 11.2). Six families; the grid in 11.3 is this
// crossed with the class band below, and a named ship type is one cell of it.
//
// ⚑⚑ IT IS NOT `PilotRole` AND THE TWO ARE DELIBERATELY NOT UNIFIED. This says
// what the HULL WAS BUILT TO DO; `PilotRole` says what THIS INSTANCE IS DOING,
// which is why a freighter flown by a raider is a fighter in a logistics hull
// rather than a contradiction. They are different questions asked of different
// things, and Phase 32 stage D is where one is mapped onto the other.
enum class HullRole : std::uint32_t
{
    Line = 0,   // shoots things and is shot at
    Carrier,    // projects force it does not itself carry: hangars, drones
    Logistics,  // moves matter: haulers, freighters, miners, salvagers
    Support,    // makes other ships better or worse: repair, EW, command
    Covert,     // operates where it should not be
    Industrial, // builds and services
    Count,
};

inline constexpr std::size_t kHullRoleCount = static_cast<std::size_t>(HullRole::Count);

// The def spellings, and the only place they live - the same shape
// `mountKindName`/`parseMountKind` has, for the same reason.
[[nodiscard]] const char* hullRoleName(HullRole role);
[[nodiscard]] bool parseHullRole(std::string_view text, HullRole& out);

// What a hull WEIGHS (gdd.md 11.1): the scale band, the mount budget and
// roughly what it costs to keep alive, in eight steps.
//
// ⚑⚑⚑ IT IS A WORD IN THE FILE AND NOT A NUMBER, WHICH IS THE CHECKPOINT
// RULING ON STAGE A'S OWN VOCABULARY. 11.1 numbers its rows and the first cut
// of this key was `class = 3`, which reads as a magic constant in a file whose
// every other enumerated key is a word - `kind = "turret"`, `size = "medium"`,
// `region = "core"`. A number also has no wrong spelling: `class = 9` is a
// typo the schema can catch, but `class = 2` where the author meant 3 is not,
// while `"heavy"` and `"medium"` cannot be confused by a slipped digit.
//
// ⚑⚑ THE DECLARATION ORDER IS THE SIZE ORDER AND EVERYTHING BELOW DEPENDS ON
// IT - `hullClassBand` indexes the bound table by the ordinal, so moving a
// member here re-bands every hull in the game. The ordinal IS 11.1's class
// number, which is what lets the tool print "heavy (class 3)" and stay honest
// about the table it came from.
enum class HullClass : std::uint32_t
{
    Skiff = 0,    // 8-20 m
    Light,        // 20-45 m
    Medium,       // 45-120 m
    Heavy,        // 120-300 m
    Cruiser,      // 300-600 m
    Capital,      // 600-1200 m
    SuperCapital, // 1.2-3 km
    Titan,        // 3 km+
    Count,
};

inline constexpr std::size_t kHullClassCount = static_cast<std::size_t>(HullClass::Count);

// The def spellings, and the only place they live - the same shape
// `mountKindName`/`parseMountKind` has, for the same reason.
[[nodiscard]] const char* hullClassName(HullClass hullClass);
[[nodiscard]] bool parseHullClass(std::string_view text, HullClass& out);

// ⚑⚑⚑ THE BAND IS A *LENGTH* BAND, AND THAT IS THE FINDING PHASE 32's RE-READ
// TURNED ON: NOTHING IN A `ShipDef` IS A LENGTH. `model` names a mesh, `scale`
// multiplies it, and `ModelDef::radius` is a hand-authored collision SPHERE
// rather than a measurement - the shipped `ship` row says 8.0 against a mesh
// that measures 7.0064. The cooked `.smesh` header carries no bounds at all, so
// the game cannot read one either. Every number this table is compared against
// therefore has to be MEASURED off geometry, which is why the check lives in
// the Forge - the only place in this project that holds a mesh and a def at the
// same time - and not in this parser.
//
// ⚑ The band is a SOFT contract, per 11.1 in as many words: a def that violates
// its own class is "a content bug the tools should say so about, not a schema
// error". So nothing below refuses anything; it only measures.
struct HullClassBand
{
    float minLength = 0.0f; // metres, inclusive
    float maxLength = 0.0f; // metres, EXCLUSIVE - so the bands partition, and a
                            // hull measuring exactly 20 m is Light, not Skiff
};

[[nodiscard]] HullClassBand hullClassBand(HullClass hullClass);

// The class a measured length actually falls in. FALSE below the smallest band:
// under 8 m is not a small ship, it is a fitting, and answering `Skiff` would
// be this function inventing the answer the bands decline to give.
[[nodiscard]] bool hullClassForLength(float metres, HullClass& out);

// Whether a measured length sits inside the band a class declares.
[[nodiscard]] bool hullLengthInBand(HullClass hullClass, float metres);

struct ShipDef
{
    std::string id;   // stable, namespaced, e.g. "sol.shuttle"
    std::string name; // display name
    std::string model = "ship";
    // The interior drawn when the player flies THIS ship from the seat
    // (Phase 19). Empty means the `cockpit` role, which is what all three
    // shipped hulls take - so a freighter and an interceptor still share one
    // interior until somebody authors a second. Only ever consulted for the
    // player's active ship; an NPC has no seat.
    std::string cockpit;
    float scale = 1.0f;
    // ⚑⚑ WHAT THE HULL IS (gdd.md 11.1) AND WHAT IT IS FOR (11.2), BOTH
    // OPTIONAL AND BOTH WITH NO DEFAULT - which is the whole reason each
    // carries a `has` flag rather than leaning on a sentinel. `Skiff` is a real
    // class and `Line` is a real role, so there is no spare value to
    // mean "unset"; and a class this parser invented would be the schema
    // deciding a hull's weight, its mount budget and its crew, while a role it
    // invented would be the schema deciding what the ship is FOR. Absent means
    // nobody has said, which the tools report as a gap rather than a violation.
    //
    // ⚑ Nothing in the engine reads either yet, deliberately. They are the
    // vocabulary Phase 32's rosters and spawn tables are built on, and the only
    // consumer this stage ships is the Forge's band check - which is a
    // statement about content, not a rule the game enforces at runtime.
    HullClass hullClass = HullClass::Skiff;
    bool hasHullClass = false;
    HullRole role = HullRole::Line;
    bool hasRole = false;
    ShipFlightTuning flight;
    ShipDefenseTuning defense;
    ShipPowerTuning power;
    float cargoCapacity = 50.0f; // trade goods, units
    // Scanning (engine plan Phase 8e): how far a pulse reaches and how fast a
    // target scan resolves. Scanner components move both like any other stat.
    float scanRange = 2.5e8f; // meters
    float scanSpeed = 1.0f;   // target-scan progress multiplier
    // Mining (engine plan Phase 8f): how far loose ore is drawn in from.
    // Deliberately far shorter than a mining beam reaches, so an unfitted ship
    // has to fly in and scoop what it cut; a collector rig is what buys the
    // right to sit still and mine.
    float collectorRange = 120.0f; // meters
    // ⚑⚑ HOW LOUD THIS HULL IS (engine plan Phase 36 stage E), and 1.0 is
    // "ordinary" rather than "none": every stat above is a quantity a ship has
    // and this is a RATIO against the hull nobody has tuned. It is authorable
    // so that a covert hull can be quiet before a single component is bought -
    // no shipped hull uses that, deliberately (Phase 32 ruling 11 left
    // `HullRole` unread vocabulary and this phase ships covert COMPONENTS, not
    // covert hulls), but the key is what makes the eventual family a data pass.
    float signature = 1.0f;
    // Outfitting (engine plan Phase 8a): hull price, fitting budgets, slots.
    float price = 10'000.0f;
    float mass = 10'000.0f;    // kg; component mass dilutes accelerations
    float powerOutput = 10.0f; // fit budget: sum of component power_draw
    std::uint32_t crewBerths = 1;
    // Where fittings go, and since Phase 31 stage B the ONLY place they go.
    // Authored order, which is the order the outfitting screen and the Forge's
    // list show. The four `slots_*` counts and `weapon` that used to live here
    // are gone: a count cannot say which KIND of kit a hull takes or how big,
    // which is the whole of decisions/014.
    //
    // EMPTY IS LEGAL AND NOW MEANS SOMETHING: a hull with no mounts fits
    // nothing and flies unarmed. That is a mod's ship def written before
    // mounts existed, and it loads rather than refusing - the def layer's
    // standing rule is that a missing key is a default, not an error.
    std::vector<ShipMount> mounts;
    CatalogGate gate;
    std::string source; // document that last defined this id (diagnostics)

    // The mount an id names, or null. Linear over a list that is a dozen long
    // at most - a hull with enough mounts for a map to pay off is a hull class
    // gdd.md 11.1 does not have.
    [[nodiscard]] const ShipMount* findMount(std::string_view mountId) const;
};

// An outfitting component: occupies one mount, costs power from the ship's
// budget, adds mass, and modifies stats (engine plan Phase 8a; mounts in 31B).
//
// `mount` and `size` replaced the four-value `slot` enum, and the pair is what
// a count could never say: WHICH kind of place this goes in and how big a one
// it needs. Both are required of an author for the same reason a mount's are -
// a kind this parser invented is kit silently accepted by a hull that never
// said it takes it, and a size it invented is the fitting budget written by
// the parser rather than by the person balancing the ship.
struct ComponentDef
{
    std::string id;
    std::string name;
    MountKind mount = MountKind::Utility;
    MountSize size = MountSize::Small;
    // What it is drawn as standing in an EXTERNAL mount (Phase 31 stage E), on
    // the same terms as `WeaponDef::model`: empty means not drawn, and the mesh
    // is authored at real size and drawn at the hull's scale. An internal mount
    // draws nothing whatever this says - that is `decisions/014` rule 2, and it
    // is the MOUNT's decision rather than the kit's.
    std::string model;
    float price = 100.0f;
    float mass = 0.0f;      // kg
    float powerDraw = 0.0f; // against ShipDef::powerOutput
    StatModifiers modifiers;
    CatalogGate gate;
    std::string source;
};

// A hireable crew member (decisions/006: trivial passive bonuses): occupies a
// berth, costs a one-time hire fee, and modifies stats like a component without
// mass or power draw.
struct CrewDef
{
    std::string id;
    std::string name;
    std::string role; // display flavor, e.g. "Engineer"
    float price = 200.0f;
    StatModifiers modifiers;
    CatalogGate gate;
    std::string source;
};

struct WeaponDef
{
    std::string id;
    std::string name;
    std::string kind; // "projectile" | "hitscan" - how it TRAVELS, not where it sits
    // Where it goes (Phase 31 stage B). `mount` must be one of the four
    // weapon-taking kinds (`mountTakesWeapon`); a gun authored `fixed` also
    // fits a `turret`, which is `mountAcceptsKind`'s one asymmetry and is why
    // the four shipped guns did not have to be authored twice.
    MountKind mount = MountKind::Fixed;
    MountSize size = MountSize::Small;
    float damage = 0.0f;
    float rateOfFire = 1.0f;      // shots/s
    float range = 1'000.0f;       // meters
    float projectileSpeed = 0.0f; // m/s; 0 for hitscan
    float energyCost = 10.0f;     // capacitor draw per shot
    // Mining (engine plan Phase 8f): yield units cut out of a rock or wreck
    // per second of held beam. 0 leaves a weapon a weapon — a mining laser is
    // an ordinary hardpoint choice, not a mode.
    float miningPower = 0.0f;
    float price = 500.0f; // shipyard price (Phase 8a outfitting)
    // ⚑⚑ WHAT THE GUN ITSELF IS DRAWN AS, STANDING IN ITS MOUNT (Phase 31
    // stage E). Empty means it is NOT drawn, which is what every weapon in this
    // game did before the key existed - so a mod's gun that names no mesh still
    // fits and still fires, and leaves the hardpoint bare rather than sprouting
    // a fallback box on somebody else's ship. A name that resolves to nothing
    // is a different case and falls back to the `fitting` role, because that
    // one is an author's mistake and should be visible.
    //
    // ⚑ IT IS AUTHORED AT REAL SIZE AND DRAWN AT THE HULL'S SCALE, exactly as
    // `at` is - so one turret mesh reads as a light ring on a shuttle and a
    // heavy one on a `scale = 4.0` freighter. The mount's `size` does NOT
    // multiply it: a size-to-metres table would be a second opinion about how
    // big a `medium` gun is, and the mesh is already the first one.
    std::string model;
    // What its BOLT is drawn as (Phase 19). Empty means the `bolt` role. A
    // hitscan weapon spawns no projectile and ignores it.
    //
    // ⚑ IT WAS SPELLED `model` UNTIL PHASE 31 STAGE E, and moved to make room
    // for the key above under the spelling `CommodityDef` already uses for the
    // same shape: `model` is the thing itself, `<derived>_model` is what it
    // produces. No shipped weapon set it, so the rename cost no content.
    //
    // ⚑ Under the unit-radius contract: a bolt is drawn at 0.3 x 0.3 x 4 m,
    // so the model must be authored at radius 1.
    std::string boltModel;
    CatalogGate gate;
    std::string source;
};

enum class FactionKind : std::uint32_t
{
    Major = 0, // authored territory claimant (generator capital)
    Pirate,    // clan template; instantiated per lawless fringe neighborhood
};

// One side's declared initial standing toward another faction ("id:standing"
// strings in TOML, -100..100). The matrix is symmetric: a pair declared from
// both sides must agree (validateFactions).
struct FactionRelation
{
    std::string otherId;
    float standing = 0.0f;
};

// How strongly a faction favours one station archetype in the territory it
// holds ("sol.station_factory:2.5" in TOML), Phase 13.
//
// A MULTIPLIER on the archetype's region weight rather than a replacement for
// it, so the economy's regional logic — ore in the fringe, factories in the
// core — still sets the baseline and a faction only tilts it. 0 is a legal and
// deliberate "never builds one"; the default, for an archetype a faction says
// nothing about, is 1.0.
struct StationBias
{
    std::string stationId;
    float weight = 1.0f;
};

// ⚑⚑⚑⚑ THE THREE ROSTER CELLS, AS A VOCABULARY (Phase 32 stage C), AND THE
// REASON THEY NEEDED ONE IS THAT AN EMPTY CELL ALREADY MEANT SOMETHING - FOUR
// SOMETHINGS. Before this stage `ships_raider = []` meant "fly the patrol
// roster instead" where a contested system spawns an attacker, `ships_trader =
// []` meant "haul in the patrol roster instead" at both trader sites, and
// `ships_patrol = []` meant "fly the raider roster" for a response wing - while
// everywhere else it meant "spawn nothing, silently". So the format had no way
// to say *we build none of those*: every spelling of it was already taken by a
// substitution rule written out by hand at each site.
//
// ⚑⚑ AND IT IS A SEPARATE KEY RATHER THAN AN EMPTY LIST, WHICH IS STAGE A's
// RULING APPLIED AGAIN. There the class became a WORD because a number in a
// file has no wrong spelling; here "we build none" becomes a THING THE AUTHOR
// WRITES, because deleting a line and emptying a list are a one-character
// difference in meaning that no schema can catch and no reader can see. The
// same file already says this about stations, in its own header: for
// `station_bias`, "0 means this faction never builds one at all". A roster had
// no equivalent until now.
//
// ⚑ A cell named here AND populated is refused at parse rather than resolved by
// precedence, the same bargain a `[[model]]` carrying both a material and the
// four surface keys makes.
enum class RosterCell : std::uint32_t
{
    Patrol = 0, // ships_patrol - what this faction polices its space with
    Raider,     // ships_raider - what it sends to take someone else's
    Trader,     // ships_trader - what its haulers fly
    Count,
};

inline constexpr std::size_t kRosterCellCount = static_cast<std::size_t>(RosterCell::Count);

// The def spellings, and the only place they live - the same shape
// `hullClassName`/`parseHullClass` has, for the same reason.
[[nodiscard]] const char* rosterCellName(RosterCell cell);
[[nodiscard]] bool parseRosterCell(std::string_view text, RosterCell& out);

struct FactionDef
{
    std::string id;
    std::string name;
    std::string description;
    float color[3] = {1.0f, 1.0f, 1.0f}; // sRGB accent for HUD/markers
    FactionKind kind = FactionKind::Major;
    // Personality weights (0..1) consumed by the Lua decision rules
    // (Phase 8b faction sim): raid appetite and drift-back-to-baseline rate.
    float aggression = 0.5f;
    float forgiveness = 0.5f;
    std::vector<FactionRelation> relations;
    std::vector<std::string> shipsPatrol; // ship def ids for ambient wings
    std::vector<std::string> shipsRaider;
    // Hulls this faction's haulers fly (Phase 8x). A trader puppet is a body
    // for a coarse EconomyTrader, and it needs a roster for the same reason a
    // patrol wing does: the def decides what the player sees coming.
    std::vector<std::string> shipsTrader;
    // ⚑⚑ THE CELLS THIS FACTION DECLARES IT BUILDS NOTHING FOR (Phase 32 stage
    // C): `builds_no = ["trader"]`. An unset bit is NOT the same as an empty
    // roster - see `RosterCell` above. Unset means "unspecified", and every
    // site's own substitution rule still applies to it; set means the author
    // said so, and nothing is ever substituted for a cell a faction declared it
    // does not field.
    bool buildsNo[kRosterCellCount] = {false, false, false};
    // What this faction BUILDS where it holds territory (Phase 13). Empty —
    // the default — means it builds to the region's baseline like everyone
    // else, which is what every faction did before this existed.
    //
    // ⚑ These descriptions have asserted economic characters since 8b — an
    // "authoritarian industrial bloc", "traders and haulers", "settler
    // militias" — and the galaxy generator read none of it. This is the key
    // that turns that prose into a number.
    std::vector<StationBias> stationBias;
    // ⚑⚑⚑ WHAT THIS FACTION'S LAW SAYS ABOUT WHAT YOU ARE CARRYING (gdd.md 13,
    // Phase 33 stage D). Commodity ids: `contraband` is forbidden outright,
    // `restricted` is licensed - carriable, and a thing a patrol will want to
    // see papers for. Both empty - the default - is a jurisdiction with no
    // opinion, which is a REAL answer and not a missing one: gdd.md 13 says
    // "the same crate of stims is ordinary medicine in the Compact", and the
    // way a def says that is by not mentioning it.
    //
    // ⚑⚑ THE LISTS LIVE HERE AND NOT ON `CommodityDef`, WHICH IS THE WHOLE
    // POINT OF THE FEATURE. "Cargo is never intrinsically illegal;
    // jurisdictions are." A `contraband = true` on a good would have been the
    // schema deciding that a crate means the same thing everywhere, which is
    // exactly the thing this game is about not being true. `CommodityTier`
    // records the absence for the same reason.
    std::vector<std::string> contraband;
    std::vector<std::string> restricted;
    std::string source;
};

// What a jurisdiction says about one good (Phase 33 stage D).
//
// ⚑⚑⚑ `Unpoliced` IS NOT `Legal`, AND KEEPING THEM APART IS THE POINT. A
// system nobody holds has no legality TABLE - not an empty one - so the honest
// answer there is "nobody here has an opinion", which is a different fact from
// "the faction that holds this place has considered it and does not mind".
// Phase 37's shadow faction needs exactly that difference, and a bool would
// have thrown it away before anybody noticed it was there.
//
// ⚑ In the shipped galaxy that answer is reachable at exactly ONE dock. Every
// procedurally lawless neighbourhood is swept up by `spawnClans`, so the only
// system nobody owns is the authored `sol.lantern` - and a pirate clan holding
// a system IS a jurisdiction, one that happens to have no table, which is why
// clan space is where a smuggler breathes out.
enum class Legality : std::uint32_t
{
    Unpoliced = 0, // nobody holds this place; there is no table to consult
    Legal,         // the holder has a table and this is not on it
    Restricted,    // licensed: carriable, and a patrol will want papers
    Contraband,    // forbidden outright
};

[[nodiscard]] const char* legalityName(Legality legality);

// What one faction's law says about one commodity id. Never answers
// `Unpoliced` - that is a fact about a SYSTEM, not about a faction, and the
// caller who knows which system is the one that can say it.
[[nodiscard]] Legality factionLegalityOf(const FactionDef& faction, std::string_view commodityId);

// What a hold can hold (gdd.md §12's Storage family: bulk, cryo, hazardous).
//
// ⚑⚑ A GOODS CLASS IS NOT A MATERIAL TIER AND THE TWO MUST NOT BE MERGED.
// `CommodityTier` says where a good sits in the tree that MAKES it; a goods
// class says what it takes to keep it in one place. Ore and hull plate are
// different tiers and the same bulk problem.
//
// ⚑⚑ STAGE D SUPPLIED THE OTHER HALF AND THAT IS WHY THIS ENUM SITS HERE RATHER
// THAN DOWN WITH THE MODULES IT ARRIVED WITH. Stage A wrote "which class each
// commodity is does not exist yet: it is a key on `[[commodity]]` and it belongs
// to stage D, where the class is first read". It exists now, so the enum has two
// clients on opposite sides of the file - `CommodityDef::goodsClass` says which
// class a good IS, `ModuleStorage` says how much of that class a hold takes -
// and the pair of them is the whole mechanism: **a station cannot hold what it
// has no hold for, including contraband.**
enum class GoodsClass : std::uint32_t
{
    Bulk = 0,   // ore, plate, sections: cheap, heavy, wants volume
    Cryo,       // foodstuffs and anything that spoils
    Hazardous,  // reactive, radioactive, or the sort of thing a patrol asks about
    Count,
};

inline constexpr std::size_t kGoodsClassCount = static_cast<std::size_t>(GoodsClass::Count);

[[nodiscard]] const char* goodsClassName(GoodsClass goods);
[[nodiscard]] bool parseGoodsClass(std::string_view text, GoodsClass& out);

// Where a good sits in the material tree (gdd.md 6, Phase 33 stage B).
//
// ⚑⚑⚑ A WORD IN THE FILE, NOT A NUMBER, WHICH IS PHASE 32 STAGE A'S CHECKPOINT
// RULING APPLIED TO THE SECOND VOCABULARY THAT ASKED FOR IT. gdd.md 6 numbers
// its rows T0..T3 and `tier = 2` would read as a magic constant in a file whose
// every other enumerated key is a word - and, worse, a slipped digit between
// `1` and `2` is a typo no schema can catch while `refined` and `component`
// cannot be confused.
//
// ⚑⚑⚑ AND UNLIKE `HullClass`, THE ORDINAL IS NOT AN ORDER. The first four ARE
// the T0..T3 steps and each is made from the one before it, but `Consumer` is
// not a fifth step: foodstuffs and medical supplies are made from organics at
// T0, and nothing is ever built out of them. Anything that compares two tiers
// with `<` is therefore wrong about consumer goods, which is why this enum has
// no band table, no bounds and no `tierBelow` - it says what a good IS, and the
// production graph in `stations.toml` says what it comes from.
//
// ⚑ Contraband is deliberately absent. gdd.md 13: "Contraband is not a tier;
// it is a LEGALITY, and the same crate is cargo in one jurisdiction and a crime
// in the next." That lives on a faction (Phase 33 stage D), never on the good.
enum class CommodityTier : std::uint32_t
{
    Raw = 0,   // T0: ores, ices, gases, silicates, organics, and salvage
    Refined,   // T1: alloys, concentrates, fuel, polymers, reclaimed alloy
    Component, // T2: what a mount fitting is made of (gdd.md 11.5)
    Assembly,  // T3: ship and station module kits, hull and drive sections
    Consumer,  // food, medicine, textiles, luxuries - a branch, not a step
    Count,
};

inline constexpr std::size_t kCommodityTierCount = static_cast<std::size_t>(CommodityTier::Count);

// The def spellings, and the only place they live - the same shape
// `hullClassName`/`parseHullClass` has, for the same reason.
[[nodiscard]] const char* commodityTierName(CommodityTier tier);
[[nodiscard]] bool parseCommodityTier(std::string_view text, CommodityTier& out);

struct CommodityDef
{
    std::string id;
    std::string name;
    float basePrice = 10.0f; // credits/unit at neutral stock
    // How often asteroids in each region tier are made of this (Phase 8f).
    // All zero — the default — means it is not something you mine.
    float oreWeightCore = 0.0f;
    float oreWeightFrontier = 0.0f;
    float oreWeightFringe = 0.0f;
    // What a rock of this ore, and a chunk cut out of one, are drawn as
    // (Phase 19). Empty means the `rock` and `ore_chunk` roles, so adding
    // these changed nothing for the shipped commodities. Only a commodity
    // with an ore weight is ever mined, so on anything else they are inert.
    //
    // ⚑ Both are under the unit-radius contract: a rock's instance scale IS
    // its radius in metres and a chunk is drawn at 6 m, so a model of any
    // other radius silently resizes every instance AND its mining hit sphere.
    std::string model;
    std::string chunkModel;
    // Which tier of the material tree this good is (Phase 33 stage B).
    //
    // ⚑⚑ OPTIONAL AND NOT DEFAULTED, for `ShipDef::hullClass`'s reason: `Raw`
    // is a real answer and cannot double as "nobody said". A parser that
    // picked one would be answering gdd.md 6 on the author's behalf, and the
    // whole value of the key is that somebody decided.
    //
    // ⚑ Nothing in the sim reads it yet, deliberately - it is vocabulary for
    // the salvage roll (stage C) and the legality table (stage D), and the only
    // thing that reads it in stage B is the validator below.
    CommodityTier tier = CommodityTier::Raw;
    bool hasTier = false;
    // What it takes to keep this good in one place (Phase 34 stage D), and the
    // half of `GoodsClass` that did not exist when the enum was written: stage A
    // said in as many words that "which class each commodity is does not exist
    // yet: it is a key on `[[commodity]]` and it belongs to stage D, where the
    // class is first read". This is that key.
    //
    // ⚑⚑ OPTIONAL, AND UNLIKE `tier` IT IS READ AS A DEFAULT RATHER THAN LEFT
    // UNANSWERED — the two differ because "unsaid" means something different in
    // each. A good can genuinely sit outside the material tree, so `hasTier`
    // stays a real third state; but every physical good has to be kept
    // SOMEWHERE, so there is no station behaviour that "nobody said" could
    // describe. The composer reads an unclassed good as `Bulk`, which is the
    // class that means "the warehouse", and the flag is kept so that a reader
    // which wants to know whether an author decided still can.
    //
    // ⚑ Bulk rather than nothing, deliberately: a good no hold admits would
    // vanish from every market in the galaxy, which is the silent-disappearance
    // failure this project has now named three times. A mod author who forgets
    // the key gets a good that is stocked everywhere — visible, and harmless.
    GoodsClass goodsClass = GoodsClass::Bulk;
    bool hasGoodsClass = false;
    std::string source;
};

// A sound cue (Phase 8t): what the game asks for by id, pointing at a cooked
// .saud by name. Data rather than an enum so a cue can be retuned or replaced
// on a script reload, and so a mod layer can override one without a rebuild.
struct SoundDef
{
    std::string id;
    std::string asset; // cooked file stem, e.g. "weapon_fire" -> weapon_fire.saud
    float gain = 1.0f;
    // Playback rate is jittered by +-pitchJitter around 1, so a cue fired
    // repeatedly (a gun, a mining beam) does not comb-filter against itself.
    float pitchJitter = 0.0f;
    // Meters at which a positional voice has fallen to half gain. Ignored
    // when the cue is played 2D.
    float rolloff = 500.0f;
    // Concurrent voices of this cue; 0 is unlimited. A cap keeps one chattering
    // source from crowding the whole mix out.
    std::uint32_t maxInstances = 0;
    std::string source;
};

// "no material", the value a model's resolved index holds until the merged
// database has been walked. Namespace scope rather than a `DefDatabase`
// static because `ModelDef` carries one and is declared first.
inline constexpr std::uint32_t kNoMaterial = 0xFFFFFFFFu;

// How colour leaves a material's fragment shader and reaches the target.
// ⚑ A MIRROR OF `rhi::BlendMode` AND DELIBERATELY NOT THAT TYPE: `sol::assets`
// does not depend on `sol::rhi`, and a def layer that included Vulkan headers
// to name three states would be the wrong end of that dependency. The mapping
// is one switch in `MaterialRegistry`, and a test asserts it stays total.
enum class MaterialBlend
{
    Opaque,
    Alpha,    // premultiplied: src.rgb + dst.rgb * (1 - src.a)
    Additive, // src.rgb + dst.rgb
};

// One extra texture a material declares beyond its albedo (Phase 25 stage C).
//
// ⚑ THE ALBEDO IS NOT ONE OF THESE, and that asymmetry is deliberate rather
// than a leftover. `texture` is the one every mesh material has, it is bound at
// set 0 exactly as it has been since Phase 3, and keeping it there is what lets
// `mesh.frag`, `membrane.frag` and the Forge's whole viewport stay untouched by
// this stage. These are the material's OWN slots, bound at set 1 in the order
// written, which is the order the shader must declare them in.
struct MaterialSlot
{
    std::string name;    // the shader's variable name, for an error a reader can act on
    std::string texture; // cooked file stem, e.g. "cockpit_glow" -> cockpit_glow.stex
};

// One scalar a material tunes on its shader (Phase 25 stage C). Matched into
// the shader's uniform block BY NAME, never by position - see `spirv_reflect`
// for why position is the silent-misalignment bug this exists to avoid.
struct MaterialParam
{
    std::string name;
    float value = 0.0f;
};

// HOW A SURFACE IS DRAWN (Phase 25 stage A), split out of the model that wears
// it, WHAT DRAWS IT (stage B), and WHAT THAT SHADER GETS FED (stage C).
//
// ⚑ THE PREFIX `sol.auto.` IS RESERVED and an authored row using it is
// refused. Every `[[model]]` row that names no material gets one SYNTHESISED
// from its own `texture`/`emissive`/`translucent`/`alpha` under that prefix,
// which is what lets every committed def file keep working untouched while the
// renderer stops reading a model's fields at all.
struct MaterialDef
{
    std::string id;
    std::string texture; // cooked file stem, e.g. "hull" -> hull.stex
    // Unlit albedo glow added at draw time; see `ModelDef` for what it is for.
    float emissive = 0.0f;
    // Phase 12's alpha-blended pipeline, recorded after the sky. `alpha` is
    // coverage in 0..1 and is only meaningful when `translucent` is set; the
    // opaque path passes 1.0, which the shader premultiplies by and so leaves
    // its output unchanged.
    bool translucent = false;
    float alpha = 1.0f;

    // ⚑⚑ PHASE 25 STAGE B: WHAT DRAWS IT. SPIR-V stems, so "mesh" is
    // mesh.vert.spv and mesh.frag.spv, found on the shader search path. Two
    // keys rather than one because a material may keep the stock vertex stage
    // and bring only its own fragment stage, which is the common case.
    std::string vertexShader = "mesh";
    std::string fragmentShader = "mesh";

    // ⚑⚑ PIPELINE STATE, AND ITS DEFAULTS ARE PHASE 12's HARDCODED VARIANT.
    // `mesh_renderer.cpp` used to build the translucent pipeline as "the same
    // shaders, layout and vertex format with three fields moved"; those three
    // fields are these, and `translucent` now SEEDS them rather than deciding
    // them. So a row that says nothing draws exactly as it always did, and a
    // row that wants blending without giving up its depth write can say so.
    //
    // ⚑ `translucent` still decides which PASS a draw is recorded in - after
    // the sky rather than in the opaque block - because that is a fact about
    // the frame, not about the pipeline. Blending in the opaque block would be
    // painted over by the sky, which reads as broken blending and is in fact a
    // misplaced pass.
    MaterialBlend blend = MaterialBlend::Opaque;
    bool depthTest = true;
    bool depthWrite = true;
    bool cullBackFaces = true;

    // ⚑⚑ PHASE 25 STAGE C: WHAT THE SHADER GETS FED, DECLARED RATHER THAN
    // REFLECTED (decision 2). `slots` are extra textures at set 1 bindings
    // 0..n-1 in written order; `params` are floats packed into a uniform buffer
    // at the binding after them. The declaration is what the descriptor layout
    // is BUILT from; the SPIR-V is what it is CHECKED against, so a material
    // that lies about its shader is refused at load with the slot named
    // instead of reaching the validation layer - which is on in dev builds and
    // off in shipping, i.e. exactly backwards from where it is needed.
    //
    // ⚑ A material with neither declares no set 1 at all and gets the same
    // pipeline layout every material had before this stage. That is what keeps
    // the other eight rows free.
    std::vector<MaterialSlot> slots;
    std::vector<MaterialParam> params;

    // True for a row this database derived from a `[[model]]`, false for one
    // somebody wrote. The derived set is rebuilt from scratch after every
    // merge, so a later layer editing a model row cannot leave a stale one
    // behind, and nothing outside this file has to remember to ask for it.
    bool synthesised = false;
    std::string source;
};

// A drawable model (Phase 9): the cooked mesh and texture the renderer binds,
// and the figures the sim measures the thing by. Data rather than an enum
// because until this existed the whole set was a five-member C++ enum behind
// four hardcoded switches - so a mesh could be authored and cooked and still
// have no way of reaching the game. `[[model]]` rows are what the authoring
// tool writes; the def order is the runtime model index.
struct ModelDef
{
    std::string id;
    std::string mesh; // cooked file stem, e.g. "ship" -> ship.smesh
    // ⚑ Phase 25 stage A: `material` names a `[[material]]` row, and a row
    // that names one carries NONE of the four surface keys below - they are
    // that row's business now, and a model carrying both is refused at load
    // rather than resolved by a precedence rule nobody can see in the file.
    // A row that names no material gets one synthesised from the four, so the
    // committed catalog and every mod written before this stage keep working
    // exactly as written.
    std::string material;
    // The index into `materials()` this row resolved to, or `kNoMaterial`
    // while the named row has not been merged yet. `validateMaterials` is what
    // turns "still unresolved once every layer is in" into a refusal.
    std::uint32_t materialIndex = kNoMaterial;
    std::string texture; // cooked file stem, e.g. "hull" -> hull.stex
    // Radius in meters at instance scale 1; the sim multiplies by the scale
    // for collision and weapon hit tests. A model authored at unit radius
    // (the asteroid) therefore takes its size from the scale, and one
    // authored at its real size (the ship, the station) is drawn at scale 1.
    float radius = 1.0f;
    // What NPC steering and autopilot dodge, meters at scale 1; 0 means "the
    // same as radius". A station's is deliberately wider than what you can
    // hit, because 8r's berths ring at 200 m and that approach was tuned
    // against the wider figure - larger than what you can hit is always safe.
    float avoidRadius = 0.0f;
    // Unlit albedo glow added at draw time. Vacuum ambient is 1.2%, which is
    // right for a hull and pitch black for a room the player is sitting in.
    float emissive = 0.0f;
    // Whether the model blocks anything at all. A gate is a doorway you fly
    // through (Phase 8w), and before this field that fact was expressed as
    // "the only Cube left among statics" - which would have silently
    // un-solidified any future Cube-shaped static, as 8w's own gaps recorded.
    bool solid = true;
    // Phase 12: drawn through the alpha-blended pipeline instead of the opaque
    // one, and recorded after the sky. Declared on the MODEL rather than on
    // the instance so that the next translucent thing in this game is a def
    // row and no C++ at all. `alpha` is coverage in 0..1 and is only
    // meaningful when `translucent` is set; the opaque path passes 1.0, which
    // the shader premultiplies by and so leaves its output unchanged.
    bool translucent = false;
    float alpha = 1.0f;
    std::string source;
};

// A SLOT the engine draws into, and the model that fills it (Phase 19).
//
// Stage A of Phase 9 replaced the ModelId enum with a def lookup and left a
// second wall standing: the only model resolution that read data was a ship
// def's, and every other thing in the world - the gate, its membrane, a rock,
// an ore chunk, a bolt, the cockpit - was fetched by a string literal compiled
// into C++. A `[[role]]` row is how those names leave the source.
//
// ⚑ The ROLE ids stay in C++ and only the ANSWERS move out, which is the
// distinction the whole phase turns on: the set of slots is a property of the
// engine (it either draws a cockpit or it does not), while what fills one is a
// property of the content. See `game/src/model_roles.hpp` for the list this
// game asks for.
//
// ⚑ It is an id-keyed ARRAY rather than a singleton `[world]` table because a
// singleton does not parse - `mergeToml` requires an array of tables - and
// because `id` is what `mergeDef` merges on, so a mod re-points a role exactly
// the way it re-points a ship, with no merge rule invented for the occasion.
struct RoleDef
{
    std::string id;    // the slot, e.g. "gate"; must be one the engine asks for
    std::string model; // a `[[model]]` id; must exist, checked at load
    std::string source;
};

// Somebody who is actually in the room (Phase 35 stage C): a named person the
// generator seats at one station in the galaxy, who is still there on a later
// visit and who remembers whether you have been in before.
//
// ⚑⚑⚑⚑ THIS KIND IS THE *AUTHORED* HALF AND ONLY THE AUTHORED HALF, AND
// NAMING THAT LINE BEFORE THE PASS BEGINS IS WHAT PHASE 32's RECORD ASKS FOR.
// Every room in the galaxy has somebody behind the bar - that person's name is
// generated from syllables the way a system's name has been since Phase 6, and
// their trade is read off the station's own composition, so the engine half
// costs no writing at all and 62 rooms of 62 have a face. A `[[character]]` row
// is the other half: a person somebody wrote, with lines somebody wrote, in one
// place in the galaxy. The spec's own risk note is the reason the split exists -
// *"a character with one line is worse than no character"* - and a cast of six
// written properly is worth more than sixty rows of filler.
//
// ⚑⚑ THE ANCHORS ARE A PREFERENCE OVER SEATS, NOT A COORDINATE, and every one
// of them was measured before it was offered. Over the 62 rooms at seed 1701:
// by owner, Freight Guild 19, Ironstar Hegemony 12, Solar Navy 5, Helios
// Ascendancy 5, Frontier Compact 2 and 18 in clan space; by region, core 12,
// frontier 26, fringe 24; by archetype, 1 (Foundry) to 10 (Agricultural); by
// room, Bar 50, Restaurant 10, Concourse 1, Resort 1. ⚑ And the one that has to
// be said out loud because it is the obvious idea and the galaxy will not hold
// it: `shadow = true` selects **two rooms in the entire galaxy**, because only 2
// of the 62 docks with a room also carry a black-market module. It is supported,
// it is not a bug, and a cast that spends two rows on it has spent them all.
//
// An anchor that is left out means "anywhere"; several are ANDed. Ids are
// checked by `validateCharacters`, which refuses, for `validateStationRecipes`'
// reason: an anchor naming a faction that does not exist selects nothing, and a
// character who is nowhere in the galaxy is indistinguishable from one nobody
// wrote.
struct CharacterDef
{
    std::string id;
    std::string name;  // what the room calls them
    std::string trade; // what they do, one short noun phrase
    // Anchors. Each is optional; each narrows the seats this person will take.
    std::string factionId;   // a `[[faction]]` id: sits in that faction's space
    std::string archetypeId; // a `[[station]]` id: sits on that kind of dock
    std::string moduleId;    // a `[[module]]` id: sits in that kind of room
    std::string region;      // "core" | "frontier" | "fringe"
    bool lawless = false;    // sits under a pirate clan's law rather than a major's
    bool shadow = false;     // sits on a dock with a black-market module (2 seats)
    std::string source;
};

// One production or consumption line on a station ("sol.food:0.5" in TOML).
struct StationRate
{
    std::string commodityId;
    float rate = 0.0f; // units/s
};

// One line of a station's recipe ("sol.mod_bar:0.4" in TOML): a module, and
// the chance a composed station has it (Phase 34 stage B).
//
// ⚑⚑ A CHANCE RATHER THAN A COUNT, AND THE ARITHMETIC IS WHY. The composer has
// to reproduce the archetype's authored rate lists IN EXPECTATION - that is the
// whole contract of the decomposition - and expectation over a chance is one
// multiplication: a module rated 0.05 at chance 0.7 contributes 0.035, which is
// checkable against `stations.toml`'s own number by a test rather than by an
// argument. A module that wants to be there twice says so in its own rates, the
// same rule `stores` follows.
struct StationModuleEntry
{
    std::string moduleId;
    float chance = 1.0f; // (0, 1]; 1.0 means every station of this archetype
};

// A station archetype: how often the galaxy generator places it per region
// tier, and what its market produces/consumes (Phase 7 economy).
struct StationDef
{
    std::string id;
    std::string name;
    // Which `[[model]]` this archetype is drawn as (Phase 9 stage H).
    //
    // ⚑ Before this key existed a station's LOOK was C++: `space_world.cpp`
    // resolved `modelByName("station")` once and handed the same model to every
    // station in the galaxy, so an authored station mesh had no way of reaching
    // the game at all. Stage A took `ModelId` out of the enum and left it a
    // hardcoded NAME, and this is the first of those names to become data.
    // The default is the model every archetype already drew, so it changes
    // nothing until somebody authors a second one.
    std::string model = "station";
    float weightCore = 1.0f;
    float weightFrontier = 1.0f;
    float weightFringe = 1.0f;
    std::vector<StationRate> produces;
    // Upkeep (Phase 8g): burned regardless of what the station makes, clamped
    // at zero, and it never gates production. A hungry station is a hook for a
    // later unrest mechanic, not a production stall.
    std::vector<StationRate> consumes;
    // Feedstock (Phase 8g): the thing the station transforms. Production is
    // throttled by how much of it is on hand, so a refinery with no ore makes
    // no metal. Kept separate from `consumes` because the production graph is
    // a cycle (food -> ore -> metal -> machinery -> food) and gating on every
    // input would let one empty link seize the whole galaxy permanently.
    std::vector<StationRate> feedstock;
    // Where the archetype's output comes from (Phase 8g). Empty — the
    // default — means it is made from stocked feedstock like everything else.
    // "field" means it comes out of the ground: a Mining Outpost draws ore
    // from the asteroid fields in its own system, so a system with no rock
    // supports no mine.
    std::string producesFrom;
    float stockCapacity = 1'000.0f; // per commodity
    // Refinery service (Phase 8f): the archetype takes refine_input off the
    // player's hands and hands back refine_output later. Both empty — the
    // default — means the station refines nothing.
    std::string refineInput;
    std::string refineOutput;
    // The recipe: which modules this archetype is composed of, and how often
    // (Phase 34 stage B). Empty - the default - means the archetype's own rate
    // lists above are what its stations run on, which is what every station in
    // the galaxy was before stage B and what a mod's archetype still is.
    std::vector<StationModuleEntry> modules;
    std::string source;
};

// ---------------------------------------------------------------------------
// Station modules (Phase 34 stage A). gdd.md §12: a station is a list of
// modules, and what it produces, holds, offers and eventually looks like all
// follow from that list. The generator composes NPC stations from this
// vocabulary in v1 (`decisions/016`); the player builds from the same words in
// v2.
//
// ⚑⚑⚑ NOTHING IN THE ENGINE READS A MODULE YET, AND THAT IS THE STAGE, NOT AN
// OVERSIGHT. Stage A writes the vocabulary down and stops. Its readers arrive
// in order: the composer reads `powerOutput`/`powerDraw` and the rate lists in
// stage B, the dock screen reads `screens` in stage C, and the market reads
// `stores` in stage D. ⚑ The precedent that makes naming this worth it is
// Phase 32's `role` (vocabulary with no engine reader, deliberate and
// recorded) and Phase 33 stage A's inert commodity, which sat at exactly half
// capacity forever and passed every band the economy's own exit test checks.
// A word with no reader is only honest while somebody has written down when
// its reader arrives.
// ---------------------------------------------------------------------------

// gdd.md §12's eight families. The family is what a module is FOR, and it is
// the only field of a module that another module's validity depends on: a
// composition is legal only when its draw is covered by its own `Power` rows.
enum class ModuleFamily : std::uint32_t
{
    Power = 0,  // solar, fission, fusion, capacitors - the budget
    Habitat,    // rings, barracks, medical - population, and therefore appetite
    Storage,    // holds, per goods class
    Industry,   // the production chains of gdd.md §6
    Commerce,   // the Trade, Outfitting and Shipyard screens
    Recreation, // where people are, which is where rumours are (Phase 35)
    Services,   // the remaining dock screens
    Shadow,     // black-market services, on stations of ANY owner (Phase 37)
    Count,
};

inline constexpr std::size_t kModuleFamilyCount = static_cast<std::size_t>(ModuleFamily::Count);

// The def spellings, and the only place they live - the shape
// `commodityTierName`/`parseCommodityTier` already has.
[[nodiscard]] const char* moduleFamilyName(ModuleFamily family);
[[nodiscard]] bool parseModuleFamily(std::string_view text, ModuleFamily& out);

// A dock screen a module offers. gdd.md §12's dividend - "a mining outpost with
// no market floor has no Trade tab" - is a union over the modules a station
// has, so two modules naming the same screen is an ordinary composition and
// never a conflict.
//
// ⚑⚑ THIS IS THE SECOND HALF OF A PARALLEL PAIR AND SAYING SO NOW IS THE POINT.
// `game::StationScreenState::Tab` is the other half, and Phase 34's own risk
// register names "two tables silently parallel" as the defect this phase
// produces if it produces one. They are deliberately NOT one enum: this layer
// must not learn what a tab is, and the game layer must not learn what a def
// is. Stage C owns the mapping between them and owes it a static assertion on
// the counts, not a comment.
//
// ⚑ Two screens on the list have no module that offers them and that is a
// question rather than a gap: `Factions` is standings, which are a fact about
// the galaxy rather than a facility, and `Survey` sells scan data - whose own
// hint text says it "sells anywhere". Stage C rules on whether they are
// unconditional; the vocabulary is complete either way so that the ruling has
// somewhere to land.
enum class StationScreen : std::uint32_t
{
    Trade = 0,
    Outfitting,
    Shipyard,
    Crew,
    Factions,
    Missions,
    Survey,
    Refinery,
    // Phase 35 stage A. APPENDED rather than slotted beside Missions, and the
    // reason is worth a line: the strip draws in enum order, so the position of
    // a value here is a UI decision made in an assets header. Appending keeps
    // every existing number where it was and puts the new tab at the end, which
    // is where a room you go to for gossip belongs relative to eight tabs you go
    // to for business.
    Bar,
    Count,
};

inline constexpr std::size_t kStationScreenCount = static_cast<std::size_t>(StationScreen::Count);

[[nodiscard]] const char* stationScreenName(StationScreen screen);
[[nodiscard]] bool parseStationScreen(std::string_view text, StationScreen& out);

// One hold line on a module ("bulk:1200" in TOML): units of stock capacity, per
// commodity, for goods of that class. The same shape `StationRate` has, and for
// the same reason - a list of "id:number" strings reads as a sentence and
// parses in one place.
struct ModuleStorage
{
    GoodsClass goods = GoodsClass::Bulk;
    float capacity = 0.0f; // units per commodity of this class
};

// One module. A station is a list of these (v1: composed by the generator).
struct ModuleDef
{
    std::string id;
    std::string name;
    // Required, so the default below is never the answer to anything. There is
    // no "unset" family for the reason `CommodityTier` has no default: every
    // value is a real answer, so a parser that picked one would be answering
    // gdd.md §12 on the author's behalf.
    ModuleFamily family = ModuleFamily::Power;
    // The same three lists a station archetype carries, and deliberately the
    // same vocabulary: a composed station's rates are the SUM of its modules'
    // (stage B), so anything that cannot be said here cannot survive the
    // decomposition of `stations.toml`.
    std::vector<StationRate> produces;
    std::vector<StationRate> consumes;
    std::vector<StationRate> feedstock;
    std::vector<ModuleStorage> stores;
    // The budget, in the ship system's own terms (`ComponentDef::powerDraw`
    // against `ShipDef::powerOutput`) because gdd.md §12 asks for exactly that
    // analogy: "a station is power-limited exactly as a ship is".
    //
    // ⚑⚑ RULED 2026-08-31: POWER IS THE CONSTRAINT THE COMPOSER SATISFIES, so
    // this figure has exactly one reader and it is the generator - a
    // composition is valid only when its draw is covered by its own output.
    // That is what stops a "weighted recipe" being a bag of modules with a
    // weight on it.
    float powerOutput = 0.0f; // Power family only, refused elsewhere
    float powerDraw = 0.0f;
    std::vector<StationScreen> screens;
    // The player-facing refining service (Phase 8f), which is a SERVICE rather
    // than a production line: it takes `refineInput` off the player's hands and
    // hands back `refineOutput` later. Both empty - the default - means the
    // module offers no service. Kept separate from `feedstock`/`produces` for
    // the reason `StationDef` keeps them separate, one layer down.
    std::string refineInput;
    std::string refineOutput;
    std::string source;
};

// ---------------------------------------------------------------------------
// Authored systems (Phase 29). A `[[system]]` row is a place somebody PUT
// somewhere, as opposed to the eighty the seed produces, and the campaign can
// name it because its id is invented here rather than rolled.
//
// ⚑⚑⚑ EVERY OPTIONAL FIELD CARRIES AN EXPLICIT `has…` FLAG RATHER THAN A
// SENTINEL, AND THAT IS FORCED RATHER THAN CHOSEN. Two of the fields the
// generator holds have no free sentinel to spare: `SystemSpec::factionIndex`
// is `kNoFaction` both when nobody has said anything and when an author
// deliberately says "lawless", and `primaryPlanet == 0` is both "unset" and
// "the first planet". Comparing against a default therefore cannot answer
// "did the author write this?", which is the only question the generator needs
// to ask - so the parser answers it once and records the answer.
// ---------------------------------------------------------------------------

// A planet an author named. Positions are deliberately absent: the generator
// still lays the orbits out, so writing a system by hand never means writing
// coordinates in metres.
struct AuthoredPlanetDef
{
    std::string name;
    double radius = 0.0; // metres; unset means the generator rolls one
    bool hasRadius = false;
};

// A station an author placed. `stationId` names a `[[station]]` archetype and
// is resolved to its index at the same place, and by the same rule, that
// `faction_station_bias` already resolves one.
struct AuthoredStationDef
{
    std::string name;
    std::string stationId;
};

struct SystemDef
{
    std::string id; // invented by the author; the campaign's handle on the place
    // How the generator chooses which node this system becomes: "random",
    // "anywhere", "at_system" or "jumps_from".
    //
    // ⚑⚑ THE RULE IS ALWAYS NAMED HERE, AND A RULE THAT TAKES PARAMETERS PUTS
    // THEM IN A SIBLING KEY OF THE SAME NAME. decisions/018 wrote the four
    // rules two different ways - two as bare words and two as keys carrying a
    // value - which left a reader scanning every key in the row to find out
    // how the system was placed. Naming it in one place costs one line in the
    // parameterised cases and makes the refusals sayable:
    //
    //   placement = "jumps_from"
    //   jumps_from = { system = "campaign.hollow", min = 2, max = 4 }
    std::string placement = "random";
    // "at_system" only: a `[[faction]]` id, meaning THAT FACTION'S CAPITAL.
    //
    // ⚑⚑⚑ IT NAMES A FACTION RATHER THAN A SYSTEM BECAUSE A CAPITAL IS THE
    // ONLY STABLE PROCEDURAL LANDMARK THERE IS. An authored id cannot be named
    // - that system already occupies its own node, so every such argument is a
    // contradiction - and a procedural NAME is a fact about one seed at one
    // system count, rolled after placement has already happened.
    std::string atSystemFactionId;
    // "jumps_from" only: an earlier authored system's id and a closed ring of
    // gate distance around it.
    std::string jumpsFromSystemId;
    std::uint32_t jumpsFromMin = 0;
    std::uint32_t jumpsFromMax = 0;
    std::string name;
    bool hasName = false;
    // "core" | "frontier" | "fringe".
    std::string region;
    bool hasRegion = false;
    // A `[[faction]]` id. Mutually exclusive with `lawless`, because the two
    // are exactly the pair that a sentinel could not tell apart.
    std::string factionId;
    bool hasFaction = false;
    bool lawless = false;
    std::uint32_t primaryPlanet = 0;
    bool hasPrimaryPlanet = false;
    // How hard this place is held, from 0 to 1 (Phase 30 stage E). It replaces
    // the generated baseline outright rather than nudging it: a fortress on the
    // rim and a neglected core system are both things a campaign gets to state
    // rather than argue for.
    //
    // ⚑⚑⚑ IT IS A MAGNITUDE, AND A SIGNED ONE IS REFUSED BY NAME. The rating a
    // player reads is signed and `sol.security` prints it that way, so writing
    // -0.6 here is the obvious mistake - and decisions/019 decision 2 says
    // exactly why it cannot be allowed: THE SIGN IS NOT HOW MUCH, IT IS WHO
    // POLICES THE PLACE. The generator takes it from the owner, where it is a
    // fact rather than a choice, and a clan-held system therefore reads
    // negative on its own.
    float security = 0.0f;
    bool hasSecurity = false;
    // A placement flag and nothing else in this phase: nothing hides a system
    // from the map yet, and the exploration payoff GDD §8 promises is a later
    // phase reading this.
    bool secret = false;
    std::vector<AuthoredPlanetDef> planets;
    std::vector<AuthoredStationDef> stations;
    std::string source;
};

// One internal jump lane inside a constellation, naming two of that same
// constellation's member ids (Phase 29 stage C).
struct ConstellationLinkDef
{
    std::string fromId;
    std::string toId;
};

// A group of systems placed together, keeping the lanes their author drew
// between them (Phase 29 stage C, decisions/018).
//
// ⚑⚑⚑ ITS MEMBERS ARE `[[constellation.system]]` ROWS RATHER THAN IDS POINTING
// AT `[[system]]` ROWS ELSEWHERE, because a member has no placement of its own
// - the group carries it - and a `[[system]]` that is secretly placed by
// something else is a row whose own `placement` key would be a lie. Nesting
// says it once: everything inside the group belongs to the group.
//
// ⚑⚑ `placement` HAS EXACTLY ONE LEGAL VALUE, AND SAYING SO IS WHY THE KEY
// EXISTS. Three of the four rules REPLACE an existing system, and a group
// cannot replace one node as a unit; making `random` mean "near a random
// system" for a group while it means "become a random system" for a system is
// two rules wearing one word. Anything but "anywhere" is refused with that
// reason, which is a better answer to an author than a key that is not there.
struct ConstellationDef
{
    std::string id; // the group's own id; names it in a refusal
    std::string placement = "anywhere";
    std::vector<SystemDef> members;
    // Empty means the members are chained in declaration order, which is what
    // the generator does with it: a group whose members have no lanes between
    // them is not a group.
    std::vector<ConstellationLinkDef> links;
    std::string source;
};

class DefDatabase
{
public:
    void clear();

    // Merges one TOML document (any mix of [[ship]]/[[weapon]]/[[faction]]).
    // sourceName appears in errors and def provenance. On error the database
    // is left as it was before the call.
    [[nodiscard]] bool
    mergeToml(const char* text, std::size_t length, const char* sourceName, std::string* outError = nullptr);

    // Reads and merges every *.toml directly inside directory, sorted by
    // path for determinism. A missing directory is fine (a mod without data).
    [[nodiscard]] bool mergeDirectory(const char* directory, std::string* outError = nullptr);

    // Cross-def checks that need the fully merged database: a faction pair
    // declaring initial relations from both sides must agree (Phase 8b).
    // Relations naming unknown faction ids are ignored downstream, not here —
    // a mod may remove a faction another def still references.
    [[nodiscard]] bool validateFactions(std::string* outError = nullptr) const;

    // Cross-def check for `[[role]]` rows, and it REFUSES rather than warns
    // (Phase 19). `modelIdFromName`'s warn-and-fall-back is right for a ship
    // def - one bad name breaks one ship and a real hull is still there to
    // stand in - but a role has no fallback once the literal it replaced is
    // gone, and a silently invisible gate in all eighty systems is worse than
    // a refusal that names the file. Same treatment `loadModels` gives a mesh
    // that is not on disk.
    //
    // `required` is the caller's vocabulary: `sol::assets` validates whatever
    // list it is handed and does not itself know what a cockpit is.
    [[nodiscard]] bool validateRoles(std::span<const char* const> required,
                                     std::string* outError = nullptr) const;

    // Cross-def check for `[[model]]` rows naming a `[[material]]` (Phase 25
    // stage A), and it REFUSES for the same reason `validateRoles` does: a
    // model whose material is missing has nothing left to draw with, because
    // naming a material is exactly what gave up the four surface keys. It is
    // a separate pass rather than a parse-time check because a material may
    // legitimately live in an earlier or a later layer than the model.
    [[nodiscard]] bool validateMaterials(std::string* outError = nullptr) const;

    // Cross-def check for `[[system]]` and `[[constellation]]` rows (Phase 29),
    // and it REFUSES for the
    // reason decision 3 gives: an authored system whose faction or whose
    // station archetype does not exist has no fallback that is not a lie about
    // where the campaign starts. Separate from the parse for the same reason
    // `validateMaterials` is - a faction may legitimately live in an earlier or
    // a later layer than the system that names it.
    [[nodiscard]] bool validateSystems(std::string* outError = nullptr) const;

    // ⚑⚑ EVERY SHIP ID IN EVERY FACTION ROSTER NAMES A `[[ship]]` THAT EXISTS
    // (Phase 32 stage C), AND IT REFUSES. Until this pass a roster entry with a
    // stale id was found by `spawnWing` at spawn time, which warned and then
    // ABANDONED THE WHOLE WING - so one wrong character meant a patrol that
    // silently never appeared, in a system a player might never visit. That is
    // the "broken" this stage has to be distinguishable from, and the cheapest
    // way to distinguish it is to make it impossible: a roster that cannot fly
    // has no fallback that is not a lie about who holds the sky. Separate from
    // the parse for the same reason `validateMaterials` is - a ship def may
    // legitimately live in an earlier or a later layer than the faction.
    [[nodiscard]] bool validateRosters(std::string* outError = nullptr) const;

    // Phase 33 stage B: a `requires` on any sellable def names a commodity that
    // exists. Refuses for `validateRosters`'s reason - a gate keyed on a good
    // no `[[commodity]]` row defines can never open, so the item is invisible
    // in every station in the galaxy and nothing ever says why.
    [[nodiscard]] bool validateCatalogGates(std::string* outError = nullptr) const;

    // Phase 33 stage D: every id a faction lists as contraband or restricted
    // names a real commodity. Same shape and same reason as the gate check
    // above, with one difference worth stating - a gate that can never open
    // hides an item, and a law that can never fire looks exactly like a
    // patrol deciding to let you off.
    [[nodiscard]] bool validateLegality(std::string* outError = nullptr) const;

    // Phase 34 stage B: every module a `[[station]]` recipe names exists, and
    // no recipe names a POWER module. Refuses for `validateCatalogGates`'s
    // reason and one of its own: a recipe line that resolves to nothing is a
    // production line missing from every station of that archetype in the
    // galaxy, which reads as an economy that does not balance rather than as a
    // typo - and power is DERIVED (the composer fits a plant to the draw), so a
    // hand-placed plant is an author expecting a rule that does not exist.
    [[nodiscard]] bool validateStationRecipes(std::string* outError = nullptr) const;

    // Phase 35 stage C: every anchor a `[[character]]` names - a faction, a
    // station archetype, a room module - exists, and `region` is one of the
    // three the generator has. Refuses for `validateStationRecipes`' reason and
    // one sharper: an anchor that resolves to nothing selects NO SEAT, so the
    // character is simply not in the galaxy, and a person who is nowhere is
    // indistinguishable from a person nobody wrote. ⚑ Whether an anchor that
    // resolves finds a FREE seat in a PARTICULAR galaxy is a different question
    // and is not askable here - it is a galaxy-level claim, and it is asserted
    // where a galaxy is in hand (`game/test/station_cast_tests.cpp`).
    [[nodiscard]] bool validateCharacters(std::string* outError = nullptr) const;

    [[nodiscard]] const ShipDef* findShip(const char* id) const;
    [[nodiscard]] const WeaponDef* findWeapon(const char* id) const;
    [[nodiscard]] const FactionDef* findFaction(const char* id) const;
    [[nodiscard]] const CommodityDef* findCommodity(const char* id) const;
    [[nodiscard]] const StationDef* findStation(const char* id) const;
    [[nodiscard]] const ModuleDef* findModule(const char* id) const;
    [[nodiscard]] const SystemDef* findSystem(const char* id) const;
    [[nodiscard]] const ComponentDef* findComponent(const char* id) const;
    [[nodiscard]] const CrewDef* findCrew(const char* id) const;
    [[nodiscard]] const SoundDef* findSound(const char* id) const;
    [[nodiscard]] const ModelDef* findModel(const char* id) const;
    [[nodiscard]] const MaterialDef* findMaterial(const char* id) const;
    [[nodiscard]] const RoleDef* findRole(const char* id) const;
    [[nodiscard]] const CharacterDef* findCharacter(const char* id) const;

    // Index of a model by id, or kNoModel. The renderer and the sim both key
    // off this index rather than off the string, so a name is resolved once at
    // spawn and never in a per-tick loop.
    static constexpr std::uint32_t kNoModel = 0xFFFFFFFFu;
    [[nodiscard]] std::uint32_t modelIndex(const char* id) const;

    // Index of a material by id, or `kNoMaterial`. Same bargain as
    // `modelIndex`: the renderer binds off an integer, never off a string.
    [[nodiscard]] std::uint32_t materialIndex(const char* id) const;

    // The model index filling a role, or kNoModel. After `validateRoles` has
    // passed this cannot fail for a required role, so callers resolve once at
    // spawn and do not branch per instance.
    [[nodiscard]] std::uint32_t roleModelIndex(const char* id) const;

    // First-definition order; later layers replace elements in place, so
    // indices stay stable across a reload that only edits values.
    [[nodiscard]] const std::vector<ShipDef>& ships() const { return m_ships; }

    [[nodiscard]] const std::vector<WeaponDef>& weapons() const { return m_weapons; }

    [[nodiscard]] const std::vector<FactionDef>& factions() const { return m_factions; }

    [[nodiscard]] const std::vector<CommodityDef>& commodities() const { return m_commodities; }

    [[nodiscard]] const std::vector<StationDef>& stations() const { return m_stations; }

    // gdd.md §12's vocabulary, in first-definition order (Phase 34 stage A).
    [[nodiscard]] const std::vector<ModuleDef>& modules() const { return m_modules; }

    // Authored systems in first-definition order, which is the order their
    // placement rules resolve in (decision 4).
    [[nodiscard]] const std::vector<SystemDef>& systems() const { return m_systems; }

    // Groups placed as a unit, in first-definition order. Their members are
    // authored systems too, so everything `systems()` promises about a
    // `[[system]]` row holds for a `[[constellation.system]]` row as well.
    [[nodiscard]] const std::vector<ConstellationDef>& constellations() const { return m_constellations; }

    [[nodiscard]] const std::vector<ComponentDef>& components() const { return m_components; }

    [[nodiscard]] const std::vector<CrewDef>& crew() const { return m_crew; }

    [[nodiscard]] const std::vector<SoundDef>& sounds() const { return m_sounds; }

    [[nodiscard]] const std::vector<ModelDef>& models() const { return m_models; }

    // Authored rows first, in first-definition order, then the synthesised
    // ones in model order. Both halves are deterministic, which is what lets a
    // model hold an index into this rather than a name.
    [[nodiscard]] const std::vector<MaterialDef>& materials() const { return m_materials; }

    [[nodiscard]] const std::vector<RoleDef>& roles() const { return m_roles; }

    // The authored cast, in first-definition order - which is the order the
    // placement pass seats them in, so a row APPENDED to the file cannot move
    // anybody already written (Phase 35 stage C).
    [[nodiscard]] const std::vector<CharacterDef>& characters() const { return m_characters; }

private:
    std::vector<ShipDef> m_ships;
    std::vector<WeaponDef> m_weapons;
    std::vector<FactionDef> m_factions;
    std::vector<CommodityDef> m_commodities;
    std::vector<StationDef> m_stations;
    std::vector<ModuleDef> m_modules;
    std::vector<SystemDef> m_systems;
    std::vector<ConstellationDef> m_constellations;
    std::vector<ComponentDef> m_components;
    std::vector<CrewDef> m_crew;
    std::vector<SoundDef> m_sounds;
    std::vector<ModelDef> m_models;
    std::vector<MaterialDef> m_materials;
    std::vector<RoleDef> m_roles;
    std::vector<CharacterDef> m_characters;

    // Drops every synthesised row, derives one afresh for each model that
    // names no material, and re-resolves every model's index. Runs at the tail
    // of each merge so the database is never half-resolved and no caller has
    // to remember a second call - the trap `validateRoles` avoids only by
    // being const.
    void resolveMaterials();
};

} // namespace sol::assets
