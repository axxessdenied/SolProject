#include <sol/assets/data_defs.hpp>

#include <sol/test/test.hpp>

#include <cstring>
#include <string>

using sol::assets::DefDatabase;
using sol::assets::ShipDef;

namespace {

bool merge(DefDatabase& db, const char* toml, const char* source, std::string* outError = nullptr)
{
    return db.mergeToml(toml, std::strlen(toml), source, outError);
}

constexpr const char* kBaseShips = R"(
[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0
max_turn_rate = [1.6, 1.2, 2.6]

[[ship]]
id = "sol.freighter"
name = "Freighter"
scale = 3.0
forward_accel = 20.0
max_speed = 120.0
)";

} // namespace

SOL_TEST(data_defs_parse_ships)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kBaseShips, "ships.toml", &error));
    SOL_CHECK(db.ships().size() == 2);

    const ShipDef* shuttle = db.findShip("sol.shuttle");
    SOL_CHECK(shuttle != nullptr);
    SOL_CHECK(shuttle->name == "Shuttle");
    SOL_CHECK(shuttle->flight.maxSpeed == 220.0f);
    SOL_CHECK(shuttle->flight.maxTurnRate[2] == 2.6f);
    // Unspecified keys keep defaults.
    SOL_CHECK(shuttle->flight.forwardAccel == 60.0f);
    SOL_CHECK(shuttle->scale == 1.0f);

    const ShipDef* freighter = db.findShip("sol.freighter");
    SOL_CHECK(freighter != nullptr);
    SOL_CHECK(freighter->scale == 3.0f);
    SOL_CHECK(freighter->flight.forwardAccel == 20.0f);
}

SOL_TEST(data_defs_layer_override_replaces_in_place)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kBaseShips, "base/ships.toml", &error));

    const char* modShips = R"(
[[ship]]
id = "sol.shuttle"
name = "Shuttle Mk2"
max_speed = 300.0
)";
    SOL_CHECK(merge(db, modShips, "mod/ships.toml", &error));

    SOL_CHECK(db.ships().size() == 2);
    // Replaced in place: still first, fully overridden (not patched).
    SOL_CHECK(db.ships()[0].id == "sol.shuttle");
    SOL_CHECK(db.ships()[0].name == "Shuttle Mk2");
    SOL_CHECK(db.ships()[0].flight.maxSpeed == 300.0f);
    SOL_CHECK(db.ships()[0].flight.maxTurnRate[0] == 1.6f); // back to default
    SOL_CHECK(db.ships()[0].source == "mod/ships.toml");
}

SOL_TEST(data_defs_weapons_and_factions)
{
    DefDatabase db;
    std::string error;
    const char* doc = R"(
[[weapon]]
id = "sol.pulse"
name = "Pulse Cannon"
kind = "projectile"
damage = 12.0
rate_of_fire = 4.0
range = 2500.0
projectile_speed = 900.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.2, 0.4, 1.0]
)";
    SOL_CHECK(merge(db, doc, "defs.toml", &error));
    SOL_CHECK(db.findWeapon("sol.pulse") != nullptr);
    SOL_CHECK(db.findWeapon("sol.pulse")->damage == 12.0f);
    SOL_CHECK(db.findFaction("sol.navy") != nullptr);
    SOL_CHECK(db.findFaction("sol.navy")->color[2] == 1.0f);
}

SOL_TEST(data_defs_faction_sim_fields_and_gates)
{
    DefDatabase db;
    std::string error;
    const char* doc = R"(
[[faction]]
id = "sol.navy"
name = "Solar Navy"
kind = "major"
aggression = 0.3
forgiveness = 0.7
relations = ["sol.corsairs:-60", "sol.guild:40"]
ships_patrol = ["sol.fighter"]
ships_raider = ["sol.fighter"]

[[faction]]
id = "sol.corsairs"
name = "Corsairs"
kind = "pirate"
relations = ["sol.navy:-60"]

[[faction]]
id = "sol.guild"
name = "Freight Guild"

[[module]]
id = "sol.navy_shield"
name = "Navy Shield"
slot = "shield"
factions = ["sol.navy"]
min_rep = 20.0
shield_strength_add = 50.0
)";
    SOL_CHECK(merge(db, doc, "factions.toml", &error));
    const sol::assets::FactionDef* navy = db.findFaction("sol.navy");
    SOL_CHECK(navy != nullptr);
    SOL_CHECK(navy->kind == sol::assets::FactionKind::Major);
    SOL_CHECK(navy->aggression == 0.3f);
    SOL_CHECK(navy->forgiveness == 0.7f);
    SOL_CHECK(navy->relations.size() == 2);
    SOL_CHECK(navy->relations[0].otherId == "sol.corsairs");
    SOL_CHECK(navy->relations[0].standing == -60.0f);
    SOL_CHECK(navy->shipsPatrol.size() == 1 && navy->shipsPatrol[0] == "sol.fighter");
    SOL_CHECK(db.findFaction("sol.corsairs")->kind == sol::assets::FactionKind::Pirate);
    // Defaults on the untouched faction.
    SOL_CHECK(db.findFaction("sol.guild")->aggression == 0.5f);
    SOL_CHECK(db.findFaction("sol.guild")->relations.empty());

    const sol::assets::ModuleDef* gated = db.findModule("sol.navy_shield");
    SOL_CHECK(gated != nullptr);
    SOL_CHECK(gated->gate.factions.size() == 1 && gated->gate.factions[0] == "sol.navy");
    SOL_CHECK(gated->gate.minRep == 20.0f);
    // Ungated defs default open.
    SOL_CHECK(navy->shipsRaider.size() == 1);

    // Both sides declared and agreeing: valid.
    SOL_CHECK(db.validateFactions(&error));

    // Bad kind, out-of-range weights, malformed relations: schema errors.
    SOL_CHECK(!merge(db, "[[faction]]\nid = \"f\"\nname = \"F\"\nkind = \"guild\"\n", "bad.toml",
                     &error));
    SOL_CHECK(error.find("kind") != std::string::npos);
    SOL_CHECK(!merge(db, "[[faction]]\nid = \"f\"\nname = \"F\"\naggression = 1.5\n", "bad.toml",
                     &error));
    SOL_CHECK(!merge(db, "[[faction]]\nid = \"f\"\nname = \"F\"\nrelations = [\"sol.navy\"]\n",
                     "bad.toml", &error));
    SOL_CHECK(error.find("id:standing") != std::string::npos);
    SOL_CHECK(!merge(db, "[[module]]\nid = \"m\"\nname = \"M\"\nslot = \"cargo\"\nmin_rep = 150\n",
                     "bad.toml", &error));
    SOL_CHECK(error.find("min_rep") != std::string::npos);
}

SOL_TEST(data_defs_faction_relations_must_agree)
{
    DefDatabase db;
    std::string error;
    const char* doc = R"(
[[faction]]
id = "sol.a"
name = "A"
relations = ["sol.b:-40"]

[[faction]]
id = "sol.b"
name = "B"
relations = ["sol.a:-10"]
)";
    SOL_CHECK(merge(db, doc, "factions.toml", &error));
    SOL_CHECK(!db.validateFactions(&error));
    SOL_CHECK(error.find("mismatched") != std::string::npos);
    // One-sided declarations and unknown ids are fine.
    DefDatabase oneSided;
    SOL_CHECK(merge(oneSided, "[[faction]]\nid = \"sol.a\"\nname = \"A\"\nrelations = [\"sol.gone:-40\"]\n",
                    "factions.toml", &error));
    SOL_CHECK(oneSided.validateFactions(&error));
}

SOL_TEST(data_defs_validation_errors)
{
    DefDatabase db;
    std::string error;

    // Missing required key.
    SOL_CHECK(!merge(db, "[[ship]]\nname = 'NoId'\n", "bad.toml", &error));
    SOL_CHECK(error.find("missing key 'id'") != std::string::npos);

    // Unknown key (typo) is an error, and names the offending def.
    const char* typo = R"(
[[ship]]
id = "sol.typo"
name = "Typo"
maxspeed = 100.0
)";
    error.clear();
    SOL_CHECK(!merge(db, typo, "bad.toml", &error));
    SOL_CHECK(error.find("unknown key 'maxspeed'") != std::string::npos);
    SOL_CHECK(error.find("sol.typo") != std::string::npos);

    // Wrong type.
    error.clear();
    SOL_CHECK(!merge(db, "[[ship]]\nid = \"s\"\nname = \"S\"\nmax_speed = \"fast\"\n", "bad.toml",
                     &error));
    SOL_CHECK(error.find("must be a number") != std::string::npos);

    // Bad weapon kind.
    error.clear();
    SOL_CHECK(!merge(db, "[[weapon]]\nid = \"w\"\nname = \"W\"\nkind = \"beam\"\n", "bad.toml",
                     &error));
    SOL_CHECK(error.find("kind") != std::string::npos);

    // Unknown top-level array.
    error.clear();
    SOL_CHECK(!merge(db, "[[shp]]\nid = \"s\"\nname = \"S\"\n", "bad.toml", &error));
    SOL_CHECK(error.find("unknown def kind 'shp'") != std::string::npos);

    // A failed merge leaves the database untouched.
    SOL_CHECK(db.ships().empty() && db.weapons().empty() && db.factions().empty());
}

SOL_TEST(data_defs_failed_merge_preserves_previous_layers)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kBaseShips, "base/ships.toml", &error));
    SOL_CHECK(!merge(db, "[[ship]]\nid = \"x\"\n", "mod/ships.toml", &error));
    SOL_CHECK(db.ships().size() == 2);
    SOL_CHECK(db.findShip("sol.shuttle")->name == "Shuttle");
}

SOL_TEST(data_defs_parse_commodities_and_stations)
{
    DefDatabase db;
    std::string error;
    const char* toml = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_frontier = 1.5
produces = ["sol.food:0.8"]
consumes = ["sol.machinery:0.1"]
stock_capacity = 1200.0
)";
    SOL_CHECK(merge(db, toml, "economy.toml", &error));
    const sol::assets::CommodityDef* food = db.findCommodity("sol.food");
    SOL_CHECK(food != nullptr);
    SOL_CHECK(food->basePrice == 8.0f);

    const sol::assets::StationDef* agri = db.findStation("sol.station_agri");
    SOL_CHECK(agri != nullptr);
    SOL_CHECK(agri->weightFrontier == 1.5f);
    SOL_CHECK(agri->weightCore == 1.0f); // default
    SOL_CHECK(agri->produces.size() == 1);
    SOL_CHECK(agri->produces[0].commodityId == "sol.food");
    SOL_CHECK(agri->produces[0].rate == 0.8f);
    SOL_CHECK(agri->consumes.size() == 1);
    SOL_CHECK(agri->stockCapacity == 1200.0f);

    // Malformed rate strings are load errors and leave the db untouched.
    SOL_CHECK(!merge(db, R"(
[[station]]
id = "sol.bad"
name = "Bad"
produces = ["sol.food"]
)",
                     "bad.toml", &error));
    SOL_CHECK(error.find("id:rate") != std::string::npos);
    SOL_CHECK(db.findStation("sol.bad") == nullptr);
    SOL_CHECK(!merge(db, R"(
[[station]]
id = "sol.bad2"
name = "Bad"
produces = ["sol.food:-1"]
)",
                     "bad2.toml", &error));
}

SOL_TEST(data_defs_parse_sounds)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, R"(
[[sound]]
id = "sol.weapon_fire"
asset = "weapon_fire"
gain = 0.55
pitch_jitter = 0.08
max_instances = 4

[[sound]]
id = "sol.engine_loop"
asset = "engine_loop"
)",
                    "sounds.toml", &error));

    SOL_CHECK(db.sounds().size() == 2);
    const auto* fire = db.findSound("sol.weapon_fire");
    SOL_REQUIRE(fire != nullptr);
    SOL_CHECK(fire->asset == "weapon_fire");
    SOL_CHECK(fire->gain > 0.54f && fire->gain < 0.56f);
    SOL_CHECK(fire->pitchJitter > 0.07f && fire->pitchJitter < 0.09f);
    SOL_CHECK(fire->maxInstances == 4);

    // Defaults: full gain, no jitter, unlimited instances, 500 m rolloff.
    const auto* loop = db.findSound("sol.engine_loop");
    SOL_REQUIRE(loop != nullptr);
    SOL_CHECK(loop->gain == 1.0f);
    SOL_CHECK(loop->pitchJitter == 0.0f);
    SOL_CHECK(loop->maxInstances == 0);
    SOL_CHECK(loop->rolloff == 500.0f);
    SOL_CHECK(db.findSound("sol.nothing") == nullptr);
}

SOL_TEST(data_defs_sound_validation_errors)
{
    DefDatabase db;
    std::string error;

    // A cue with no asset cannot resolve to anything, so it is a load error
    // rather than a silent cue discovered later.
    SOL_CHECK(!merge(db, "[[sound]]\nid = \"a\"\n", "s.toml", &error));
    SOL_CHECK(error.find("asset") != std::string::npos);

    SOL_CHECK(!merge(db, "[[sound]]\nid = \"a\"\nasset = \"x\"\ngain = -1.0\n", "s.toml", &error));
    SOL_CHECK(!merge(db, "[[sound]]\nid = \"a\"\nasset = \"x\"\npitch_jitter = 0.9\n", "s.toml",
                     &error));
    SOL_CHECK(!merge(db, "[[sound]]\nid = \"a\"\nasset = \"x\"\nrolloff = 0.0\n", "s.toml", &error));
    // Strict schema: a typo dies at load, not at play time.
    SOL_CHECK(!merge(db, "[[sound]]\nid = \"a\"\nasset = \"x\"\nvolume = 1.0\n", "s.toml", &error));

    // Nothing above was allowed to land.
    SOL_CHECK(db.sounds().empty());

    // A mod layer replaces a cue in place, the same rule every other def has.
    SOL_CHECK(merge(db, "[[sound]]\nid = \"a\"\nasset = \"x\"\ngain = 1.0\n", "base.toml", &error));
    SOL_CHECK(merge(db, "[[sound]]\nid = \"a\"\nasset = \"y\"\ngain = 0.25\n", "mod.toml", &error));
    SOL_CHECK(db.sounds().size() == 1);
    SOL_CHECK(db.findSound("a")->asset == "y");
    SOL_CHECK(db.findSound("a")->gain == 0.25f);
}

SOL_TEST(data_defs_parse_mining_fields)
{
    DefDatabase db;
    std::string error;
    const char* toml = R"(
[[commodity]]
id = "sol.ore"
name = "Raw Ore"
base_price = 12.0
ore_weight_core = 1.0
ore_weight_fringe = 2.5

[[commodity]]
id = "sol.metal"
name = "Refined Metal"
base_price = 32.0

[[station]]
id = "sol.station_refinery"
name = "Refinery"
produces = ["sol.metal:1.4"]
consumes = ["sol.ore:2.4"]
refine_input = "sol.ore"
refine_output = "sol.metal"

[[weapon]]
id = "sol.mining_laser"
name = "Mining Laser"
kind = "hitscan"
damage = 3.0
mining_power = 4.0
)";
    SOL_REQUIRE(merge(db, toml, "mining.toml", &error));

    // Ore weights say what a rock can be made of, per region tier; a
    // commodity that says nothing is simply not mineable.
    const sol::assets::CommodityDef* ore = db.findCommodity("sol.ore");
    const sol::assets::CommodityDef* metal = db.findCommodity("sol.metal");
    SOL_REQUIRE(ore != nullptr && metal != nullptr);
    SOL_CHECK(ore->oreWeightCore == 1.0f);
    SOL_CHECK(ore->oreWeightFrontier == 0.0f); // default: not this tier
    SOL_CHECK(ore->oreWeightFringe == 2.5f);
    SOL_CHECK(metal->oreWeightCore == 0.0f);
    SOL_CHECK(metal->oreWeightFringe == 0.0f);

    const sol::assets::StationDef* refinery = db.findStation("sol.station_refinery");
    SOL_REQUIRE(refinery != nullptr);
    SOL_CHECK(refinery->refineInput == "sol.ore");
    SOL_CHECK(refinery->refineOutput == "sol.metal");

    const sol::assets::WeaponDef* laser = db.findWeapon("sol.mining_laser");
    SOL_REQUIRE(laser != nullptr);
    SOL_CHECK(laser->miningPower == 4.0f);

    // An ordinary gun mines nothing unless its def says so.
    SOL_REQUIRE(merge(db, R"(
[[weapon]]
id = "sol.pulse_cannon"
name = "Pulse Cannon"
kind = "projectile"
damage = 12.0
)",
                      "guns.toml", &error));
    SOL_CHECK(db.findWeapon("sol.pulse_cannon")->miningPower == 0.0f);

    // Half a refinery is a schema error: the service needs both ends.
    SOL_CHECK(!merge(db, R"(
[[station]]
id = "sol.half_refinery"
name = "Half Refinery"
refine_input = "sol.ore"
)",
                     "half.toml", &error));
    SOL_CHECK(error.find("refine_input") != std::string::npos);
    SOL_CHECK(db.findStation("sol.half_refinery") == nullptr);

    // Negative ore weights are a schema error, not a silent zero.
    SOL_CHECK(!merge(db, R"(
[[commodity]]
id = "sol.bad_ore"
name = "Bad Ore"
ore_weight_core = -1.0
)",
                     "bad_ore.toml", &error));
}
