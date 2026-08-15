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
