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
station_bias = ["sol.station_factory:2.5", "sol.station_agri:0"]

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

    // Phase 13: what a faction builds. A zero is a legal, deliberate "never",
    // which is why the parser bounds it at >= 0 rather than > 0.
    SOL_REQUIRE(navy->stationBias.size() == 2);
    SOL_CHECK(navy->stationBias[0].stationId == "sol.station_factory");
    SOL_CHECK(navy->stationBias[0].weight == 2.5f);
    SOL_CHECK(navy->stationBias[1].stationId == "sol.station_agri");
    SOL_CHECK(navy->stationBias[1].weight == 0.0f);
    // A faction with no character says nothing, and that is the default.
    SOL_CHECK(db.findFaction("sol.guild")->stationBias.empty());

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
    // Malformed and negative station_bias entries are schema errors; an unknown
    // station id is NOT (a mod may remove an archetype, so it warns downstream).
    SOL_CHECK(!merge(db, "[[faction]]\nid = \"f\"\nname = \"F\"\nstation_bias = [\"sol.st\"]\n",
                     "bad.toml", &error));
    SOL_CHECK(error.find("id:weight") != std::string::npos);
    SOL_CHECK(!merge(db,
                     "[[faction]]\nid = \"f\"\nname = \"F\"\nstation_bias = [\"sol.st:-1\"]\n",
                     "bad.toml", &error));
    SOL_CHECK(merge(db,
                    "[[faction]]\nid = \"f2\"\nname = \"F2\"\nstation_bias = [\"sol.nope:2\"]\n",
                    "ok.toml", &error));
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
    // ⚑ Phase 9 stage H. The default is the model every archetype already drew,
    // so adding the key changed nothing the game draws; what it changed is that
    // a station's LOOK stopped being a name compiled into space_world.cpp.
    SOL_CHECK(agri->model == "station");

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

    // A station naming its own model, which is the whole point of the key.
    SOL_CHECK(merge(db, R"(
[[station]]
id = "sol.station_relay"
name = "Relay"
model = "gate"
)",
                    "relay.toml", &error));
    const sol::assets::StationDef* relay = db.findStation("sol.station_relay");
    SOL_CHECK(relay != nullptr);
    SOL_CHECK(relay->model == "gate");
    // An empty one is refused rather than silently meaning "no model at all",
    // which would draw nothing and read as a missing station.
    SOL_CHECK(!merge(db, R"(
[[station]]
id = "sol.station_void"
name = "Void"
model = ""
)",
                     "void.toml", &error));
    SOL_CHECK(error.find("model") != std::string::npos);
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

SOL_TEST(data_defs_parse_models)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, R"(
[[model]]
id = "cube"
mesh = "cube"
texture = "checker"
radius = 1.0

[[model]]
id = "station"
mesh = "station"
texture = "hull"
radius = 100.0
avoid_radius = 130.0

[[model]]
id = "gate"
mesh = "cube"
texture = "checker"
solid = false
)",
                    "models.toml", &error));

    SOL_CHECK(db.models().size() == 3);
    // Def order IS the runtime model index, which is what lets the renderer
    // and the sim key off an integer instead of re-resolving a string.
    SOL_CHECK(db.modelIndex("cube") == 0);
    SOL_CHECK(db.modelIndex("station") == 1);
    SOL_CHECK(db.modelIndex("gate") == 2);
    SOL_CHECK(db.modelIndex("nothing") == DefDatabase::kNoModel);
    SOL_CHECK(db.findModel("nothing") == nullptr);

    // Two spheres, and the wider one is what steering dodges (Phase 8r's
    // berth approach was tuned against 130, not against the 100 you can hit).
    const auto* station = db.findModel("station");
    SOL_REQUIRE(station != nullptr);
    SOL_CHECK(station->radius == 100.0f);
    SOL_CHECK(station->avoidRadius == 130.0f);

    // Omitting avoid_radius means "the same as radius" and is resolved at
    // parse time, so no reader has to know about the sentinel.
    const auto* cube = db.findModel("cube");
    SOL_REQUIRE(cube != nullptr);
    SOL_CHECK(cube->avoidRadius == 1.0f);
    SOL_CHECK(cube->solid);
    SOL_CHECK(cube->emissive == 0.0f);

    // A gate is a doorway you fly through (Phase 8w), and that is now the
    // model's own property rather than "the only Cube left among statics".
    const auto* gate = db.findModel("gate");
    SOL_REQUIRE(gate != nullptr);
    SOL_CHECK(!gate->solid);
    // Two models sharing one mesh is the normal case, not a special one.
    SOL_CHECK(gate->mesh == cube->mesh);

    // Phase 12: a model is opaque unless it says otherwise, and the alpha
    // default is the value the shader premultiplies by to reproduce the
    // pre-Phase-12 output exactly. Both defaults are load-bearing: they are
    // what makes adding the keys a no-op for the five models that predate them.
    SOL_CHECK(!cube->translucent);
    SOL_CHECK(cube->alpha == 1.0f);
}

// Phase 12. Translucency is declared on the MODEL rather than on the instance,
// so that the second translucent thing in this game is a def row and no C++ at
// all - the same bet Phase 9 stage A made when it turned a five-member enum
// behind four hardcoded switches into this table.
SOL_TEST(data_defs_model_translucency)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, R"(
[[model]]
id = "membrane"
mesh = "gate_membrane"
texture = "hull"
radius = 70.0
translucent = true
alpha = 0.30
emissive = 0.35
solid = false
)",
                    "models.toml", &error));

    const auto* membrane = db.findModel("membrane");
    SOL_REQUIRE(membrane != nullptr);
    SOL_CHECK(membrane->translucent);
    SOL_CHECK(std::abs(membrane->alpha - 0.30f) < 1e-6f);
    SOL_CHECK(std::abs(membrane->emissive - 0.35f) < 1e-6f);
    SOL_CHECK(!membrane->solid);

    // Coverage outside 0..1 is meaningless under premultiplied blending and
    // would read as a wrongly-lit model rather than as a bad number, so it dies
    // at load while the file name is still in hand.
    SOL_CHECK(!merge(db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\nalpha = 1.5\n",
                     "m.toml", &error));
    SOL_CHECK(!merge(db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\nalpha = -0.1\n",
                     "m.toml", &error));
    // translucent takes a bool, for the same reason solid does.
    SOL_CHECK(!merge(db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\ntranslucent = 1\n",
                     "m.toml", &error));
}

// Phase 19. A `[[role]]` row says which model fills a slot the engine draws
// into, so that a gate, a rock, an ore chunk and a bolt stop being string
// literals compiled into the game. `sol::assets` deliberately does NOT know
// what those slots are - the caller hands in its own vocabulary - so every
// test here invents one, which is also what proves the layering.
namespace {

constexpr const char* kTestRoles[] = {"gate", "rock"};

constexpr const char* kRoleModels = R"(
[[model]]
id = "gate"
mesh = "gate"
texture = "hull"
radius = 70.0

[[model]]
id = "asteroid"
mesh = "asteroid"
texture = "hull"
radius = 1.0
)";

} // namespace

SOL_TEST(data_defs_parse_roles)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kRoleModels, "models.toml", &error));
    SOL_REQUIRE(merge(db, R"(
[[role]]
id = "gate"
model = "gate"

[[role]]
id = "rock"
model = "asteroid"
)",
                      "models.toml", &error));

    SOL_CHECK(db.roles().size() == 2);
    SOL_REQUIRE(db.findRole("gate") != nullptr);
    SOL_CHECK(db.findRole("gate")->model == "gate");
    SOL_CHECK(db.findRole("nothing") == nullptr);

    // What callers actually want is the model INDEX, resolved once at spawn
    // rather than per instance, exactly as `modelIndex` is used elsewhere.
    SOL_CHECK(db.roleModelIndex("rock") == db.modelIndex("asteroid"));
    SOL_CHECK(db.roleModelIndex("nothing") == DefDatabase::kNoModel);

    SOL_CHECK(db.validateRoles(kTestRoles, &error));

    // Two keys and no more: a strict schema is what makes a typo die at load
    // rather than silently draw nothing at play time.
    SOL_CHECK(!merge(db, "[[role]]\nid = \"gate\"\nmodel = \"gate\"\nscale = 2.0\n", "m.toml",
                     &error));
    SOL_CHECK(!merge(db, "[[role]]\nid = \"gate\"\n", "m.toml", &error));
    SOL_CHECK(!merge(db, "[[role]]\nmodel = \"gate\"\n", "m.toml", &error));
    // A singleton `[role]` is not a schema this loader has: every def kind is
    // an array of tables, which is the reason roles are id-keyed rows at all.
    SOL_CHECK(!merge(db, "[role]\ngate = \"gate\"\n", "m.toml", &error));
}

// ⚑ THE REASON ROLES ARE AN ID-KEYED ARRAY AND NOT A SINGLETON TABLE: this
// comes free. `mergeDef` replaces by id, so a later layer re-points a role
// exactly the way it re-points a ship, with no merge rule invented for it.
SOL_TEST(data_defs_role_overridden_by_a_later_layer)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kRoleModels, "models.toml", &error));
    SOL_REQUIRE(
        merge(db, "[[role]]\nid = \"rock\"\nmodel = \"asteroid\"\n", "models.toml", &error));
    SOL_REQUIRE(merge(db, "[[role]]\nid = \"rock\"\nmodel = \"gate\"\n", "mod.toml", &error));

    // Replaced in place, not appended: one row per slot, and the last layer
    // to speak wins.
    SOL_CHECK(db.roles().size() == 1);
    SOL_CHECK(db.findRole("rock")->model == "gate");
    SOL_CHECK(db.findRole("rock")->source == "mod.toml");
}

// ⚑ ROLES REFUSE WHERE A SHIP DEF WARNS, AND THE ASYMMETRY IS DELIBERATE.
// `modelIdFromName` falls back to a real hull and logs, which is right when
// one bad name breaks one ship. A role has nothing to fall back to once the
// literal it replaced is gone, so a bad one would un-draw every gate in the
// galaxy in silence.
SOL_TEST(data_defs_role_validation_refuses_rather_than_warns)
{
    // A role naming a model that does not exist.
    {
        DefDatabase db;
        std::string error;
        SOL_REQUIRE(merge(db, kRoleModels, "models.toml", &error));
        SOL_REQUIRE(merge(db, R"(
[[role]]
id = "gate"
model = "gate"

[[role]]
id = "rock"
model = "not_a_model"
)",
                          "models.toml", &error));
        SOL_CHECK(!db.validateRoles(kTestRoles, &error));
        // The message has to name the file, because that is the whole
        // advantage a refusal has over a warning at play time.
        SOL_CHECK(error.find("models.toml") != std::string::npos);
        SOL_CHECK(error.find("not_a_model") != std::string::npos);
    }
    // A required role nobody filled.
    {
        DefDatabase db;
        std::string error;
        SOL_REQUIRE(merge(db, kRoleModels, "models.toml", &error));
        SOL_REQUIRE(merge(db, "[[role]]\nid = \"gate\"\nmodel = \"gate\"\n", "models.toml",
                          &error));
        SOL_CHECK(!db.validateRoles(kTestRoles, &error));
        SOL_CHECK(error.find("rock") != std::string::npos);
    }
    // A role the engine does not ask for. It would otherwise do nothing at
    // all, quietly and forever, which is the failure a strict schema exists
    // to prevent - so it is rejected the same way an unknown KEY is.
    {
        DefDatabase db;
        std::string error;
        SOL_REQUIRE(merge(db, kRoleModels, "models.toml", &error));
        SOL_REQUIRE(merge(db, R"(
[[role]]
id = "gate"
model = "gate"

[[role]]
id = "rock"
model = "asteroid"

[[role]]
id = "cockpti"
model = "gate"
)",
                          "models.toml", &error));
        SOL_CHECK(!db.validateRoles(kTestRoles, &error));
        SOL_CHECK(error.find("cockpti") != std::string::npos);
    }
    // And the empty vocabulary accepts an empty table, so a library with no
    // roles at all is not a broken one.
    {
        DefDatabase db;
        std::string error;
        SOL_CHECK(db.validateRoles({}, &error));
    }
}

// Phase 19 stage C. A weapon and a commodity may each name what their
// drawable is, and every def the base game ships leaves them empty - which is
// the only reason adding the keys is a no-op. Resolution (empty means "the
// role") is game-side and tested in `game.unit`; what belongs here is that the
// schema takes them and still refuses everything else.
SOL_TEST(data_defs_parse_drawable_overrides)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, R"(
[[weapon]]
id = "w.plain"
name = "Plain"
kind = "projectile"

[[weapon]]
id = "w.fancy"
name = "Fancy"
kind = "projectile"
model = "tracer"

[[commodity]]
id = "c.plain"
name = "Plain"

[[commodity]]
id = "c.ice"
name = "Ice"
model = "ice_rock"
chunk_model = "ice_shard"
)",
                      "defs.toml", &error));

    // Absent is EMPTY, not a name - the fallback is a decision the game makes
    // at resolve time, so the parser must not invent one here.
    SOL_CHECK(db.findWeapon("w.plain")->model.empty());
    SOL_CHECK(db.findWeapon("w.fancy")->model == "tracer");
    SOL_CHECK(db.findCommodity("c.plain")->model.empty());
    SOL_CHECK(db.findCommodity("c.plain")->chunkModel.empty());
    SOL_CHECK(db.findCommodity("c.ice")->model == "ice_rock");
    SOL_CHECK(db.findCommodity("c.ice")->chunkModel == "ice_shard");

    // ⚑ The rock and the chunk are SEPARATE keys because they are separate
    // drawables at separate scales - a chunk is not a small rock.
    SOL_CHECK(db.findCommodity("c.ice")->model != db.findCommodity("c.ice")->chunkModel);

    // Still a strict schema: a plausible near-miss is an error, not a silent
    // no-op, which is the whole reason these are typed keys and not free text.
    SOL_CHECK(!merge(db, "[[commodity]]\nid = \"c\"\nname = \"C\"\nchunk = \"x\"\n", "d.toml",
                     &error));
    SOL_CHECK(!merge(db, "[[weapon]]\nid = \"w\"\nname = \"W\"\nkind = \"projectile\"\n"
                         "bolt_model = \"x\"\n",
                     "d.toml", &error));
}

SOL_TEST(data_defs_model_validation_errors)
{
    DefDatabase db;
    std::string error;

    // A model with no mesh or no texture cannot be drawn, so it fails at load
    // rather than becoming an invisible entity somebody debugs later.
    SOL_CHECK(!merge(db, "[[model]]\nid = \"a\"\ntexture = \"t\"\n", "m.toml", &error));
    SOL_CHECK(error.find("mesh") != std::string::npos);
    SOL_CHECK(!merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\n", "m.toml", &error));
    SOL_CHECK(error.find("texture") != std::string::npos);

    SOL_CHECK(!merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nradius = 0.0\n",
                     "m.toml", &error));
    // An avoidance sphere inside the collision sphere is steering that clears
    // the obstacle it is about to hit, so it is rejected rather than clamped.
    SOL_CHECK(!merge(db,
                     "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nradius = "
                     "10.0\navoid_radius = 5.0\n",
                     "m.toml", &error));
    SOL_CHECK(!merge(db,
                     "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nemissive = -0.5\n",
                     "m.toml", &error));
    // solid takes a bool, not the 0/1 a TOML author might reach for.
    SOL_CHECK(!merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nsolid = 0\n",
                     "m.toml", &error));
    // Strict schema: a typo dies at load, not as a mis-sized collision sphere.
    SOL_CHECK(!merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nscale = 2.0\n",
                     "m.toml", &error));

    SOL_CHECK(db.models().empty());

    // A mod layer replaces a model in place, so the index a spawned entity is
    // holding still names the same thing after a reload.
    SOL_CHECK(merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\n", "base.toml",
                    &error));
    SOL_CHECK(merge(db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\n", "base.toml",
                    &error));
    SOL_CHECK(merge(db, "[[model]]\nid = \"a\"\nmesh = \"m2\"\ntexture = \"t\"\nradius = 4.0\n",
                    "mod.toml", &error));
    SOL_CHECK(db.models().size() == 2);
    SOL_CHECK(db.modelIndex("a") == 0);
    SOL_CHECK(db.modelIndex("b") == 1);
    SOL_CHECK(db.findModel("a")->mesh == "m2");
    SOL_CHECK(db.findModel("a")->radius == 4.0f);
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
