#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::factionLegalityOf;
using sol::assets::Legality;
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
mount = "fixed"
size = "small"
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

[[component]]
id = "sol.navy_shield"
name = "Navy Shield"
mount = "shield"
size = "small"
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

    const sol::assets::ComponentDef* gated = db.findComponent("sol.navy_shield");
    SOL_CHECK(gated != nullptr);
    SOL_CHECK(gated->gate.factions.size() == 1 && gated->gate.factions[0] == "sol.navy");
    SOL_CHECK(gated->gate.minRep == 20.0f);
    // Ungated defs default open.
    SOL_CHECK(navy->shipsRaider.size() == 1);

    // Both sides declared and agreeing: valid.
    SOL_CHECK(db.validateFactions(&error));

    // Bad kind, out-of-range weights, malformed relations: schema errors.
    SOL_CHECK(!merge(db, "[[faction]]\nid = \"f\"\nname = \"F\"\nkind = \"guild\"\n", "bad.toml", &error));
    SOL_CHECK(error.find("kind") != std::string::npos);
    SOL_CHECK(!merge(db, "[[faction]]\nid = \"f\"\nname = \"F\"\naggression = 1.5\n", "bad.toml", &error));
    SOL_CHECK(!merge(
        db, "[[faction]]\nid = \"f\"\nname = \"F\"\nrelations = [\"sol.navy\"]\n", "bad.toml", &error));
    SOL_CHECK(error.find("id:standing") != std::string::npos);
    // Malformed and negative station_bias entries are schema errors; an unknown
    // station id is NOT (a mod may remove an archetype, so it warns downstream).
    SOL_CHECK(!merge(
        db, "[[faction]]\nid = \"f\"\nname = \"F\"\nstation_bias = [\"sol.st\"]\n", "bad.toml", &error));
    SOL_CHECK(error.find("id:weight") != std::string::npos);
    SOL_CHECK(!merge(
        db, "[[faction]]\nid = \"f\"\nname = \"F\"\nstation_bias = [\"sol.st:-1\"]\n", "bad.toml", &error));
    SOL_CHECK(merge(
        db, "[[faction]]\nid = \"f2\"\nname = \"F2\"\nstation_bias = [\"sol.nope:2\"]\n", "ok.toml", &error));
    SOL_CHECK(!merge(db,
                     "[[component]]\nid = \"m\"\nname = \"M\"\nmount = \"utility\"\nsize = \"small\"\n"
                     "min_rep = 150\n",
                     "bad.toml",
                     &error));
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
    SOL_CHECK(merge(oneSided,
                    "[[faction]]\nid = \"sol.a\"\nname = \"A\"\nrelations = [\"sol.gone:-40\"]\n",
                    "factions.toml",
                    &error));
    SOL_CHECK(oneSided.validateFactions(&error));
}

// ⚑⚑⚑ THE THREE STATES A ROSTER CELL CAN BE IN (Phase 32 stage C), AND UNTIL
// THIS STAGE THERE WERE TWO. A cell with hulls, a cell the faction DECLARED it
// fields nothing for, and a cell nobody has decided about - the last two were
// the same empty vector, and four call sites in the game each substituted a
// different roster for it. The whole point of the key is that `buildsNo` and
// `empty()` are now different questions.
SOL_TEST(data_defs_faction_declares_a_cell_it_builds_nothing_for)
{
    DefDatabase db;
    std::string error;
    const char* doc = R"(
[[faction]]
id = "sol.clan"
name = "Clan"
kind = "pirate"
ships_raider = ["sol.shuttle"]
builds_no = ["patrol", "trader"]

[[faction]]
id = "sol.undecided"
name = "Undecided"
ships_raider = ["sol.shuttle"]
)";
    SOL_CHECK(merge(db, doc, "factions.toml", &error));

    const sol::assets::FactionDef* clan = db.findFaction("sol.clan");
    SOL_REQUIRE(clan != nullptr);
    SOL_CHECK(clan->buildsNo[static_cast<std::size_t>(sol::assets::RosterCell::Patrol)]);
    SOL_CHECK(clan->buildsNo[static_cast<std::size_t>(sol::assets::RosterCell::Trader)]);
    // The cell it DOES field is untouched, which is what makes this a
    // per-cell declaration rather than a flag on the faction.
    SOL_CHECK(!clan->buildsNo[static_cast<std::size_t>(sol::assets::RosterCell::Raider)]);
    SOL_CHECK(clan->shipsRaider.size() == 1);

    // ⚑ The discriminating pair: this faction's patrol cell is EMPTY exactly
    // like the clan's, and it has declared nothing. A reader that only asked
    // `shipsPatrol.empty()` could not tell these two apart - which is the bug
    // the key exists to fix, so the test asserts both halves of it.
    const sol::assets::FactionDef* undecided = db.findFaction("sol.undecided");
    SOL_REQUIRE(undecided != nullptr);
    SOL_CHECK(undecided->shipsPatrol.empty() && clan->shipsPatrol.empty());
    SOL_CHECK(!undecided->buildsNo[static_cast<std::size_t>(sol::assets::RosterCell::Patrol)]);

    // Nothing declared is the default, and a faction that says nothing about
    // any cell is every faction that shipped before this stage.
    for (std::size_t i = 0; i < sol::assets::kRosterCellCount; ++i) {
        SOL_CHECK(!undecided->buildsNo[i]);
    }
}

// A cell named in `builds_no` AND listed is refused rather than resolved, and
// a cell name that is not one of the three is refused too. Both are stage A's
// ruling in another place: the vocabulary is words so that a wrong one has a
// wrong spelling, and a contradiction is an author who changed their mind in
// one place of two.
SOL_TEST(data_defs_faction_cannot_both_field_and_refuse_a_cell)
{
    DefDatabase both;
    std::string error;
    SOL_CHECK(!merge(both,
                     R"(
[[faction]]
id = "sol.a"
name = "A"
ships_trader = ["sol.freighter"]
builds_no = ["trader"]
)",
                     "factions.toml",
                     &error));
    SOL_CHECK(error.find("builds_no") != std::string::npos);

    // The same row without the contradiction parses, so the refusal above is
    // about the pair and not about either key on its own.
    DefDatabase one;
    SOL_CHECK(merge(one,
                    R"(
[[faction]]
id = "sol.a"
name = "A"
builds_no = ["trader"]
)",
                    "factions.toml",
                    &error));

    DefDatabase typo;
    SOL_CHECK(!merge(typo,
                     R"(
[[faction]]
id = "sol.a"
name = "A"
builds_no = ["trade"]
)",
                     "factions.toml",
                     &error));
    SOL_CHECK(error.find("roster cell") != std::string::npos);
}

// ⚑⚑ A ROSTER NAMING A SHIP THAT DOES NOT EXIST REFUSES THE LOAD, and it has
// to be a separate pass for the same reason `validateMaterials` is: the ship
// may legitimately arrive in a later layer. Before this, `spawnWing` found it
// at spawn time, warned, and abandoned the whole wing - so one wrong character
// was a patrol that silently never appeared in a system nobody might visit.
SOL_TEST(data_defs_faction_roster_must_name_ships_that_exist)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kBaseShips, "ships.toml"));
    SOL_CHECK(merge(db,
                    R"(
[[faction]]
id = "sol.a"
name = "A"
ships_patrol = ["sol.shuttle"]
)",
                    "factions.toml",
                    &error));
    SOL_CHECK(db.validateRosters(&error));

    // The layering half: a roster naming a ship no layer defines is what the
    // pass exists to catch, and the message names the faction, the cell and
    // the id, because a validation error a modder cannot act on is a crash
    // with better manners.
    DefDatabase stale;
    SOL_CHECK(merge(stale, kBaseShips, "ships.toml"));
    SOL_CHECK(merge(stale,
                    R"(
[[faction]]
id = "sol.a"
name = "A"
ships_raider = ["sol.gunship"]
)",
                    "factions.toml",
                    &error));
    SOL_CHECK(!stale.validateRosters(&error));
    SOL_CHECK(error.find("sol.gunship") != std::string::npos);
    SOL_CHECK(error.find("raider") != std::string::npos);

    // ⚑ A DECLARED-EMPTY CELL IS NOT A BROKEN ONE, which is this stage's whole
    // exit criterion stated as a test: the faction below fields nothing at all
    // and validates, while the one above names one wrong hull and does not.
    DefDatabase none;
    SOL_CHECK(merge(none, kBaseShips, "ships.toml"));
    SOL_CHECK(merge(none,
                    R"(
[[faction]]
id = "sol.a"
name = "A"
builds_no = ["patrol", "raider", "trader"]
)",
                    "factions.toml",
                    &error));
    SOL_CHECK(none.validateRosters(&error));
}

// --- Phase 33 stage B: the material tree's vocabulary ------------------------

// ⚑⚑ A WORD, NOT A NUMBER, AND THE ROUND TRIP IS WHAT SAYS SO. Same shape as
// `HullClass` and `MountKind` before it, and for the reason Phase 32 stage A's
// checkpoint settled: gdd.md 6 numbers its tiers T0..T3, and a `tier = 2` where
// the author meant 3 is a typo no schema can ever catch while `refined` and
// `component` cannot be confused by a slipped digit.
// ⚑⚑⚑ THE OTHER HALF OF THE STORAGE MECHANISM (Phase 34 stage D). Stage A wrote
// the `GoodsClass` enum and said in as many words that "which class each
// commodity is does not exist yet: it is a key on `[[commodity]]` and it belongs
// to stage D". This is that key, and the thing worth pinning is the ONE place it
// deliberately differs from `tier` beside it: an unsaid tier is a third state, an
// unsaid class is `bulk`. A good outside the material tree is a real answer; a
// good nobody can warehouse anywhere is not - it would vanish from every market
// in the galaxy, which is the silent-disappearance failure this project has now
// named three times.
SOL_TEST(data_defs_commodity_goods_class_defaults_to_the_warehouse)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
[[commodity]]
id = "sol.unsaid"
name = "Unsaid"

[[commodity]]
id = "sol.chilled"
name = "Chilled"
tier = "consumer"
goods_class = "cryo"

[[commodity]]
id = "sol.nasty"
name = "Nasty"
goods_class = "hazardous"
)",
                    "commodities.toml",
                    &error));
    const sol::assets::CommodityDef* unsaid = db.findCommodity("sol.unsaid");
    const sol::assets::CommodityDef* chilled = db.findCommodity("sol.chilled");
    const sol::assets::CommodityDef* nasty = db.findCommodity("sol.nasty");
    SOL_REQUIRE(unsaid != nullptr && chilled != nullptr && nasty != nullptr);

    // Unsaid is bulk AND says nobody said so - both halves matter, because the
    // flag is what a later reader would use to tell a decision from a default.
    SOL_CHECK(!unsaid->hasGoodsClass);
    SOL_CHECK(unsaid->goodsClass == sol::assets::GoodsClass::Bulk);
    SOL_CHECK(chilled->hasGoodsClass);
    SOL_CHECK(chilled->goodsClass == sol::assets::GoodsClass::Cryo);
    SOL_CHECK(nasty->goodsClass == sol::assets::GoodsClass::Hazardous);
    // A class is not a tier: `nasty` declared one and not the other.
    SOL_CHECK(!nasty->hasTier);

    DefDatabase bad;
    SOL_CHECK(!merge(bad,
                     R"(
[[commodity]]
id = "sol.x"
name = "X"
goods_class = "volatile"
)",
                     "commodities.toml",
                     &error));
    SOL_CHECK(error.find("not a goods class") != std::string::npos);
    // The classes are the module vocabulary's, spelled the same way.
    DefDatabase mixed;
    SOL_CHECK(!merge(mixed,
                     R"(
[[commodity]]
id = "sol.y"
name = "Y"
goods_class = "raw"
)",
                     "commodities.toml",
                     &error));
}

SOL_TEST(data_defs_commodity_tier_is_a_word_and_round_trips)
{
    for (std::size_t i = 0; i < sol::assets::kCommodityTierCount; ++i) {
        const auto tier = static_cast<sol::assets::CommodityTier>(i);
        sol::assets::CommodityTier parsed{};
        SOL_CHECK(sol::assets::parseCommodityTier(sol::assets::commodityTierName(tier), parsed));
        SOL_CHECK(parsed == tier);
    }
    sol::assets::CommodityTier ignored{};
    SOL_CHECK(!sol::assets::parseCommodityTier("t2", ignored));
    SOL_CHECK(!sol::assets::parseCommodityTier("components", ignored)); // singular, as authored
    SOL_CHECK(!sol::assets::parseCommodityTier("", ignored));

    // Absent is its own state rather than a default, exactly as `class` is on a
    // hull: `raw` is a real answer and cannot double as "nobody said".
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
[[commodity]]
id = "sol.silent"
name = "Silent"

[[commodity]]
id = "sol.spoken"
name = "Spoken"
tier = "component"
)",
                    "commodities.toml",
                    &error));
    const sol::assets::CommodityDef* silent = db.findCommodity("sol.silent");
    const sol::assets::CommodityDef* spoken = db.findCommodity("sol.spoken");
    SOL_REQUIRE(silent != nullptr && spoken != nullptr);
    SOL_CHECK(!silent->hasTier);
    SOL_CHECK(spoken->hasTier);
    SOL_CHECK(spoken->tier == sol::assets::CommodityTier::Component);

    DefDatabase bad;
    SOL_CHECK(!merge(bad,
                     R"(
[[commodity]]
id = "sol.wrong"
name = "Wrong"
tier = "t1"
)",
                     "commodities.toml",
                     &error));
    SOL_CHECK(error.find("t1") != std::string::npos);
}

// ⚑⚑ ONE DIRECTION ONLY, AND THE CONVERSE IS ASSERTED TO STAY UNCHECKED.
// gdd.md 6 puts everything that comes out of a rock at T0, so an ore weight on a
// `refined` row is an authoring slip. But T0 is wider than mining - salvage is
// raw and comes off a hull, ices and gases are raw and come out of an envelope -
// so a raw good with NO ore weight has to stay legal, and the second half of
// this test is what stops somebody "completing" the rule.
SOL_TEST(data_defs_only_a_raw_commodity_can_be_mined)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
[[commodity]]
id = "sol.rock"
name = "Rock"
tier = "raw"
ore_weight_fringe = 1.0

[[commodity]]
id = "sol.scrap"
name = "Scrap"
tier = "raw"
)",
                    "commodities.toml",
                    &error));

    DefDatabase bad;
    SOL_CHECK(!merge(bad,
                     R"(
[[commodity]]
id = "sol.alloy"
name = "Alloy"
tier = "refined"
ore_weight_core = 0.5
)",
                     "commodities.toml",
                     &error));
    SOL_CHECK(error.find("raw") != std::string::npos);

    // An untiered commodity with an ore weight is what every shipped def looked
    // like before this key existed, and it still loads.
    DefDatabase legacy;
    SOL_CHECK(merge(legacy,
                    R"(
[[commodity]]
id = "sol.ore"
name = "Ore"
ore_weight_core = 1.0
)",
                    "commodities.toml",
                    &error));
}

// ⚑⚑⚑ A GATE THAT CAN NEVER OPEN IS THE WORST SHAPE A CONTENT ERROR TAKES,
// which is `validateRosters`'s finding one phase over: the item is simply absent
// from every station in the galaxy and nothing anywhere says why. A player would
// read it as the economy working.
SOL_TEST(data_defs_a_catalog_requirement_names_a_real_commodity)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
[[commodity]]
id = "sol.plate"
name = "Plate"
tier = "component"

[[component]]
id = "sol.armor"
name = "Armor"
mount = "utility"
size = "small"
requires = "sol.plate"
)",
                    "defs.toml",
                    &error));
    SOL_CHECK(db.validateCatalogGates(&error));
    const sol::assets::ComponentDef* armor = db.findComponent("sol.armor");
    SOL_REQUIRE(armor != nullptr);
    SOL_CHECK(armor->gate.requiresCommodity == "sol.plate");

    DefDatabase stale;
    SOL_CHECK(merge(stale,
                    R"(
[[component]]
id = "sol.armor"
name = "Armor"
mount = "utility"
size = "small"
requires = "sol.plate"
)",
                    "components.toml",
                    &error));
    SOL_CHECK(!stale.validateCatalogGates(&error));
    SOL_CHECK(error.find("sol.plate") != std::string::npos);
    SOL_CHECK(error.find("component") != std::string::npos);

    // ⚑ And an ungated def is not a broken one: the key is optional, and every
    // def written before this stage carries no requirement at all.
    DefDatabase plain;
    SOL_CHECK(merge(plain,
                    R"(
[[component]]
id = "sol.armor"
name = "Armor"
mount = "utility"
size = "small"
)",
                    "components.toml",
                    &error));
    SOL_CHECK(plain.validateCatalogGates(&error));
    SOL_REQUIRE(plain.findComponent("sol.armor") != nullptr);
    SOL_CHECK(plain.findComponent("sol.armor")->gate.requiresCommodity.empty());
}

// ⚑⚑⚑ THE LISTS LIVE ON THE FACTION AND THAT IS THE FEATURE, NOT AN
// IMPLEMENTATION CHOICE (gdd.md §13: "Cargo is never intrinsically illegal;
// jurisdictions are"). The assertion worth making at this layer is therefore
// not "the key parses" but "the SAME id gets two different answers out of two
// different factions", which is the whole sentence the phase exits on.
SOL_TEST(data_defs_the_same_good_is_contraband_to_one_faction_and_licensed_to_another)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
[[commodity]]
id = "sol.salvage"
name = "Salvage"
tier = "raw"

[[faction]]
id = "sol.hegemony"
name = "Ironstar Hegemony"
kind = "major"
contraband = ["sol.salvage"]

[[faction]]
id = "sol.navy"
name = "Solar Navy"
kind = "major"
restricted = ["sol.salvage"]

[[faction]]
id = "sol.compact"
name = "Frontier Compact"
kind = "major"
)",
                    "defs.toml",
                    &error));
    SOL_CHECK(db.validateLegality(&error));

    const sol::assets::FactionDef* hegemony = db.findFaction("sol.hegemony");
    const sol::assets::FactionDef* navy = db.findFaction("sol.navy");
    const sol::assets::FactionDef* compact = db.findFaction("sol.compact");
    SOL_REQUIRE(hegemony != nullptr && navy != nullptr && compact != nullptr);

    SOL_CHECK(factionLegalityOf(*hegemony, "sol.salvage") == Legality::Contraband);
    SOL_CHECK(factionLegalityOf(*navy, "sol.salvage") == Legality::Restricted);
    // ⚑ A faction that declares nothing answers `Legal`, and that is a real
    // answer rather than a missing one - it is how a def says "we do not mind".
    // The absence that means something else, `Unpoliced`, is a fact about a
    // system and this layer deliberately cannot produce it.
    SOL_CHECK(factionLegalityOf(*compact, "sol.salvage") == Legality::Legal);
    SOL_CHECK(factionLegalityOf(*hegemony, "sol.food") == Legality::Legal);
}

// The same shape as `data_defs_a_catalog_requirement_names_a_real_commodity`,
// and it refuses for a worse reason: a gate that can never open hides an item
// and a player eventually notices the shelf, while a law that can never fire
// looks exactly like a patrol deciding to let you off.
SOL_TEST(data_defs_a_legality_list_names_a_real_commodity)
{
    DefDatabase stale;
    std::string error;
    SOL_CHECK(merge(stale,
                    R"(
[[faction]]
id = "sol.hegemony"
name = "Ironstar Hegemony"
kind = "major"
contraband = ["sol.stims"]
)",
                    "factions.toml",
                    &error));
    SOL_CHECK(!stale.validateLegality(&error));
    SOL_CHECK(error.find("sol.stims") != std::string::npos);
    SOL_CHECK(error.find("contraband") != std::string::npos);

    // The restricted list is checked too, and says which key it was.
    DefDatabase other;
    SOL_CHECK(merge(other,
                    R"(
[[faction]]
id = "sol.navy"
name = "Solar Navy"
kind = "major"
restricted = ["sol.stims"]
)",
                    "factions.toml",
                    &error));
    SOL_CHECK(!other.validateLegality(&error));
    SOL_CHECK(error.find("restricted") != std::string::npos);

    // ⚑ And a faction with no lists at all is not a broken one. Both keys are
    // optional and every faction written before this stage carries neither.
    DefDatabase plain;
    SOL_CHECK(merge(plain,
                    R"(
[[faction]]
id = "sol.guild"
name = "Freight Guild"
kind = "major"
)",
                    "factions.toml",
                    &error));
    SOL_CHECK(plain.validateLegality(&error));
    SOL_REQUIRE(plain.findFaction("sol.guild") != nullptr);
    SOL_CHECK(plain.findFaction("sol.guild")->contraband.empty());
    SOL_CHECK(plain.findFaction("sol.guild")->restricted.empty());
}

// ⚑⚑ REFUSED RATHER THAN RESOLVED, the same bargain `builds_no` makes. Picking
// a winner quietly would make the difference between "ten years in a labour
// camp" and "show me the paper" depend on which loop in the loader runs first.
SOL_TEST(data_defs_a_good_cannot_be_both_contraband_and_licensed)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(!merge(db,
                     R"(
[[commodity]]
id = "sol.salvage"
name = "Salvage"
tier = "raw"

[[faction]]
id = "sol.hegemony"
name = "Ironstar Hegemony"
kind = "major"
contraband = ["sol.salvage"]
restricted = ["sol.salvage"]
)",
                     "factions.toml",
                     &error));
    SOL_CHECK(error.find("sol.salvage") != std::string::npos);
    SOL_CHECK(error.find("contraband") != std::string::npos);
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
    SOL_CHECK(!merge(db, "[[ship]]\nid = \"s\"\nname = \"S\"\nmax_speed = \"fast\"\n", "bad.toml", &error));
    SOL_CHECK(error.find("must be a number") != std::string::npos);

    // Bad weapon kind.
    error.clear();
    SOL_CHECK(!merge(db,
                     "[[weapon]]\nid = \"w\"\nname = \"W\"\nkind = \"beam\"\n"
                     "mount = \"fixed\"\nsize = \"small\"\n",
                     "bad.toml",
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
    SOL_CHECK(!merge(db,
                     R"(
[[station]]
id = "sol.bad"
name = "Bad"
produces = ["sol.food"]
)",
                     "bad.toml",
                     &error));
    SOL_CHECK(error.find("id:rate") != std::string::npos);
    SOL_CHECK(db.findStation("sol.bad") == nullptr);

    // A station naming its own model, which is the whole point of the key.
    SOL_CHECK(merge(db,
                    R"(
[[station]]
id = "sol.station_relay"
name = "Relay"
model = "gate"
)",
                    "relay.toml",
                    &error));
    const sol::assets::StationDef* relay = db.findStation("sol.station_relay");
    SOL_CHECK(relay != nullptr);
    SOL_CHECK(relay->model == "gate");
    // An empty one is refused rather than silently meaning "no model at all",
    // which would draw nothing and read as a missing station.
    SOL_CHECK(!merge(db,
                     R"(
[[station]]
id = "sol.station_void"
name = "Void"
model = ""
)",
                     "void.toml",
                     &error));
    SOL_CHECK(error.find("model") != std::string::npos);
    SOL_CHECK(!merge(db,
                     R"(
[[station]]
id = "sol.bad2"
name = "Bad"
produces = ["sol.food:-1"]
)",
                     "bad2.toml",
                     &error));
}

SOL_TEST(data_defs_parse_sounds)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
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
                    "sounds.toml",
                    &error));

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
    SOL_CHECK(!merge(db, "[[sound]]\nid = \"a\"\nasset = \"x\"\npitch_jitter = 0.9\n", "s.toml", &error));
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
    SOL_CHECK(merge(db,
                    R"(
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
                    "models.toml",
                    &error));

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
    SOL_CHECK(merge(db,
                    R"(
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
                    "models.toml",
                    &error));

    const auto* membrane = db.findModel("membrane");
    SOL_REQUIRE(membrane != nullptr);
    SOL_CHECK(membrane->translucent);
    SOL_CHECK(std::abs(membrane->alpha - 0.30f) < 1e-6f);
    SOL_CHECK(std::abs(membrane->emissive - 0.35f) < 1e-6f);
    SOL_CHECK(!membrane->solid);

    // Coverage outside 0..1 is meaningless under premultiplied blending and
    // would read as a wrongly-lit model rather than as a bad number, so it dies
    // at load while the file name is still in hand.
    SOL_CHECK(
        !merge(db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\nalpha = 1.5\n", "m.toml", &error));
    SOL_CHECK(
        !merge(db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\nalpha = -0.1\n", "m.toml", &error));
    // translucent takes a bool, for the same reason solid does.
    SOL_CHECK(!merge(
        db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\ntranslucent = 1\n", "m.toml", &error));
}

// Phase 25 stage A. A `[[material]]` row owns how a surface is drawn, and every
// `[[model]]` row reaches one - either by naming it or by having one derived
// from the four keys it used to carry. The three cases below are the whole
// migration: a row WITH a material, a row WITHOUT, and a row with BOTH.
SOL_TEST(data_defs_model_reaches_a_material_by_naming_or_by_synthesis)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
[[material]]
id = "sol.hull"
texture = "hull"

[[model]]
id = "ship"
mesh = "ship"
material = "sol.hull"
radius = 8.0

[[model]]
id = "cockpit"
mesh = "cockpit"
texture = "cockpit"
radius = 8.0
emissive = 0.10
solid = false
)",
                    "models.toml",
                    &error));
    SOL_CHECK(db.validateMaterials(&error));

    // WITH. The row carries none of the four surface keys itself; the material
    // it names carries all of them.
    const auto* ship = db.findModel("ship");
    SOL_REQUIRE(ship != nullptr);
    SOL_CHECK(ship->material == "sol.hull");
    SOL_CHECK(ship->texture.empty());
    SOL_REQUIRE(ship->materialIndex < db.materials().size());
    const sol::assets::MaterialDef& hull = db.materials()[ship->materialIndex];
    SOL_CHECK(hull.id == "sol.hull");
    SOL_CHECK(hull.texture == "hull");
    SOL_CHECK(!hull.synthesised);
    SOL_CHECK(db.materialIndex("sol.hull") == ship->materialIndex);

    // WITHOUT. The four keys stay legal and stay where they were written; what
    // changed is that the renderer now reads them off a derived row. This is
    // what makes the stage a no-op for every def file already committed and
    // every mod written before it.
    const auto* cockpit = db.findModel("cockpit");
    SOL_REQUIRE(cockpit != nullptr);
    SOL_CHECK(cockpit->material.empty());
    SOL_REQUIRE(cockpit->materialIndex < db.materials().size());
    const sol::assets::MaterialDef& derived = db.materials()[cockpit->materialIndex];
    SOL_CHECK(derived.id == "sol.auto.cockpit");
    SOL_CHECK(derived.texture == "cockpit");
    SOL_CHECK(std::abs(derived.emissive - 0.10f) < 1e-6f);
    SOL_CHECK(!derived.translucent);
    SOL_CHECK(derived.alpha == 1.0f);
    SOL_CHECK(derived.synthesised);
    // Authored rows first, derived ones after, so an author's indices do not
    // move when somebody else adds a model.
    SOL_CHECK(ship->materialIndex < cockpit->materialIndex);
    SOL_CHECK(db.materials().size() == 2);

    // BOTH, and it is REFUSED rather than resolved by precedence. A row
    // carrying a material and a surface key gives a reader no way to tell
    // which half is doing anything, and the error names the key to move.
    SOL_CHECK(!merge(db,
                     R"(
[[model]]
id = "bad"
mesh = "ship"
material = "sol.hull"
texture = "checker"
)",
                     "bad.toml",
                     &error));
    SOL_CHECK(error.find("texture") != std::string::npos);
    SOL_CHECK(error.find("sol.hull") != std::string::npos);
    for (const char* key : {"emissive = 0.5", "translucent = true", "alpha = 0.5"}) {
        const std::string toml = std::string("[[model]]\nid = \"bad\"\nmesh = \"ship\"\nmaterial = "
                                             "\"sol.hull\"\n") +
                                 key + "\n";
        SOL_CHECK(!merge(db, toml.c_str(), "bad.toml", &error));
    }
    // A failed merge leaves the previous state intact, derived rows included.
    SOL_CHECK(db.models().size() == 2);
    SOL_CHECK(db.materials().size() == 2);
}

// The cross-def half, and it is a SEPARATE pass for the same reason
// `validateRoles` is: which layer a material lives in is not the merge's
// business, and refusing at parse time would forbid the perfectly ordinary
// case below.
SOL_TEST(data_defs_material_validation_and_layer_order)
{
    DefDatabase db;
    std::string error;
    // A model naming a material NOT YET MERGED parses. It has to: a mod may
    // ship its models in one file and its materials in another, and
    // `mergeDirectory` reads them in sorted order.
    SOL_CHECK(merge(
        db, "[[model]]\nid = \"ship\"\nmesh = \"ship\"\nmaterial = \"sol.hull\"\n", "a_models.toml", &error));
    SOL_CHECK(db.findModel("ship")->materialIndex == sol::assets::kNoMaterial);
    // ... and it is `validateMaterials`, once every layer is in, that refuses.
    SOL_CHECK(!db.validateMaterials(&error));
    SOL_CHECK(error.find("ship") != std::string::npos);
    SOL_CHECK(error.find("sol.hull") != std::string::npos);
    SOL_CHECK(error.find("a_models.toml") != std::string::npos);

    SOL_CHECK(merge(db, "[[material]]\nid = \"sol.hull\"\ntexture = \"hull\"\n", "b_materials.toml", &error));
    SOL_CHECK(db.validateMaterials(&error));
    SOL_CHECK(db.findModel("ship")->materialIndex == db.materialIndex("sol.hull"));

    // The prefix the synthesis owns is reserved, because a derived row is
    // rebuilt after every merge and would otherwise shadow an authored one -
    // a name resolving to something no file says.
    SOL_CHECK(!merge(db, "[[material]]\nid = \"sol.auto.ship\"\ntexture = \"hull\"\n", "m.toml", &error));
    SOL_CHECK(error.find("reserved") != std::string::npos);

    // The rest of the schema, in the shape the model's own keys had.
    SOL_CHECK(!merge(db, "[[material]]\nid = \"m\"\n", "m.toml", &error)); // texture is required
    SOL_CHECK(!merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nalpha = 1.5\n", "m.toml", &error));
    SOL_CHECK(!merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nemissive = -0.1\n", "m.toml", &error));
    SOL_CHECK(!merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nshader = \"x\"\n", "m.toml", &error));
    // An empty name is a mistake rather than "no material": omitting the key
    // is how a row says it describes its own surface.
    SOL_CHECK(!merge(db, "[[model]]\nid = \"e\"\nmesh = \"m\"\nmaterial = \"\"\n", "m.toml", &error));
}

// The derived rows are rebuilt from the models after every merge, which is what
// keeps a caller from ever seeing a half-resolved database - and what keeps a
// layer that edits a model row from leaving the old surface behind.
SOL_TEST(data_defs_synthesised_materials_are_rebuilt_by_a_later_layer)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    "[[model]]\nid = \"gate\"\nmesh = \"gate\"\ntexture = \"hull\"\nradius = 106.7\n",
                    "base.toml",
                    &error));
    SOL_CHECK(db.materials().size() == 1);
    SOL_CHECK(db.materials()[0].texture == "hull");

    // A mod replaces the row wholesale (that is what `mergeDef` does), so the
    // material derived from the old values must not survive it.
    SOL_CHECK(merge(db,
                    "[[model]]\nid = \"gate\"\nmesh = \"gate\"\ntexture = \"checker\"\nradius = "
                    "106.7\ntranslucent = true\nalpha = 0.5\n",
                    "mod.toml",
                    &error));
    SOL_CHECK(db.models().size() == 1);
    SOL_CHECK(db.materials().size() == 1); // rebuilt, not appended to
    const sol::assets::MaterialDef& derived = db.materials()[db.findModel("gate")->materialIndex];
    SOL_CHECK(derived.id == "sol.auto.gate");
    SOL_CHECK(derived.texture == "checker");
    SOL_CHECK(derived.translucent);
    SOL_CHECK(std::abs(derived.alpha - 0.5f) < 1e-6f);

    // And a row that switches to naming a material drops its derived one
    // entirely rather than leaving an orphan behind.
    SOL_CHECK(merge(db,
                    "[[material]]\nid = \"sol.film\"\ntexture = \"hull\"\ntranslucent = true\nalpha = "
                    "0.3\n\n[[model]]\nid = \"gate\"\nmesh = \"gate\"\nmaterial = \"sol.film\"\nradius = "
                    "106.7\n",
                    "mod2.toml",
                    &error));
    SOL_CHECK(db.validateMaterials(&error));
    SOL_CHECK(db.materials().size() == 1);
    SOL_CHECK(db.materials()[0].id == "sol.film");
    SOL_CHECK(db.findModel("gate")->materialIndex == 0);
}

// Phase 25 stage B. A material names its shader pair and its pipeline state,
// and the three state keys are SEEDED from `translucent` before being
// overridden - which is what makes Phase 12's hardcoded variant a default
// instead of a branch, and what lets a row want blending without giving up
// its depth write.
SOL_TEST(data_defs_material_shaders_and_pipeline_state)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db,
                    R"(
[[material]]
id = "sol.hull"
texture = "hull"

[[material]]
id = "sol.film"
texture = "hull"
fragment_shader = "membrane"
translucent = true
alpha = 0.3

[[material]]
id = "sol.exhaust"
texture = "hull"
vertex_shader = "billboard"
fragment_shader = "glow"
blend = "additive"
depth_write = false
cull = false

[[material]]
id = "sol.decal"
texture = "hull"
translucent = true
depth_write = true
)",
                    "materials.toml",
                    &error));

    // Says nothing: the stock pair and the opaque state, which is exactly the
    // pipeline this engine had before a material could name one.
    const auto* hull = db.findMaterial("sol.hull");
    SOL_REQUIRE(hull != nullptr);
    SOL_CHECK(hull->vertexShader == "mesh");
    SOL_CHECK(hull->fragmentShader == "mesh");
    SOL_CHECK(hull->blend == sol::assets::MaterialBlend::Opaque);
    SOL_CHECK(hull->depthTest);
    SOL_CHECK(hull->depthWrite);
    SOL_CHECK(hull->cullBackFaces);

    // ⚑ Brings only a FRAGMENT stage, which is the common case and the reason
    // there are two keys rather than one stem - a byte-identical copy of
    // mesh.vert would be a file nothing binds.
    const auto* film = db.findMaterial("sol.film");
    SOL_REQUIRE(film != nullptr);
    SOL_CHECK(film->vertexShader == "mesh");
    SOL_CHECK(film->fragmentShader == "membrane");
    // Phase 12's three fields, arriving as defaults rather than as a branch.
    SOL_CHECK(film->blend == sol::assets::MaterialBlend::Alpha);
    SOL_CHECK(!film->depthWrite);
    SOL_CHECK(!film->cullBackFaces);
    SOL_CHECK(film->depthTest);

    // Both stages replaced, and state said outright rather than implied.
    const auto* exhaust = db.findMaterial("sol.exhaust");
    SOL_REQUIRE(exhaust != nullptr);
    SOL_CHECK(exhaust->vertexShader == "billboard");
    SOL_CHECK(exhaust->fragmentShader == "glow");
    SOL_CHECK(exhaust->blend == sol::assets::MaterialBlend::Additive);
    SOL_CHECK(!exhaust->depthWrite);

    // ⚑⚑ THE CASE THE SEEDING EXISTS FOR: blended, and keeping its depth
    // write. Under a branch this was not expressible at all; here it is one
    // key over a default, and the ORDER of the two reads is what makes it
    // work - defaults first, then the author.
    const auto* decal = db.findMaterial("sol.decal");
    SOL_REQUIRE(decal != nullptr);
    SOL_CHECK(decal->blend == sol::assets::MaterialBlend::Alpha);
    SOL_CHECK(decal->depthWrite);
    SOL_CHECK(!decal->cullBackFaces); // untouched keys keep the seeded value

    // The schema. An unknown blend names the three that exist rather than
    // falling back to opaque, and a shader stem reaches the filesystem as
    // "<stem>.vert.spv" so an empty one would build an unreadable path.
    SOL_CHECK(
        !merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nblend = \"screen\"\n", "m.toml", &error));
    SOL_CHECK(error.find("additive") != std::string::npos);
    SOL_CHECK(
        !merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nvertex_shader = \"\"\n", "m.toml", &error));
    SOL_CHECK(
        !merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nfragment_shader = \"\"\n", "m.toml", &error));
    SOL_CHECK(!merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\ncull = \"yes\"\n", "m.toml", &error));
}

// ⚑⚑ PHASE 25 STAGE C: WHAT A MATERIAL DECLARES. `textures` is ORDERED because
// a slot's position is its descriptor binding number; `params` is NAMED because
// a param is matched into the shader's uniform block by name. Both facts are
// asserted here because both are invisible in the file and load-bearing in the
// renderer.
SOL_TEST(data_defs_material_declares_slots_and_params)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db,
                      R"(
[[material]]
id = "sol.plain"
texture = "hull"

[[material]]
id = "sol.cabin"
texture = "cockpit"
fragment_shader = "cockpit"
textures = { glow = "cockpit_glow", wear = "hull" }
params = { glow_strength = 2.2, tint = 0.5 }
)",
                      "materials.toml",
                      &error));

    const sol::assets::MaterialDef* plain = db.findMaterial("sol.plain");
    SOL_REQUIRE(plain != nullptr);
    // ⚑ A row that declares nothing must stay EMPTY rather than gaining an
    // implicit slot, because that emptiness is what keeps every material
    // written before this stage on the pipeline layout it already had.
    SOL_CHECK(plain->slots.empty());
    SOL_CHECK(plain->params.empty());

    const sol::assets::MaterialDef* cabin = db.findMaterial("sol.cabin");
    SOL_REQUIRE(cabin != nullptr);
    SOL_REQUIRE(cabin->slots.size() == 2);
    // File order, which is binding order. Reversing these two lines in the file
    // rewires the shader, and that is the whole reason this is a table and not
    // a set.
    SOL_CHECK(cabin->slots[0].name == "glow");
    SOL_CHECK(cabin->slots[0].texture == "cockpit_glow");
    SOL_CHECK(cabin->slots[1].name == "wear");
    SOL_CHECK(cabin->slots[1].texture == "hull");
    // ⚑ The ALBEDO is not one of them. It is set 0, it is what every mesh
    // material has, and a stage that folded it in here would have changed every
    // shader in the engine.
    SOL_CHECK(cabin->texture == "cockpit");

    SOL_REQUIRE(cabin->params.size() == 2);
    SOL_CHECK(cabin->params[0].name == "glow_strength");
    SOL_CHECK(cabin->params[0].value > 2.19f && cabin->params[0].value < 2.21f);
    SOL_CHECK(cabin->params[1].name == "tint");

    // The schema, one refusal per way of getting it wrong.
    SOL_CHECK(
        !merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\ntextures = \"glow\"\n", "m.toml", &error));
    SOL_CHECK(error.find("table") != std::string::npos);
    // An empty stem would reach the filesystem as ".stex", which is a path
    // nobody can read back to a row - so it dies where the slot name is in hand.
    SOL_CHECK(!merge(
        db, "[[material]]\nid = \"m\"\ntexture = \"t\"\ntextures = { glow = \"\" }\n", "m.toml", &error));
    SOL_CHECK(error.find("glow") != std::string::npos);
    SOL_CHECK(!merge(
        db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nparams = { gain = \"loud\" }\n", "m.toml", &error));
    SOL_CHECK(error.find("gain") != std::string::npos);
    // ⚑ An integer IS a number here. TOML tells 2 and 2.0 apart and a shader
    // param does not, so refusing `glow_strength = 2` would be the schema
    // enforcing a distinction the renderer cannot see.
    SOL_CHECK(
        merge(db, "[[material]]\nid = \"m\"\ntexture = \"t\"\nparams = { gain = 2 }\n", "m.toml", &error));
    const sol::assets::MaterialDef* integral = db.findMaterial("m");
    SOL_REQUIRE(integral != nullptr);
    SOL_REQUIRE(integral->params.size() == 1);
    SOL_CHECK(integral->params[0].value > 1.99f && integral->params[0].value < 2.01f);
}

// ⚑ The shipped cockpit material, because stage C's exit criterion is about
// `game/data` and not about a fixture: two textures and a tunable parameter,
// on a row two models wear.
SOL_TEST(data_defs_shipped_cockpit_declares_two_textures_and_a_param)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(db.validateMaterials(&error));

    const sol::assets::MaterialDef* cockpit = db.findMaterial("sol.cockpit");
    SOL_REQUIRE(cockpit != nullptr);
    SOL_CHECK(cockpit->texture == "cockpit"); // set 0, the albedo
    SOL_REQUIRE(cockpit->slots.size() == 1);  // set 1 binding 0
    SOL_CHECK(cockpit->slots[0].name == "glow");
    SOL_CHECK(cockpit->slots[0].texture == "cockpit_glow");
    SOL_REQUIRE(cockpit->params.size() == 1); // set 1 binding 1
    SOL_CHECK(cockpit->params[0].name == "glow_strength");
    SOL_CHECK(cockpit->fragmentShader == "cockpit");
    SOL_CHECK(!cockpit->synthesised);

    // ⚑ TWO MODELS, ONE MATERIAL - the first time that has been true in this
    // game, and the reason the boot line's model and material counts differ.
    std::size_t wearers = 0;
    for (const auto& model : db.models()) {
        if (model.material == "sol.cockpit") {
            ++wearers;
            // A row that names a material carries none of the four surface
            // keys; stage A refuses one that carries both.
            SOL_CHECK(model.texture.empty());
        }
    }
    SOL_CHECK(wearers == 2);
}

// ⚑ The shipped catalog itself, because the claim this stage has to make is
// about `game/data` and not about a fixture: every committed `[[model]]` row
// keeps working untouched, and every one of them now draws through a material.
SOL_TEST(data_defs_shipped_models_all_reach_a_material)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_CHECK(db.validateMaterials(&error));
    SOL_CHECK(!db.models().empty());
    for (const auto& model : db.models()) {
        SOL_REQUIRE(model.materialIndex < db.materials().size());
        const sol::assets::MaterialDef& material = db.materials()[model.materialIndex];
        SOL_CHECK(!material.texture.empty());
        if (!model.material.empty()) {
            // An AUTHORED material: the row names it and carries none of the
            // four surface keys itself.
            SOL_CHECK(!material.synthesised);
            SOL_CHECK(material.id == model.material);
            SOL_CHECK(model.texture.empty());
            continue;
        }
        // A DERIVED one carries exactly what the model row says, which is what
        // makes stage A's frame identical rather than merely similar - and is
        // still true of every row that did not need its own shader.
        SOL_CHECK(material.synthesised);
        SOL_CHECK(material.id == "sol.auto." + model.id);
        SOL_CHECK(material.texture == model.texture);
        SOL_CHECK(material.emissive == model.emissive);
        SOL_CHECK(material.translucent == model.translucent);
        SOL_CHECK(material.alpha == model.alpha);
        // ⚑ Stage B: a row that says nothing about its pipeline gets the
        // stock lambert pair and Phase 12's state, so the derived set produces
        // exactly the two pipelines it always did.
        SOL_CHECK(material.vertexShader == "mesh");
        SOL_CHECK(material.fragmentShader == "mesh");
        SOL_CHECK(material.depthTest);
        SOL_CHECK(material.blend == (model.translucent ? sol::assets::MaterialBlend::Alpha
                                                       : sol::assets::MaterialBlend::Opaque));
        SOL_CHECK(material.depthWrite == !model.translucent);
        SOL_CHECK(material.cullBackFaces == !model.translucent);
    }

    // ⚑⚑ THE MEMBRANE IS THE SHIPPED PROOF OF STAGE B, and the assertion is
    // against `game/data` rather than a fixture on purpose: the row named a
    // material, the material named a fragment shader, and the values that used
    // to sit on the model row came across unchanged.
    const auto* membrane = db.findModel("gate_membrane");
    SOL_REQUIRE(membrane != nullptr);
    SOL_CHECK(membrane->material == "sol.gate_membrane");
    const sol::assets::MaterialDef& film = db.materials()[membrane->materialIndex];
    SOL_CHECK(!film.synthesised);
    SOL_CHECK(film.fragmentShader == "membrane");
    // It brings only a FRAGMENT stage; the vertex work is every other mesh's.
    SOL_CHECK(film.vertexShader == "mesh");
    SOL_CHECK(film.translucent);
    SOL_CHECK(film.blend == sol::assets::MaterialBlend::Alpha);
    SOL_CHECK(!film.depthWrite);
    SOL_CHECK(!film.cullBackFaces);
    SOL_CHECK(std::abs(film.alpha - 0.30f) < 1e-6f);
    SOL_CHECK(std::abs(film.emissive - 0.35f) < 1e-6f);
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
    SOL_REQUIRE(merge(db,
                      R"(
[[role]]
id = "gate"
model = "gate"

[[role]]
id = "rock"
model = "asteroid"
)",
                      "models.toml",
                      &error));

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
    SOL_CHECK(!merge(db, "[[role]]\nid = \"gate\"\nmodel = \"gate\"\nscale = 2.0\n", "m.toml", &error));
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
    SOL_REQUIRE(merge(db, "[[role]]\nid = \"rock\"\nmodel = \"asteroid\"\n", "models.toml", &error));
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
        SOL_REQUIRE(merge(db,
                          R"(
[[role]]
id = "gate"
model = "gate"

[[role]]
id = "rock"
model = "not_a_model"
)",
                          "models.toml",
                          &error));
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
        SOL_REQUIRE(merge(db, "[[role]]\nid = \"gate\"\nmodel = \"gate\"\n", "models.toml", &error));
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
        SOL_REQUIRE(merge(db,
                          R"(
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
                          "models.toml",
                          &error));
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
//
// ⚑⚑ PHASE 31 STAGE E SPLIT THE WEAPON'S ONE KEY INTO TWO, and the shape of
// that split is exactly what this test was already about. `model` is the GUN
// standing in its mount; `bolt_model` is what its shot is drawn as, and it was
// spelled `model` until the gun needed the word. They are two keys for the
// same reason a commodity's rock and chunk are: two drawables, at two places,
// at two scales.
//
// ⚑ The near-miss below used to BE `bolt_model` - the test guessed it as a
// plausible spelling of something that did not exist, and this stage made it
// real. It is `bolt` now, which is the other plausible spelling.
SOL_TEST(data_defs_parse_drawable_overrides)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db,
                      R"(
[[weapon]]
id = "w.plain"
name = "Plain"
kind = "projectile"
mount = "fixed"
size = "small"

[[weapon]]
id = "w.fancy"
name = "Fancy"
kind = "projectile"
mount = "fixed"
size = "small"
model = "big_gun"
bolt_model = "tracer"

[[commodity]]
id = "c.plain"
name = "Plain"

[[commodity]]
id = "c.ice"
name = "Ice"
model = "ice_rock"
chunk_model = "ice_shard"
)",
                      "defs.toml",
                      &error));

    // Absent is EMPTY, not a name - the fallback is a decision the game makes
    // at resolve time, so the parser must not invent one here.
    SOL_CHECK(db.findWeapon("w.plain")->model.empty());
    SOL_CHECK(db.findWeapon("w.plain")->boltModel.empty());
    SOL_CHECK(db.findWeapon("w.fancy")->model == "big_gun");
    SOL_CHECK(db.findWeapon("w.fancy")->boltModel == "tracer");
    // The gun and its shot are separate drawables, exactly as the rock and
    // its chunk are - so a def naming one must not be read as naming both.
    SOL_CHECK(db.findWeapon("w.fancy")->model != db.findWeapon("w.fancy")->boltModel);
    SOL_CHECK(db.findCommodity("c.plain")->model.empty());
    SOL_CHECK(db.findCommodity("c.plain")->chunkModel.empty());
    SOL_CHECK(db.findCommodity("c.ice")->model == "ice_rock");
    SOL_CHECK(db.findCommodity("c.ice")->chunkModel == "ice_shard");

    // ⚑ The rock and the chunk are SEPARATE keys because they are separate
    // drawables at separate scales - a chunk is not a small rock.
    SOL_CHECK(db.findCommodity("c.ice")->model != db.findCommodity("c.ice")->chunkModel);

    // Still a strict schema: a plausible near-miss is an error, not a silent
    // no-op, which is the whole reason these are typed keys and not free text.
    SOL_CHECK(!merge(db, "[[commodity]]\nid = \"c\"\nname = \"C\"\nchunk = \"x\"\n", "d.toml", &error));
    SOL_CHECK(!merge(db,
                     "[[weapon]]\nid = \"w\"\nname = \"W\"\nkind = \"projectile\"\n"
                     "mount = \"fixed\"\nsize = \"small\"\nbolt = \"x\"\n",
                     "d.toml",
                     &error));
    // A component may name its model too since stage E, on the same terms -
    // and the near-miss is refused there as well.
    SOL_REQUIRE(merge(db,
                      "[[component]]\nid = \"k.pod\"\nname = \"Pod\"\n"
                      "mount = \"utility\"\nsize = \"small\"\nmodel = \"pod\"\n",
                      "d.toml",
                      &error));
    SOL_CHECK(db.findComponent("k.pod")->model == "pod");
    SOL_CHECK(!merge(db,
                     "[[component]]\nid = \"k\"\nname = \"K\"\n"
                     "mount = \"utility\"\nsize = \"small\"\nmesh = \"x\"\n",
                     "d.toml",
                     &error));
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

    SOL_CHECK(
        !merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nradius = 0.0\n", "m.toml", &error));
    // An avoidance sphere inside the collision sphere is steering that clears
    // the obstacle it is about to hit, so it is rejected rather than clamped.
    SOL_CHECK(!merge(db,
                     "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nradius = "
                     "10.0\navoid_radius = 5.0\n",
                     "m.toml",
                     &error));
    SOL_CHECK(!merge(
        db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nemissive = -0.5\n", "m.toml", &error));
    // solid takes a bool, not the 0/1 a TOML author might reach for.
    SOL_CHECK(
        !merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nsolid = 0\n", "m.toml", &error));
    // Strict schema: a typo dies at load, not as a mis-sized collision sphere.
    SOL_CHECK(
        !merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\nscale = 2.0\n", "m.toml", &error));

    SOL_CHECK(db.models().empty());

    // A mod layer replaces a model in place, so the index a spawned entity is
    // holding still names the same thing after a reload.
    SOL_CHECK(merge(db, "[[model]]\nid = \"a\"\nmesh = \"m\"\ntexture = \"t\"\n", "base.toml", &error));
    SOL_CHECK(merge(db, "[[model]]\nid = \"b\"\nmesh = \"m\"\ntexture = \"t\"\n", "base.toml", &error));
    SOL_CHECK(merge(
        db, "[[model]]\nid = \"a\"\nmesh = \"m2\"\ntexture = \"t\"\nradius = 4.0\n", "mod.toml", &error));
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
mount = "fixed"
size = "small"
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
    SOL_REQUIRE(merge(db,
                      R"(
[[weapon]]
id = "sol.pulse_cannon"
name = "Pulse Cannon"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 12.0
)",
                      "guns.toml",
                      &error));
    SOL_CHECK(db.findWeapon("sol.pulse_cannon")->miningPower == 0.0f);

    // Half a refinery is a schema error: the service needs both ends.
    SOL_CHECK(!merge(db,
                     R"(
[[station]]
id = "sol.half_refinery"
name = "Half Refinery"
refine_input = "sol.ore"
)",
                     "half.toml",
                     &error));
    SOL_CHECK(error.find("refine_input") != std::string::npos);
    SOL_CHECK(db.findStation("sol.half_refinery") == nullptr);

    // Negative ore weights are a schema error, not a silent zero.
    SOL_CHECK(!merge(db,
                     R"(
[[commodity]]
id = "sol.bad_ore"
name = "Bad Ore"
ore_weight_core = -1.0
)",
                     "bad_ore.toml",
                     &error));
}

// ---------------------------------------------------------------------------
// `[[system]]` - a place somebody put somewhere (Phase 29 stage A).
// ---------------------------------------------------------------------------

namespace {

// The smallest set an authored system can legally point at: a claimant faction
// and one station archetype.
constexpr const char* kSystemFixtureDeps = R"(
[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[faction]]
id = "sol.reavers"
name = "Reavers"
color = [0.8, 0.2, 0.2]
kind = "pirate"

[[station]]
id = "sol.station_refinery"
name = "Refinery"
)";

} // namespace

// ⚑⚑ THE WHOLE OF DECISION 2, ASSERTED RATHER THAN COMMENTED: a field an author
// WROTE and a field that happens to equal the default are different states, and
// two of them cannot be told apart any other way. `faction` unset and `lawless`
// both leave `factionIndex` at kNoFaction; `primary_planet = 0` is
// indistinguishable from an unwritten one. The `has…` flags are what carry the
// difference into the generator, so they are what this checks.
SOL_TEST(data_defs_system_records_which_fields_the_author_wrote)
{
    DefDatabase db;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[system]]
id = "campaign.harrow"
name = "Harrow"
region = "fringe"
faction = "sol.navy"
primary_planet = 0
secret = true

[[system.planet]]
name = "Harrow Prime"
radius = 3200000.0

[[system.planet]]
name = "Harrow Deep"

[[system.station]]
name = "The Long Watch"
station = "sol.station_refinery"

[[system]]
id = "campaign.bare"
)",
                      "systems.toml"));

    SOL_REQUIRE(db.systems().size() == 2);
    const sol::assets::SystemDef& harrow = db.systems()[0];
    SOL_CHECK(harrow.id == "campaign.harrow");
    SOL_CHECK(harrow.placement == "random"); // the default, and the only rule in stage A
    SOL_CHECK(harrow.hasName && harrow.name == "Harrow");
    SOL_CHECK(harrow.hasRegion && harrow.region == "fringe");
    SOL_CHECK(harrow.hasFaction && harrow.factionId == "sol.navy");
    SOL_CHECK(!harrow.lawless);
    SOL_CHECK(harrow.hasPrimaryPlanet && harrow.primaryPlanet == 0);
    SOL_CHECK(harrow.secret);
    SOL_REQUIRE(harrow.planets.size() == 2);
    SOL_CHECK(harrow.planets[0].name == "Harrow Prime");
    SOL_CHECK(harrow.planets[0].hasRadius && harrow.planets[0].radius == 3'200'000.0);
    SOL_CHECK(harrow.planets[1].name == "Harrow Deep");
    SOL_CHECK(!harrow.planets[1].hasRadius); // the generator will roll one
    SOL_REQUIRE(harrow.stations.size() == 1);
    SOL_CHECK(harrow.stations[0].name == "The Long Watch");
    SOL_CHECK(harrow.stations[0].stationId == "sol.station_refinery");

    // ⚑ The one that a sentinel gets wrong. Every value below is what the
    // default already was; only the flags say nobody wrote them.
    const sol::assets::SystemDef& bare = db.systems()[1];
    SOL_CHECK(!bare.hasName && !bare.hasRegion && !bare.hasFaction && !bare.hasPrimaryPlanet);
    SOL_CHECK(!bare.lawless && !bare.secret);
    // ⚑ And `security` is the newest member of exactly that club (Phase 30
    // stage E): 0.0 is a legal authored value MEANING "nobody comes", so the
    // flag is the only thing separating it from a row that said nothing at all
    // - and the generator's two behaviours are wholly different.
    SOL_CHECK(!bare.hasSecurity && bare.security == 0.0f);
    SOL_CHECK(bare.planets.empty() && bare.stations.empty());
    SOL_CHECK(db.findSystem("campaign.bare") == &bare);
    SOL_CHECK(db.findSystem("campaign.nowhere") == nullptr);
}

// "Nobody owns this" is a thing an author says, not a thing they leave out, and
// saying it twice in two different ways is a mistake worth naming.
SOL_TEST(data_defs_system_lawless_is_an_authored_state_not_an_absence)
{
    DefDatabase db;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[system]]
id = "campaign.haven"
lawless = true
)",
                      "systems.toml"));
    SOL_REQUIRE(db.systems().size() == 1);
    SOL_CHECK(db.systems()[0].lawless);
    SOL_CHECK(!db.systems()[0].hasFaction);

    DefDatabase both;
    std::string error;
    SOL_REQUIRE(merge(both, kSystemFixtureDeps, "deps.toml"));
    SOL_CHECK(!merge(both,
                     R"(
[[system]]
id = "campaign.confused"
faction = "sol.navy"
lawless = true
)",
                     "systems.toml",
                     &error));
    SOL_CHECK(error.find("say different things") != std::string::npos);
}

// ⚑ Both ends of the band an author may write, and the one in the middle that
// is easy to read as "unset". Zero is a legal, meaningful rating - it is the
// value `sol.lantern` has had since before the key existed - so it has to
// arrive with its flag up.
SOL_TEST(data_defs_system_security_is_a_magnitude_and_zero_is_a_value)
{
    DefDatabase db;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[system]]
id = "campaign.fortress"
faction = "sol.navy"
region = "fringe"
security = 0.9

[[system]]
id = "campaign.forgotten"
faction = "sol.navy"
security = 0.0

[[system]]
id = "campaign.whole"
faction = "sol.navy"
security = 1
)",
                      "systems.toml"));
    SOL_REQUIRE(db.systems().size() == 3);
    SOL_CHECK(db.systems()[0].hasSecurity && db.systems()[0].security == 0.9f);
    SOL_CHECK(db.systems()[1].hasSecurity && db.systems()[1].security == 0.0f);
    // Written as a TOML integer, which is the spelling an author reaches for at
    // the ends of a range - the reader takes either.
    SOL_CHECK(db.systems()[2].hasSecurity && db.systems()[2].security == 1.0f);
}

// ⚑⚑ REFUSED, AND BY NAME - decision 3 and the `validateRoles` precedent. Every
// message below has to name the file and the id, because the author reading it
// is looking at a directory of TOML rather than at a debugger.
SOL_TEST(data_defs_system_errors_name_the_file_and_the_id)
{
    const auto refused = [](const char* toml, const char* needle) {
        DefDatabase db;
        std::string error;
        SOL_CHECK(merge(db, kSystemFixtureDeps, "deps.toml"));
        const bool ok = merge(db, toml, "systems.toml", &error);
        if (ok) {
            std::printf("  expected a refusal, got a clean parse\n");
        }
        SOL_CHECK(!ok);
        if (error.find(needle) == std::string::npos) {
            std::printf("  message was: %s (wanted '%s')\n", error.c_str(), needle);
        }
        SOL_CHECK(error.find(needle) != std::string::npos);
        SOL_CHECK(error.find("systems.toml") != std::string::npos);
    };

    // ⚑ A RULE NAMED WITHOUT ITS PARAMETERS IS THE MISTAKE THE "placement
    // names it, a sibling carries them" shape invites, so it is refused in
    // both directions. Silently placing this at random instead would read to
    // an author as a parser that ate their ring.
    refused(R"(
[[system]]
id = "campaign.x"
placement = "jumps_from"
)",
            "no 'jumps_from' table");
    refused(R"(
[[system]]
id = "campaign.x"
jumps_from = { system = "campaign.anchor", min = 1, max = 2 }
)",
            "placement = \"jumps_from\"");
    refused(R"(
[[system]]
id = "campaign.x"
placement = "at_system"
)",
            "no 'at_system' key");
    refused(R"(
[[system]]
id = "campaign.x"
at_system = "sol.navy"
)",
            "placement = \"at_system\"");
    // A rule nobody implements at all still names itself in the refusal.
    refused(R"(
[[system]]
id = "campaign.x"
placement = "somewhere_nice"
)",
            "somewhere_nice");
    refused(R"(
[[system]]
id = "campaign.x"
region = "outskirts"
)",
            "outskirts");
    // The generator invariant six unguarded call sites depend on, stated where
    // an author can be told about it rather than crashed by it.
    refused(R"(
[[system]]
id = "campaign.x"
primary_planet = 3

[[system.planet]]
name = "Only One"
)",
            "primary_planet");
    refused(R"(
[[system]]
id = "campaign.x"
primary_planet = 0
)",
            "primary_planet");
    refused(R"(
[[system]]
id = "campaign.x"
name = ""
)",
            "must not be empty");
    // ⚑⚑⚑ THE MISTAKE THE SIGNED READOUT INVITES (Phase 30 stage E). An author
    // who has read "-0.75" off `sol.security` for a clan home will write -0.75
    // here, and it has to be refused rather than honoured: the sign says WHO
    // polices a place, and a negative on a system the Navy holds would put
    // "Held by Solar Navy" on the map and leave the sky over it empty, because
    // `patrolsFor` is zero below zero and `raidersFor` is only reached down the
    // pirate branch. The message carries the reason, not just the range.
    refused(R"(
[[system]]
id = "campaign.x"
security = -0.6
)",
            "the SIGN is not yours");
    refused(R"(
[[system]]
id = "campaign.x"
security = 1.4
)",
            "from 0 to 1");
    // The same shape as `faction` + `lawless` directly above it, and the same
    // reason: two adjacent lines saying different things.
    refused(R"(
[[system]]
id = "campaign.x"
lawless = true
security = 0.5
)",
            "say different things");
    refused(R"(
[[system]]
id = "campaign.x"
speed = 12
)",
            "unknown key");
    // A nested row is held to the same strict schema as the row that holds it.
    refused(R"(
[[system]]
id = "campaign.x"

[[system.station]]
name = "Nameless Dock"
)",
            "missing key 'station'");
    refused(R"(
[[system]]
id = "campaign.x"

[[system.planet]]
name = "Hollow"
radius = -1.0
)",
            "radius");
}

// Cross-def checks, which cannot run at parse time because a faction may
// legitimately live in an earlier or a later layer than the system naming it -
// the same reason `validateMaterials` is a separate pass.
SOL_TEST(data_defs_system_validation_refuses_names_that_resolve_to_nothing)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[system]]
id = "campaign.good"
faction = "sol.navy"

[[system.station]]
name = "Watchpost"
station = "sol.station_refinery"
)",
                      "systems.toml"));
    SOL_CHECK(db.validateSystems(&error));

    DefDatabase missingFaction;
    SOL_REQUIRE(merge(missingFaction, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(missingFaction,
                      R"(
[[system]]
id = "campaign.orphan"
faction = "sol.consortium"
)",
                      "systems.toml"));
    SOL_CHECK(!missingFaction.validateSystems(&error));
    SOL_CHECK(error.find("sol.consortium") != std::string::npos);
    SOL_CHECK(error.find("campaign.orphan") != std::string::npos);

    DefDatabase missingStation;
    SOL_REQUIRE(merge(missingStation, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(missingStation,
                      R"(
[[system]]
id = "campaign.pier"

[[system.station]]
name = "Pier Nine"
station = "sol.station_shipyard"
)",
                      "systems.toml"));
    SOL_CHECK(!missingStation.validateSystems(&error));
    SOL_CHECK(error.find("sol.station_shipyard") != std::string::npos);
    SOL_CHECK(error.find("Pier Nine") != std::string::npos);

    // Two authored systems wearing one name is not resolvable in a way either
    // author would recognise as theirs.
    DefDatabase twins;
    SOL_REQUIRE(merge(twins, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(twins,
                      R"(
[[system]]
id = "campaign.a"
name = "Harrow"

[[system]]
id = "campaign.b"
name = "Harrow"
)",
                      "systems.toml"));
    SOL_CHECK(!twins.validateSystems(&error));
    SOL_CHECK(error.find("collides") != std::string::npos);
}

// A later layer replaces an id wholesale, which is what makes a mod able to
// re-place a base-game system rather than only add beside it.
SOL_TEST(data_defs_system_overridden_by_a_later_layer)
{
    DefDatabase db;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[system]]
id = "campaign.harrow"
name = "Harrow"
secret = true

[[system.planet]]
name = "Harrow Prime"
)",
                      "base/systems.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[system]]
id = "campaign.harrow"
name = "Harrow Reborn"
)",
                      "mod/systems.toml"));

    SOL_REQUIRE(db.systems().size() == 1);
    SOL_CHECK(db.systems()[0].name == "Harrow Reborn");
    // Wholesale, not merged: the mod said nothing about planets or secrecy, so
    // the mod's silence is what the database now holds.
    SOL_CHECK(db.systems()[0].planets.empty());
    SOL_CHECK(!db.systems()[0].secret);
    SOL_CHECK(db.systems()[0].source == "mod/systems.toml");
}

// ---------------------------------------------------------------------------
// Phase 29 stage B: the other three placement rules, as an author writes them.
// ---------------------------------------------------------------------------

// THE RULE IS ALWAYS NAMED IN `placement`, AND A RULE WITH PARAMETERS PUTS
// THEM IN A SIBLING KEY OF THE SAME NAME. decisions/018 wrote the four rules
// two different ways - two as bare words and two as keys carrying a value -
// which left a reader scanning every key in a row to find out how the system
// was placed.
SOL_TEST(data_defs_system_parses_every_placement_rule)
{
    DefDatabase db;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    std::string error;
    const bool ok = merge(db,
                          R"(
[[system]]
id = "campaign.anchor"
placement = "anywhere"

[[system]]
id = "campaign.home"
placement = "at_system"
at_system = "sol.navy"

[[system]]
id = "campaign.ring"
placement = "jumps_from"
jumps_from = { system = "campaign.anchor", min = 2, max = 4 }

[[system]]
id = "campaign.plain"
)",
                          "systems.toml",
                          &error);
    if (!ok) {
        std::printf("  %s\n", error.c_str());
    }
    SOL_REQUIRE(ok);
    SOL_REQUIRE(db.systems().size() == 4);

    SOL_CHECK(db.systems()[0].placement == "anywhere");
    SOL_CHECK(db.systems()[1].placement == "at_system");
    SOL_CHECK(db.systems()[1].atSystemFactionId == "sol.navy");
    SOL_CHECK(db.systems()[2].placement == "jumps_from");
    SOL_CHECK(db.systems()[2].jumpsFromSystemId == "campaign.anchor");
    SOL_CHECK(db.systems()[2].jumpsFromMin == 2);
    SOL_CHECK(db.systems()[2].jumpsFromMax == 4);
    // Unwritten is still "random", which is what keeps every stage A file
    // parsing unchanged.
    SOL_CHECK(db.systems()[3].placement == "random");

    SOL_CHECK(db.validateSystems(&error));
}

// The ring's own arithmetic, refused where an author can read it rather than
// at generation time where it would look like an unsatisfiable galaxy.
SOL_TEST(data_defs_system_ring_bounds_are_checked_in_the_file)
{
    const auto refused = [](const char* toml, const char* needle) {
        DefDatabase db;
        std::string error;
        SOL_CHECK(merge(db, kSystemFixtureDeps, "deps.toml"));
        const bool ok = merge(db, toml, "systems.toml", &error);
        SOL_CHECK(!ok);
        if (error.find(needle) == std::string::npos) {
            std::printf("  message was: %s (wanted '%s')\n", error.c_str(), needle);
        }
        SOL_CHECK(error.find(needle) != std::string::npos);
        SOL_CHECK(error.find("systems.toml") != std::string::npos);
    };

    refused(R"(
[[system]]
id = "campaign.a"

[[system]]
id = "campaign.b"
placement = "jumps_from"
jumps_from = { system = "campaign.a", min = 5, max = 2 }
)",
            "'min' must not be greater than 'max'");
    // ZERO JUMPS FROM THE ANCHOR IS THE ANCHOR, and the anchor is already
    // taken by the system that placed it - so the ring would be empty for a
    // reason the author cannot see anywhere in their own file.
    refused(R"(
[[system]]
id = "campaign.a"

[[system]]
id = "campaign.b"
placement = "jumps_from"
jumps_from = { system = "campaign.a", min = 0, max = 0 }
)",
            "at least 1");
    refused(R"(
[[system]]
id = "campaign.a"

[[system]]
id = "campaign.b"
placement = "jumps_from"
jumps_from = { system = "campaign.a", min = 1 }
)",
            "'max' is required");
    refused(R"(
[[system]]
id = "campaign.a"

[[system]]
id = "campaign.b"
placement = "jumps_from"
jumps_from = { system = "campaign.a", min = 1, max = 2, radius = 3 }
)",
            "radius");
}

// AN ANCHOR MUST BE DECLARED EARLIER, AND THAT IS A FACT ABOUT THE FILES
// RATHER THAN ABOUT THE GALAXY - so it is settled here, in the only layer that
// can see every layer at once. The generator's own "not placed before this
// one" refusal still stands behind it and catches what this cannot: an anchor
// that parsed fine and then failed its own placement rule.
SOL_TEST(data_defs_system_ring_anchor_must_come_first)
{
    const auto refused = [](const char* toml, const char* needle) {
        DefDatabase db;
        std::string error;
        SOL_CHECK(merge(db, kSystemFixtureDeps, "deps.toml"));
        SOL_CHECK(merge(db, toml, "systems.toml", &error));
        const bool ok = db.validateSystems(&error);
        if (ok) {
            std::printf("  expected a refusal, got a clean validation\n");
        }
        SOL_CHECK(!ok);
        if (error.find(needle) == std::string::npos) {
            std::printf("  message was: %s (wanted '%s')\n", error.c_str(), needle);
        }
        SOL_CHECK(error.find(needle) != std::string::npos);
        SOL_CHECK(error.find("systems.toml") != std::string::npos);
    };

    refused(R"(
[[system]]
id = "campaign.ring"
placement = "jumps_from"
jumps_from = { system = "campaign.anchor", min = 1, max = 2 }

[[system]]
id = "campaign.anchor"
)",
            "declared after it");
    refused(R"(
[[system]]
id = "campaign.ring"
placement = "jumps_from"
jumps_from = { system = "campaign.nobody", min = 1, max = 2 }
)",
            "not a [[system]]");
    refused(R"(
[[system]]
id = "campaign.ring"
placement = "jumps_from"
jumps_from = { system = "campaign.ring", min = 1, max = 2 }
)",
            "this system itself");
}

// `at_system` NAMES A MAJOR, because a clan template claims nothing and is
// never handed a capital by `claimTerritory`. Refused rather than warned: a
// system meant to be somebody's home, placed at random instead, is a different
// place from the one the campaign was written against.
SOL_TEST(data_defs_system_at_system_names_a_faction_that_holds_a_capital)
{
    const auto refused = [](const char* toml, const char* needle) {
        DefDatabase db;
        std::string error;
        SOL_CHECK(merge(db, kSystemFixtureDeps, "deps.toml"));
        SOL_CHECK(merge(db, toml, "systems.toml", &error));
        const bool ok = db.validateSystems(&error);
        SOL_CHECK(!ok);
        if (error.find(needle) == std::string::npos) {
            std::printf("  message was: %s (wanted '%s')\n", error.c_str(), needle);
        }
        SOL_CHECK(error.find(needle) != std::string::npos);
        SOL_CHECK(error.find("systems.toml") != std::string::npos);
    };

    refused(R"(
[[system]]
id = "campaign.home"
placement = "at_system"
at_system = "sol.nobody"
)",
            "is not a [[faction]]");
    // A pirate def is a clan TEMPLATE, instantiated per lawless neighbourhood.
    // It has no capital to take and never will.
    refused(R"(
[[system]]
id = "campaign.home"
placement = "at_system"
at_system = "sol.reavers"
)",
            "clan template");
}

// ---------------------------------------------------------------------------
// Phase 29 stage C: `[[constellation]]`, a group placed as a unit.
// ---------------------------------------------------------------------------

// The whole shape, read back field by field - and the point of reading it back
// is that a member is an ordinary `[[system]]` row wearing a different header.
// Everything stage A taught the parser about a system holds inside a group,
// including the nested planet and station rows, which here sit THREE headers
// deep for the first time in this project.
SOL_TEST(data_defs_constellation_parses_members_and_lanes)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[constellation]]
id = "campaign.deadfall"

[[constellation.system]]
id = "campaign.deadfall_gate"
name = "Deadfall Gate"
region = "fringe"
lawless = true

[[constellation.system]]
id = "campaign.deadfall"
name = "Deadfall"
faction = "sol.navy"
secret = true
primary_planet = 1
security = 0.75

[[constellation.system.planet]]
name = "Deadfall I"

[[constellation.system.planet]]
name = "Deadfall Prime"
radius = 6400000.0

[[constellation.system.station]]
name = "The Long Watch"
station = "sol.station_refinery"

[[constellation.system]]
id = "campaign.deadfall_deep"
name = "Deadfall Deep"

[[constellation.link]]
from = "campaign.deadfall_gate"
to = "campaign.deadfall"

[[constellation.link]]
from = "campaign.deadfall"
to = "campaign.deadfall_deep"
)",
                      "systems.toml",
                      &error));
    SOL_REQUIRE(db.constellations().size() == 1);
    const sol::assets::ConstellationDef& group = db.constellations()[0];
    SOL_CHECK(group.id == "campaign.deadfall");
    SOL_CHECK(group.placement == "anywhere"); // the default, and the only value
    SOL_CHECK(group.source == "systems.toml");
    SOL_REQUIRE(group.members.size() == 3);

    SOL_CHECK(group.members[0].id == "campaign.deadfall_gate");
    SOL_CHECK(group.members[0].lawless);
    SOL_CHECK(group.members[0].region == "fringe");
    SOL_CHECK(group.members[0].hasRegion);
    SOL_CHECK(group.members[0].planets.empty());

    // ⚑ The nested rows landed in the member they were written under, which is
    // the three-deep descent `toml_nestedArrayOfTablesGoesThreeDeep` proves at
    // the parser and this asserts at the def layer.
    SOL_CHECK(group.members[1].id == "campaign.deadfall");
    SOL_CHECK(group.members[1].factionId == "sol.navy");
    SOL_CHECK(group.members[1].hasFaction);
    SOL_CHECK(group.members[1].secret);
    // ⚑ Including the newest key, and this is the assertion that matters for
    // it: `readSystemDef` is shared, so a member gets `security` for free - and
    // "for free" is a claim worth holding, because the shipped example mod
    // writes one on a constellation member rather than on a `[[system]]`.
    SOL_CHECK(group.members[1].hasSecurity && group.members[1].security == 0.75f);
    SOL_CHECK(!group.members[0].hasSecurity);
    SOL_CHECK(group.members[1].hasPrimaryPlanet);
    SOL_CHECK(group.members[1].primaryPlanet == 1);
    SOL_REQUIRE(group.members[1].planets.size() == 2);
    SOL_CHECK(group.members[1].planets[1].name == "Deadfall Prime");
    SOL_CHECK(group.members[1].planets[1].radius == 6'400'000.0);
    SOL_REQUIRE(group.members[1].stations.size() == 1);
    SOL_CHECK(group.members[1].stations[0].stationId == "sol.station_refinery");

    SOL_CHECK(group.members[2].id == "campaign.deadfall_deep");
    SOL_CHECK(group.members[2].planets.empty());

    SOL_REQUIRE(group.links.size() == 2);
    SOL_CHECK(group.links[0].fromId == "campaign.deadfall_gate");
    SOL_CHECK(group.links[0].toId == "campaign.deadfall");
    SOL_CHECK(group.links[1].toId == "campaign.deadfall_deep");

    // A group's members are NOT `[[system]]` rows: they are reachable through
    // the group and nowhere else, which is what stops a member being placed
    // twice.
    SOL_CHECK(db.systems().empty());

    // The cross-def checks see them anyway - that is the point of stage C's
    // change to `validateSystems`.
    SOL_CHECK(db.validateSystems(&error));
}

// Every way a constellation is refused, in one table. Each of these is a file
// somebody could plausibly write, and each of them would otherwise place
// silently and read to its author as a broken parser.
SOL_TEST(data_defs_constellation_refusals)
{
    const auto refused = [](const char* toml, const char* needle, bool atParse) {
        DefDatabase db;
        std::string error;
        SOL_CHECK(merge(db, kSystemFixtureDeps, "deps.toml"));
        const bool merged = merge(db, toml, "systems.toml", &error);
        if (atParse) {
            SOL_CHECK(!merged);
        } else {
            SOL_CHECK(merged);
            const bool ok = db.validateSystems(&error);
            if (ok) {
                std::printf("  expected a refusal, got a clean validation\n");
            }
            SOL_CHECK(!ok);
        }
        if (error.find(needle) == std::string::npos) {
            std::printf("  message was: %s (wanted '%s')\n", error.c_str(), needle);
        }
        SOL_CHECK(error.find(needle) != std::string::npos);
        SOL_CHECK(error.find("systems.toml") != std::string::npos);
    };

    // ⚑⚑ THE OTHER THREE RULES ARE REFUSED WITH THE REASON, NOT WITH SILENCE.
    // They REPLACE a system the generator already made, and a group cannot
    // replace one node as a unit.
    refused(R"(
[[constellation]]
id = "campaign.group"
placement = "random"

[[constellation.system]]
id = "campaign.a"

[[constellation.system]]
id = "campaign.b"
)",
            "cannot replace one node as a unit",
            true);

    // A group of one is a system; a group of none is nothing. Both are almost
    // certainly a file that lost a row.
    refused(R"(
[[constellation]]
id = "campaign.lonely"

[[constellation.system]]
id = "campaign.only"
)",
            "one place on its own is a [[system]]",
            true);
    refused(R"(
[[constellation]]
id = "campaign.empty"
)",
            "this one declares none",
            true);

    // ⚑ A MEMBER MAY NOT CARRY A PLACEMENT RULE, and the refusal answers the
    // question rather than calling the key unknown. The group carries one.
    for (const char* key : {"placement = \"anywhere\"",
                            "at_system = \"sol.navy\"",
                            "jumps_from = { system = \"campaign.a\", min = 1, max = 2 }"}) {
        const std::string toml = std::string(R"(
[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.a"

[[constellation.system]]
id = "campaign.b"
)") + key + "\n";
        refused(toml.c_str(), "the whole group takes one placement rule", true);
    }

    // A lane names two of THIS group's members. A lane out of the constellation
    // is a gate, and which gates a system gets is the generator's to decide.
    refused(R"(
[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.a"

[[constellation.system]]
id = "campaign.b"

[[constellation.link]]
from = "campaign.a"
to = "campaign.elsewhere"
)",
            "not a [[constellation.system]] of this constellation",
            true);
    refused(R"(
[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.a"

[[constellation.system]]
id = "campaign.b"

[[constellation.link]]
from = "campaign.a"
to = "campaign.a"
)",
            "a lane needs two ends",
            true);
    refused(R"(
[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.a"

[[constellation.system]]
id = "campaign.b"

[[constellation.link]]
from = "campaign.a"
to = "campaign.b"

[[constellation.link]]
from = "campaign.b"
to = "campaign.a"
)",
            "one lane is one lane",
            true);

    // ⚑⚑ TWO SYSTEMS CANNOT SHARE AN ID, AND ONLY THE CROSS-DEF PASS CAN SAY
    // SO. `mergeDef` keeps ids unique within a list by having a later layer
    // replace an earlier one - but members are merged WITH their group rather
    // than one at a time, so a collision inside a group, or between a group and
    // a `[[system]]`, survives the parse and would leave `sol.system_by_id`
    // answering with whichever the generator reached first.
    refused(R"(
[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.twin"

[[constellation.system]]
id = "campaign.twin"
)",
            "already used by another authored system",
            false);
    refused(R"(
[[system]]
id = "campaign.twin"

[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.twin"

[[constellation.system]]
id = "campaign.other"
)",
            "already used by another authored system",
            false);

    // A member is a system, so every cross-def check a `[[system]]` gets, it
    // gets - which is the half of the galaxy stage C would otherwise have left
    // unvalidated.
    refused(R"(
[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.a"
faction = "sol.consortium"

[[constellation.system]]
id = "campaign.b"
)",
            "which is not a [[faction]]",
            false);
    refused(R"(
[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.a"

[[constellation.system]]
id = "campaign.b"

[[constellation.system.station]]
name = "Pier Nine"
station = "sol.station_shipyard"
)",
            "which is not a [[station]]",
            false);
    // A member's name colliding with a standalone system's is the same refusal
    // two `[[system]]` rows already get.
    refused(R"(
[[system]]
id = "campaign.solo"
name = "Harrow"

[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.a"
name = "Harrow"

[[constellation.system]]
id = "campaign.b"
)",
            "collides",
            false);
}

// ⚑⚑ A RING MAY ANCHOR ON A CONSTELLATION MEMBER, AND DEF ORDER DOES NOT
// CONSTRAIN IT - which is a real difference from anchoring on a `[[system]]`,
// where the anchor must be declared first. A constellation cannot fail to be
// placed, so every member has an index before any rule runs; refusing a
// backwards reference would be a rule an author could not act on.
SOL_TEST(data_defs_ring_may_anchor_on_a_constellation_member_declared_later)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[system]]
id = "campaign.picket"
placement = "jumps_from"
jumps_from = { system = "campaign.hub", min = 1, max = 3 }

[[constellation]]
id = "campaign.group"

[[constellation.system]]
id = "campaign.hub"

[[constellation.system]]
id = "campaign.spur"
)",
                      "systems.toml",
                      &error));
    if (!db.validateSystems(&error)) {
        std::printf("  %s\n", error.c_str());
    }
    SOL_CHECK(db.validateSystems(&error));
}

// A later layer replaces a constellation wholesale by id, the way it already
// replaces a ship or a system - which is what lets a mod re-draw a base game's
// group rather than only add one beside it.
SOL_TEST(data_defs_constellation_overridden_by_a_later_layer)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kSystemFixtureDeps, "deps.toml"));
    SOL_REQUIRE(merge(db,
                      R"(
[[constellation]]
id = "campaign.deadfall"

[[constellation.system]]
id = "campaign.a"
name = "Base A"

[[constellation.system]]
id = "campaign.b"
)",
                      "base/systems.toml",
                      &error));
    SOL_REQUIRE(merge(db,
                      R"(
[[constellation]]
id = "campaign.deadfall"

[[constellation.system]]
id = "campaign.a"
name = "Modded A"

[[constellation.system]]
id = "campaign.b"

[[constellation.system]]
id = "campaign.c"
)",
                      "mods/x/systems.toml",
                      &error));
    SOL_REQUIRE(db.constellations().size() == 1);
    SOL_CHECK(db.constellations()[0].source == "mods/x/systems.toml");
    SOL_REQUIRE(db.constellations()[0].members.size() == 3);
    SOL_CHECK(db.constellations()[0].members[0].name == "Modded A");
}

// --- Mounts (Phase 31 stage A2) ---

SOL_TEST(data_defs_ship_parses_mounts)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db,
                      R"(
[[ship]]
id = "sol.destroyer"
name = "Destroyer"

  [[ship.mount]]
  id = "turret_dorsal_1"
  kind = "turret"
  size = "medium"
  at = [0.0, 3.2, -1.5]
  aim = [0.0, 1.0, 0.0]
  arc = 220.0

  [[ship.mount]]
  id = "internal_reactor"
  kind = "subsystem"
  size = "large"
)",
                      "ships.toml",
                      &error));
    const ShipDef* def = db.findShip("sol.destroyer");
    SOL_REQUIRE(def != nullptr);
    SOL_REQUIRE(def->mounts.size() == 2);

    // Authored order, because the outfitting screen and the Forge both show a
    // list and an author who groups their turrets means it.
    SOL_CHECK(def->mounts[0].id == "turret_dorsal_1");
    SOL_CHECK(def->mounts[1].id == "internal_reactor");

    const sol::assets::ShipMount* turret = def->findMount("turret_dorsal_1");
    SOL_REQUIRE(turret != nullptr);
    SOL_CHECK(turret->kind == sol::assets::MountKind::Turret);
    SOL_CHECK(turret->size == sol::assets::MountSize::Medium);
    SOL_CHECK(turret->arc == 220.0f);
    SOL_CHECK(turret->at[1] == 3.2f);
    SOL_CHECK(turret->aim[1] == 1.0f);
    // decisions/014 rule 2: `at` present is the whole of "external".
    SOL_CHECK(turret->external);

    const sol::assets::ShipMount* reactor = def->findMount("internal_reactor");
    SOL_REQUIRE(reactor != nullptr);
    SOL_CHECK(reactor->kind == sol::assets::MountKind::Subsystem);
    SOL_CHECK(reactor->size == sol::assets::MountSize::Large);
    SOL_CHECK(!reactor->external);
    // A default facing rather than a zeroed one: an internal mount points where
    // the ship points. Nothing reads it, and a reader that starts to would
    // otherwise get an aim of exactly nowhere.
    SOL_CHECK(reactor->aim[2] == -1.0f);
    SOL_CHECK(reactor->arc == 0.0f);

    SOL_CHECK(def->findMount("no_such_mount") == nullptr);
}

// ⚑ AN EMPTY MOUNT LIST IS LEGAL AND NOW MEANS SOMETHING. Before stage B it
// meant "the slot counts are still in charge"; after it, a hull with no mounts
// fits nothing and flies unarmed. It still LOADS rather than refusing, which
// is what a mod's ship def written before mounts existed needs - the def
// layer's standing rule is that a missing key is a default, not an error.
SOL_TEST(data_defs_ship_without_mounts_still_loads_and_fits_nothing)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kBaseShips, "ships.toml", &error));
    const ShipDef* def = db.findShip("sol.shuttle");
    SOL_REQUIRE(def != nullptr);
    SOL_CHECK(def->mounts.empty());
    SOL_CHECK(def->findMount("anything") == nullptr);
}

SOL_TEST(data_defs_mount_kind_and_size_names_round_trip)
{
    // The spelling table is written once and read in both directions; this is
    // what says so when somebody adds a kind to the enum and not to the table.
    for (std::size_t i = 0; i < sol::assets::kMountKindCount; ++i) {
        const auto kind = static_cast<sol::assets::MountKind>(i);
        sol::assets::MountKind parsed{};
        SOL_REQUIRE(sol::assets::parseMountKind(sol::assets::mountKindName(kind), parsed));
        SOL_CHECK(parsed == kind);
    }
    for (std::size_t i = 0; i < sol::assets::kMountSizeCount; ++i) {
        const auto size = static_cast<sol::assets::MountSize>(i);
        sol::assets::MountSize parsed{};
        SOL_REQUIRE(sol::assets::parseMountSize(sol::assets::mountSizeName(size), parsed));
        SOL_CHECK(parsed == size);
    }
    // ⚑ A ROUND TRIP ALONE CANNOT SEE A ROTATED TABLE - both directions read
    // it, so shifting every entry by one agrees with itself perfectly. These
    // pin the two ends to the words gdd.md 11.5 actually uses.
    SOL_CHECK(std::strcmp(sol::assets::mountKindName(sol::assets::MountKind::Turret), "turret") == 0);
    SOL_CHECK(std::strcmp(sol::assets::mountKindName(sol::assets::MountKind::Dock), "dock") == 0);
    SOL_CHECK(std::strcmp(sol::assets::mountSizeName(sol::assets::MountSize::Small), "small") == 0);
    SOL_CHECK(std::strcmp(sol::assets::mountSizeName(sol::assets::MountSize::XLarge), "xlarge") == 0);

    sol::assets::MountKind unused{};
    SOL_CHECK(!sol::assets::parseMountKind("Turret", unused)); // spellings are lowercase
    SOL_CHECK(!sol::assets::parseMountKind("cargo", unused));  // gdd.md 11.5 has no cargo kind
    SOL_CHECK(!sol::assets::parseMountKind("", unused));
}

SOL_TEST(data_defs_mount_accepts_its_own_size_or_smaller)
{
    using sol::assets::mountAccepts;
    using sol::assets::MountSize;
    // decisions/014 rule 3, in both directions: a large mount takes small kit
    // and wastes itself doing it, and a small mount refuses large kit.
    SOL_CHECK(mountAccepts(MountSize::Large, MountSize::Small));
    SOL_CHECK(mountAccepts(MountSize::Large, MountSize::Large));
    SOL_CHECK(!mountAccepts(MountSize::Small, MountSize::Medium));
    SOL_CHECK(!mountAccepts(MountSize::Large, MountSize::XLarge));
    SOL_CHECK(mountAccepts(MountSize::XLarge, MountSize::Large));
}

SOL_TEST(data_defs_mount_errors_name_the_hull_and_the_mount)
{
    const auto refused = [](const char* mountRows, const char* expectedFragment) {
        DefDatabase db;
        std::string error;
        const std::string toml = std::string("[[ship]]\nid = \"sol.x\"\nname = \"X\"\n") + mountRows;
        if (db.mergeToml(toml.c_str(), toml.size(), "ships.toml", &error)) {
            std::printf("  expected a refusal, got a load\n");
            return false;
        }
        if (error.find(expectedFragment) == std::string::npos) {
            std::printf("  error was: %s\n  expected to contain: %s\n", error.c_str(), expectedFragment);
            return false;
        }
        // Every one of them says which file and which hull, because a mod's
        // ship def is read by somebody who did not write it.
        if (error.find("ships.toml") == std::string::npos || error.find("sol.x") == std::string::npos) {
            std::printf("  error does not name the file and the hull: %s\n", error.c_str());
            return false;
        }
        return true;
    };

    SOL_CHECK(refused("[[ship.mount]]\nkind = \"turret\"\nsize = \"small\"\n", "missing key 'id'"));
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nsize = \"small\"\n", "missing key 'kind'"));
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\n", "missing key 'size'"));
    SOL_CHECK(
        refused("[[ship.mount]]\nid = \"a\"\nkind = \"gun\"\nsize = \"small\"\n", "not a mount kind: 'gun'"));
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\nsize = \"huge\"\n", "\"xlarge\""));
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\nsize = \"small\"\nwidth = 2.0\n",
                      "unknown key 'width'"));

    // The mount id is in the message, not just the index: a hull with eight of
    // these is read by a person who wrote them by name.
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\nsize = \"small\"\n"
                      "[[ship.mount]]\nid = \"a\"\nkind = \"fixed\"\nsize = \"small\"\n",
                      "duplicate mount id 'a'"));

    // decisions/014 rule 2 read backwards: `aim` and `arc` are meaningless
    // without a position, and dropping them silently reads as a parser that ate
    // the author's turret.
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\nsize = \"small\"\naim = [0, 1, 0]\n",
                      "'aim' needs an 'at'"));
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\nsize = \"small\"\narc = 90.0\n",
                      "'arc' needs an 'at'"));
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\nsize = \"small\"\n"
                      "at = [0, 1, 0]\naim = [0, 0, 0]\n",
                      "'aim' must not be the zero vector"));
    SOL_CHECK(refused("[[ship.mount]]\nid = \"a\"\nkind = \"turret\"\nsize = \"small\"\n"
                      "at = [0, 1, 0]\narc = 400.0\n",
                      "between 0 and 360"));
    SOL_CHECK(refused("mount = 3\n", "array of tables ([[ship.mount]])"));
}

SOL_TEST(data_defs_mount_at_the_origin_is_still_external)
{
    // THE CASE THAT MOTIVATED `present` ON `optionalFloat3`. `at = [0, 0, 0]`
    // is the hull's own origin, which is a place an author may well mean, and
    // comparing the parsed vector against its default would read that mount as
    // internal - never drawn, never shootable - with nothing said.
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db,
                      R"(
[[ship]]
id = "sol.x"
name = "X"

  [[ship.mount]]
  id = "core"
  kind = "fixed"
  size = "small"
  at = [0.0, 0.0, 0.0]
)",
                      "ships.toml",
                      &error));
    const ShipDef* def = db.findShip("sol.x");
    SOL_REQUIRE(def != nullptr);
    SOL_REQUIRE(def->mounts.size() == 1);
    SOL_CHECK(def->mounts[0].external);
}

SOL_TEST(data_defs_ship_overridden_by_a_later_layer_replaces_its_mounts)
{
    // A def re-using an id replaces it WHOLESALE, and a mount list is the first
    // thing in a ship def where "wholesale" could plausibly have meant "merge".
    // A mod that removes a hull's turret has to be able to say so.
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db,
                      R"(
[[ship]]
id = "sol.x"
name = "X"

  [[ship.mount]]
  id = "gun"
  kind = "fixed"
  size = "small"

  [[ship.mount]]
  id = "drive"
  kind = "engine"
  size = "small"
)",
                      "base/ships.toml",
                      &error));
    SOL_REQUIRE(merge(db,
                      R"(
[[ship]]
id = "sol.x"
name = "X"

  [[ship.mount]]
  id = "drive"
  kind = "engine"
  size = "large"
)",
                      "mods/x/ships.toml",
                      &error));
    SOL_REQUIRE(db.ships().size() == 1);
    const ShipDef* def = db.findShip("sol.x");
    SOL_REQUIRE(def != nullptr);
    SOL_REQUIRE(def->mounts.size() == 1);
    SOL_CHECK(def->findMount("gun") == nullptr);
    SOL_REQUIRE(def->findMount("drive") != nullptr);
    SOL_CHECK(def->findMount("drive")->size == sol::assets::MountSize::Large);
}

// The shipped hulls, read out of the file a person actually wrote (Phase 31
// stage A2). A fixture would agree with the parser by construction; what is
// worth asserting is that `game/data/ships.toml` says what stage A claims it
// says, because the fit model is built on exactly this data and a typo here
// arrives as a balance change nobody made.
SOL_TEST(data_defs_shipped_hulls_carry_the_mounts_stage_a_gave_them)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    struct Expected
    {
        const char* shipId;
        std::size_t mounts;
        std::uint32_t weapons;  // turret + fixed: what `weapon` used to be
        std::uint32_t engines;  // what `slots_engine` used to be
        std::uint32_t shields;  // what `slots_shield` used to be
        std::uint32_t fittings; // utility + subsystem: cargo + utility, merged
    };

    // ⚑ THE COUNTS ARE THE OLD ONES ON PURPOSE, and this table is the proof.
    // Stage A reproduces each hull's CURRENT capacity rather than re-balancing
    // it, so that stage B's swap is arithmetic. `slots_cargo` folds into the
    // last column because gdd.md 11.5 has no `cargo` kind - a cargo pod is a
    // utility fitting - and that merge is the one real change in this file.
    //
    // ⚑ THE FREIGHTER IS THE ONE ROW THAT NO LONGER REPRODUCES THE OLD
    // CAPACITY, and that is Phase 31 stage C1 deliberately: it gained a second
    // turret because until it had one, no hull in the base game could carry
    // two guns and the phase's own exit criterion was unreachable in shipped
    // content. The mount comes BARE, so what an NPC freighter flies is
    // unchanged - which is the next test, not this one.
    constexpr Expected kHulls[3] = {
        {"sol.shuttle", 5, 1, 1, 1, 2},     // was weapon + 1/1/1/1
        {"sol.interceptor", 5, 1, 2, 1, 1}, // was weapon + 1/2/0/1
        {"sol.freighter", 9, 2, 1, 1, 5},   // was weapon + 1/1/3/2, plus C1's turret
    };

    for (const Expected& expected : kHulls) {
        const ShipDef* def = db.findShip(expected.shipId);
        SOL_REQUIRE(def != nullptr);
        if (def->mounts.size() != expected.mounts) {
            std::printf(
                "  %s has %zu mounts, expected %zu\n", expected.shipId, def->mounts.size(), expected.mounts);
        }
        SOL_CHECK(def->mounts.size() == expected.mounts);

        std::uint32_t weapons = 0;
        std::uint32_t engines = 0;
        std::uint32_t shields = 0;
        std::uint32_t fittings = 0;
        for (const sol::assets::ShipMount& mount : def->mounts) {
            switch (mount.kind) {
            case sol::assets::MountKind::Turret:
            case sol::assets::MountKind::Fixed:
                ++weapons;
                break;
            case sol::assets::MountKind::Engine:
                ++engines;
                break;
            case sol::assets::MountKind::Shield:
                ++shields;
                break;
            case sol::assets::MountKind::Utility:
            case sol::assets::MountKind::Subsystem:
                ++fittings;
                break;
            default:
                std::printf("  %s: unexpected mount kind '%s' on '%s'\n",
                            expected.shipId,
                            sol::assets::mountKindName(mount.kind),
                            mount.id.c_str());
                SOL_CHECK(false);
                break;
            }
            // Every id in the shipped file is non-empty and unique - the parser
            // enforces both, and this is the file it has to have enforced them
            // on rather than a fixture.
            SOL_CHECK(!mount.id.empty());
        }
        SOL_CHECK(weapons == expected.weapons);
        SOL_CHECK(engines == expected.engines);
        SOL_CHECK(shields == expected.shields);
        SOL_CHECK(fittings == expected.fittings);
    }

    // The freighter is the hull that carries the file's two teaching cases: the
    // only turret, and the only internal mount.
    const ShipDef* freighter = db.findShip("sol.freighter");
    SOL_REQUIRE(freighter != nullptr);
    const sol::assets::ShipMount* turret = freighter->findMount("turret_dorsal");
    SOL_REQUIRE(turret != nullptr);
    SOL_CHECK(turret->kind == sol::assets::MountKind::Turret);
    SOL_CHECK(turret->external);
    SOL_CHECK(turret->arc > 0.0f);
    // A medium mount still takes the small gun the hull flies with today, which
    // is why converting the hull did not have to touch `weapon`.
    SOL_CHECK(turret->size == sol::assets::MountSize::Medium);
    SOL_CHECK(sol::assets::mountAccepts(turret->size, sol::assets::MountSize::Small));

    const sol::assets::ShipMount* sensor = freighter->findMount("core_sensor");
    SOL_REQUIRE(sensor != nullptr);
    SOL_CHECK(!sensor->external);
    SOL_CHECK(sensor->kind == sol::assets::MountKind::Subsystem);

    // ⚑ AND THE SECOND GUN MOUNT, WHICH IS THE ONE PIECE OF CONTENT PHASE 31
    // STAGE C1 ADDED. Both halves are asserted because either alone is
    // satisfied by the wrong thing: that it EXISTS (without it nothing in the
    // base game can carry two guns) and that it is BARE (with a `fit` it would
    // have doubled every NPC freighter's firepower, a balance change nobody
    // asked the stage to make).
    const sol::assets::ShipMount* ventral = freighter->findMount("turret_ventral");
    SOL_REQUIRE(ventral != nullptr);
    SOL_CHECK(ventral->kind == sol::assets::MountKind::Turret);
    SOL_CHECK(ventral->external);
    SOL_CHECK(ventral->fit.empty());
}

// ⚑ A WEAPON MOUNT'S `at` IS A MUZZLE SINCE PHASE 31 STAGE C1, so a gun mount
// left at the hull's origin now fires from inside the ship - and nothing else
// about the def would look wrong. Stage A2 authored every position INSIDE the
// shared `ship` mesh because nothing read them; the drive-visible half of that
// (the shuttle's nose gun sat behind the pilot's seat) is a game-side fact and
// is asserted there. What the def layer can say on its own is that somebody
// placed the thing at all.
SOL_TEST(data_defs_every_shipped_gun_mount_is_external_and_placed)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(!db.ships().empty());

    std::uint32_t checked = 0;
    for (const ShipDef& def : db.ships()) {
        for (const sol::assets::ShipMount& mount : def.mounts) {
            if (!sol::assets::mountTakesWeapon(mount.kind)) {
                continue;
            }
            ++checked;
            if (!mount.external) {
                std::printf("  %s: gun mount '%s' has no `at`\n", def.id.c_str(), mount.id.c_str());
            }
            SOL_CHECK(mount.external);
            const bool placed = mount.at[0] != 0.0f || mount.at[1] != 0.0f || mount.at[2] != 0.0f;
            if (!placed) {
                std::printf(
                    "  %s: gun mount '%s' sits on the hull's origin\n", def.id.c_str(), mount.id.c_str());
            }
            SOL_CHECK(placed);
        }
    }
    // Four gun mounts across three hulls; a hull whose mounts stopped parsing
    // would pass every check above by having none to fail.
    SOL_CHECK(checked == 4);
}

// ⚑ WHERE `weapon =` WENT (Phase 31 stage B). Each hull's gun is now a `fit`
// on a mount, and this is the assertion that the deletion of the key did not
// quietly disarm the base game - `applyShipDef` walks the mounts, so a hull
// whose gun mount lost its `fit` flies unarmed and NOTHING ELSE CHANGES about
// it, which is a defect no crash and no warning would announce.
SOL_TEST(data_defs_shipped_hulls_come_fitted_with_the_gun_the_weapon_key_named)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    struct Expected
    {
        const char* shipId;
        const char* mountId;
        const char* weaponId; // what `weapon =` said before stage B
    };

    constexpr Expected kArmed[3] = {
        {"sol.shuttle", "gun_nose", "sol.pulse_cannon"},
        {"sol.interceptor", "gun_nose", "sol.pulse_cannon"},
        {"sol.freighter", "turret_dorsal", "sol.mining_laser"},
    };

    for (const Expected& expected : kArmed) {
        const ShipDef* def = db.findShip(expected.shipId);
        SOL_REQUIRE(def != nullptr);
        const sol::assets::ShipMount* mount = def->findMount(expected.mountId);
        SOL_REQUIRE(mount != nullptr);
        if (mount->fit != expected.weaponId) {
            std::printf("  %s mount '%s' fits '%s', expected '%s'\n",
                        expected.shipId,
                        expected.mountId,
                        mount->fit.c_str(),
                        expected.weaponId);
        }
        SOL_CHECK(mount->fit == expected.weaponId);

        // And the gun the def names actually FITS the mount the def puts it in,
        // which is the check that would catch an author moving a heavy gun onto
        // a small hardpoint. The whole rule, kind and size together.
        const sol::assets::WeaponDef* weapon = db.findWeapon(expected.weaponId);
        SOL_REQUIRE(weapon != nullptr);
        SOL_CHECK(sol::assets::mountAccepts(*mount, weapon->mount, weapon->size));
    }

    // ⚑ THE FREIGHTER IS THE ASYMMETRY, LIVE IN SHIPPED CONTENT: its mount is
    // a `turret` and its gun is authored `fixed`. That pairing only works
    // because a ring holds a bare gun, and it is why converting the hulls did
    // not require authoring every weapon twice.
    const sol::assets::WeaponDef* laser = db.findWeapon("sol.mining_laser");
    SOL_REQUIRE(laser != nullptr);
    SOL_CHECK(laser->mount == sol::assets::MountKind::Fixed);
    SOL_CHECK(db.findShip("sol.freighter")->findMount("turret_dorsal")->kind ==
              sol::assets::MountKind::Turret);

    // ⚑ AND THE ONE THING THE BASE GAME CANNOT DO YET, PINNED SO THAT FILLING
    // IT IS A DELIBERATE ACT. No shipped component is a `subsystem`, so the
    // freighter's internal mount accepts nothing that exists. Phase 32 authors
    // the kit; until then this is the honest state and the test says so.
    const bool anySubsystemComponent =
        std::any_of(db.components().begin(), db.components().end(), [](const auto& component) {
            return component.mount == sol::assets::MountKind::Subsystem;
        });
    SOL_CHECK(!anySubsystemComponent);
}

// ⚑ EVERY SHIPPED FITTING STILL FITS THE HULL IT FITTED BEFORE MOUNTS. Stage
// B's conversion promised not to re-balance under cover of a schema change,
// and this is the promise stated as an assertion rather than as a comment in a
// data file: every component in the catalog has a place on at least one of the
// three hulls. A `medium` Mk2 tier - the balance change the file explicitly
// declines to make in passing - turns this red on the shuttle-only kit.
SOL_TEST(data_defs_every_shipped_component_fits_some_shipped_hull)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(!db.components().empty());

    for (const sol::assets::ComponentDef& component : db.components()) {
        bool fitsSomewhere = false;
        for (const ShipDef& hull : db.ships()) {
            for (const sol::assets::ShipMount& mount : hull.mounts) {
                fitsSomewhere =
                    fitsSomewhere || sol::assets::mountAccepts(mount, component.mount, component.size);
            }
        }
        if (!fitsSomewhere) {
            std::printf("  component '%s' (%s %s) fits no shipped hull\n",
                        component.id.c_str(),
                        sol::assets::mountSizeName(component.size),
                        sol::assets::mountKindName(component.mount));
        }
        SOL_CHECK(fitsSomewhere);
    }

    // The shuttle is the one that matters, because it is what a new player
    // flies: it must still take a shield, an engine tuner, a cargo pod and a
    // survey scanner, which is exactly what its four non-gun mounts were
    // converted from.
    const ShipDef* shuttle = db.findShip("sol.shuttle");
    SOL_REQUIRE(shuttle != nullptr);
    for (const char* id :
         {"sol.shield_booster_mk2", "sol.engine_tuner_mk2", "sol.cargo_pod_mk2", "sol.survey_scanner_mk2"}) {
        const sol::assets::ComponentDef* component = db.findComponent(id);
        SOL_REQUIRE(component != nullptr);
        bool fits = false;
        for (const sol::assets::ShipMount& mount : shuttle->mounts) {
            fits = fits || sol::assets::mountAccepts(mount, component->mount, component->size);
        }
        if (!fits) {
            std::printf("  the shuttle can no longer carry '%s'\n", id);
        }
        SOL_CHECK(fits);
    }
}

// --- the hull spine (Phase 32 stage A) ---------------------------------------

SOL_TEST(data_defs_hull_class_names_round_trip)
{
    for (std::size_t i = 0; i < sol::assets::kHullClassCount; ++i) {
        const auto hullClass = static_cast<sol::assets::HullClass>(i);
        sol::assets::HullClass parsed{};
        SOL_REQUIRE(sol::assets::parseHullClass(sol::assets::hullClassName(hullClass), parsed));
        SOL_CHECK(parsed == hullClass);
    }
    // ⚑ A round trip alone cannot see a ROTATED table, so both ends are pinned
    // to gdd.md 11.1's own rows - and the ordinal IS the class number there,
    // which is what the Forge's combo prints beside the word.
    SOL_CHECK(std::strcmp(sol::assets::hullClassName(sol::assets::HullClass::Skiff), "skiff") == 0);
    SOL_CHECK(std::strcmp(sol::assets::hullClassName(sol::assets::HullClass::Titan), "titan") == 0);
    SOL_CHECK(static_cast<std::uint32_t>(sol::assets::HullClass::Heavy) == 3);
    SOL_CHECK(std::strcmp(sol::assets::hullClassName(sol::assets::HullClass::Count), "?") == 0);

    sol::assets::HullClass unused{};
    SOL_CHECK(!sol::assets::parseHullClass("Heavy", unused)); // spellings are lowercase
    // ⚑ 11.1 hyphenates the sixth row and a def spelling is a token, so the
    // typography is NOT the word - this is the one an author will try.
    SOL_CHECK(!sol::assets::parseHullClass("super-capital", unused));
    SOL_CHECK(sol::assets::parseHullClass("supercapital", unused));
    SOL_CHECK(unused == sol::assets::HullClass::SuperCapital);
    // ⚑⚑ AND THE NUMBER IS NOT A SPELLING, which is the checkpoint ruling said
    // as a test: the first cut of this key was `class = 3`.
    SOL_CHECK(!sol::assets::parseHullClass("3", unused));
    SOL_CHECK(!sol::assets::parseHullClass("", unused));
}

SOL_TEST(data_defs_hull_role_names_round_trip)
{
    for (std::size_t i = 0; i < sol::assets::kHullRoleCount; ++i) {
        const auto role = static_cast<sol::assets::HullRole>(i);
        sol::assets::HullRole parsed{};
        SOL_REQUIRE(sol::assets::parseHullRole(sol::assets::hullRoleName(role), parsed));
        SOL_CHECK(parsed == role);
    }
    // ⚑ A round trip alone cannot see a ROTATED table - both directions read
    // it, so shifting every entry by one agrees with itself perfectly. The two
    // ends are pinned to the words gdd.md 11.2 actually uses.
    SOL_CHECK(std::strcmp(sol::assets::hullRoleName(sol::assets::HullRole::Line), "line") == 0);
    SOL_CHECK(std::strcmp(sol::assets::hullRoleName(sol::assets::HullRole::Industrial), "industrial") == 0);

    sol::assets::HullRole unused{};
    SOL_CHECK(!sol::assets::parseHullRole("Line", unused));     // spellings are lowercase
    SOL_CHECK(!sol::assets::parseHullRole("economic", unused)); // 11.2's parenthetical, not its name
    SOL_CHECK(!sol::assets::parseHullRole("", unused));
}

// ⚑⚑ THE BANDS PARTITION THE NUMBER LINE FROM 8 m UP, AND THE BOUNDARIES ARE
// WHAT THIS PINS. A value strictly inside a band agrees with an inclusive and
// an exclusive top alike, so every case here is a bound EXACTLY: 20.0 m has to
// be Light and cannot also be Skiff, or two classes claim one hull and the
// warning fires on whichever the code happened to test first.
SOL_TEST(data_defs_hull_class_bands_are_gdd_11_1_and_meet_without_overlapping)
{
    using sol::assets::HullClass;
    using sol::assets::hullClassBand;
    using sol::assets::HullClassBand;

    const float expected[8][2] = {{8.0f, 20.0f},
                                  {20.0f, 45.0f},
                                  {45.0f, 120.0f},
                                  {120.0f, 300.0f},
                                  {300.0f, 600.0f},
                                  {600.0f, 1200.0f},
                                  {1200.0f, 3000.0f},
                                  {3000.0f, 0.0f}};
    for (std::size_t i = 0; i < sol::assets::kHullClassCount; ++i) {
        const HullClassBand band = hullClassBand(static_cast<HullClass>(i));
        SOL_CHECK(band.minLength == expected[i][0]);
        if (i + 1 < sol::assets::kHullClassCount) {
            SOL_CHECK(band.maxLength == expected[i][1]);
            // Each band's top IS the next band's bottom: no gap, no overlap.
            SOL_CHECK(band.maxLength == hullClassBand(static_cast<HullClass>(i + 1)).minLength);
        }
    }
    // 11.1 writes the titan band as "3 km+", and a big number is a length a
    // titan could be authored past.
    SOL_CHECK(std::isinf(hullClassBand(HullClass::Titan).maxLength));

    // ⚑ `Count` is not a class, and it is reachable only by a cast - which is
    // exactly what a value read back from somewhere else is.
    SOL_CHECK(hullClassBand(HullClass::Count).minLength == 0.0f);
    SOL_CHECK(hullClassBand(HullClass::Count).maxLength == 0.0f);
}

SOL_TEST(data_defs_hull_class_for_length_lands_on_the_lower_side_of_every_bound)
{
    using sol::assets::HullClass;
    using sol::assets::hullClassForLength;
    HullClass found = HullClass::Count;

    // Every boundary, from the band it OPENS. Inclusive bottom, exclusive top.
    const float bounds[8] = {8.0f, 20.0f, 45.0f, 120.0f, 300.0f, 600.0f, 1200.0f, 3000.0f};
    for (std::size_t i = 0; i < 8; ++i) {
        SOL_REQUIRE(hullClassForLength(bounds[i], found));
        SOL_CHECK(found == static_cast<HullClass>(i));
    }
    // And a hair under each one falls back a class.
    for (std::size_t i = 1; i < 8; ++i) {
        SOL_REQUIRE(hullClassForLength(std::nextafter(bounds[i], 0.0f), found));
        SOL_CHECK(found == static_cast<HullClass>(i - 1));
    }

    // ⚑ BELOW THE SMALLEST BAND THERE IS NO ANSWER, and answering `skiff` would
    // be this function inventing one the table declines to give: under 8 m is
    // not a small ship, it is a fitting. A cannon mesh measures about a metre.
    SOL_CHECK(!hullClassForLength(7.99f, found));
    SOL_CHECK(!hullClassForLength(0.0f, found));
    SOL_CHECK(!hullClassForLength(-5.0f, found));

    // A titan has no top: nothing is too long to have a class.
    SOL_REQUIRE(hullClassForLength(50000.0f, found));
    SOL_CHECK(found == HullClass::Titan);
}

SOL_TEST(data_defs_hull_length_in_band_agrees_with_the_class_a_length_measures)
{
    using sol::assets::HullClass;
    using sol::assets::hullClassForLength;
    using sol::assets::hullLengthInBand;

    // The two are one rule asked two ways, so they cannot be allowed to drift:
    // a length is in class C's band exactly when it MEASURES class C.
    const float lengths[] = {8.0f, 12.0f, 19.999f, 20.0f, 48.0f, 120.0f, 3000.0f, 9000.0f};
    for (const float metres : lengths) {
        HullClass measured = HullClass::Count;
        SOL_REQUIRE(hullClassForLength(metres, measured));
        for (std::size_t i = 0; i < sol::assets::kHullClassCount; ++i) {
            const auto declared = static_cast<HullClass>(i);
            SOL_CHECK(hullLengthInBand(declared, metres) == (declared == measured));
        }
    }
    // A length below every band is in no band at all.
    for (std::size_t i = 0; i < sol::assets::kHullClassCount; ++i) {
        SOL_CHECK(!hullLengthInBand(static_cast<HullClass>(i), 4.0f));
    }
    // And `Count` is not a class, so nothing is inside it.
    SOL_CHECK(!hullLengthInBand(HullClass::Count, 12.0f));
}

SOL_TEST(data_defs_ship_class_and_role_are_optional_and_record_that_they_were_authored)
{
    DefDatabase db;
    std::string error;
    const char* toml = R"(
[[ship]]
id = "sol.declared"
name = "Declared"
class = "heavy"
role = "logistics"

[[ship]]
id = "sol.silent"
name = "Silent"
)";
    SOL_REQUIRE(merge(db, toml, "ships.toml", &error));
    SOL_CHECK(error.empty());

    const ShipDef* declared = db.findShip("sol.declared");
    SOL_REQUIRE(declared != nullptr);
    SOL_CHECK(declared->hasHullClass);
    SOL_CHECK(declared->hullClass == sol::assets::HullClass::Heavy);
    SOL_CHECK(declared->hasRole);
    SOL_CHECK(declared->role == sol::assets::HullRole::Logistics);

    // ⚑ THE FLAGS ARE THE POINT AND A SENTINEL COULD NOT DO THIS JOB: `skiff`
    // and `line` are the first members of their sets, so an unauthored hull
    // reads as a perfectly ordinary skiff fighter unless it can say it never
    // spoke.
    const ShipDef* silent = db.findShip("sol.silent");
    SOL_REQUIRE(silent != nullptr);
    SOL_CHECK(!silent->hasHullClass);
    SOL_CHECK(!silent->hasRole);
    SOL_CHECK(silent->hullClass == sol::assets::HullClass::Skiff);
    SOL_CHECK(silent->role == sol::assets::HullRole::Line);
}

SOL_TEST(data_defs_ship_refuses_a_class_outside_the_bands_and_a_role_that_is_not_one)
{
    const auto refused = [](const char* keys, const char* expectedFragment) {
        DefDatabase db;
        std::string error;
        const std::string toml = std::string("[[ship]]\nid = \"sol.x\"\nname = \"X\"\n") + keys;
        if (db.mergeToml(toml.c_str(), toml.size(), "ships.toml", &error)) {
            std::printf("  expected a refusal, got a load\n");
            return false;
        }
        if (error.find(expectedFragment) == std::string::npos) {
            std::printf("  error was '%s', expected to mention '%s'\n", error.c_str(), expectedFragment);
            return false;
        }
        return true;
    };

    // ⚑⚑ THE NUMBER IS THE MISTAKE THE CHECKPOINT CREATED, and it has to be
    // refused rather than quietly read: gdd.md 11.1 numbers its rows, the first
    // cut of this key took `class = 3`, and every reader of that table will try
    // it. A TOML integer is not a string, so it fails on the type before it
    // fails on the word - and both messages name the key.
    SOL_CHECK(refused("class = 3\n", "'class'"));
    SOL_CHECK(refused("class = \"9\"\n", "not a hull class"));
    SOL_CHECK(refused("class = \"super-capital\"\n", "not a hull class"));
    SOL_CHECK(refused("role = \"freighter\"\n", "not a hull role"));
    // The word is in gdd.md 11.2 but it is the family's parenthetical, not its
    // name - which is exactly the mistake an author makes reading that table.
    SOL_CHECK(refused("role = \"economic\"\n", "not a hull role"));
}

// ⚑⚑⚑ THE SHIPPED CONTENT, AND IT IS OUT OF BAND ON PURPOSE - THIS TEST EXISTS
// TO SAY SO OUT LOUD RATHER THAN LET A LATER SESSION "FIX" IT. Every hull in
// this game shares one 12 m placeholder mesh stretched by `scale`, so the class
// each one DECLARES is gdd.md 11.3's cell for that ship type while the geometry
// is a stand-in. The user's ruling: the bands are right, the content is wrong,
// and the meshes are not ready - so what is authored is the durable half.
//
// The LENGTH half of the claim is measured in `forge.unit`, which is the only
// suite that can open a mesh; this one pins what the file says.
SOL_TEST(data_defs_every_shipped_hull_declares_a_class_and_a_role)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_CHECK(error.empty());
    SOL_REQUIRE(!db.ships().empty());

    struct Expected
    {
        const char* shipId;
        sol::assets::HullClass hullClass;
        sol::assets::HullRole role;
    };

    using sol::assets::HullClass;
    const Expected kHulls[] = {
        {"sol.shuttle", HullClass::Light, sol::assets::HullRole::Logistics},   // 11.3: Light / Logistics
        {"sol.interceptor", HullClass::Light, sol::assets::HullRole::Line},    // 11.3: Light / Line
        {"sol.freighter", HullClass::Heavy, sol::assets::HullRole::Logistics}, // 11.3: Heavy / Logistics
    };
    for (const Expected& want : kHulls) {
        const ShipDef* def = db.findShip(want.shipId);
        SOL_REQUIRE(def != nullptr);
        SOL_CHECK(def->hasHullClass);
        SOL_CHECK(def->hullClass == want.hullClass);
        SOL_CHECK(def->hasRole);
        SOL_CHECK(def->role == want.role);
    }

    // Nothing in the base game may go unclassified: a hull with no class is one
    // the spine cannot report on at all, which is worse than one out of band.
    for (const ShipDef& def : db.ships()) {
        if (!def.hasHullClass || !def.hasRole) {
            std::printf("  '%s' declares no %s\n", def.id.c_str(), def.hasHullClass ? "role" : "class");
        }
        SOL_CHECK(def.hasHullClass);
        SOL_CHECK(def.hasRole);
    }
}

// ---------------------------------------------------------------------------
// Station modules (Phase 34 stage A). The vocabulary gdd.md §12 asks for, with
// no engine reader yet - so these tests ARE the reader until stage B arrives,
// and every rule the parser enforces is a rule somebody wrote down on purpose.
// ---------------------------------------------------------------------------

SOL_TEST(data_defs_module_family_screen_and_goods_class_names_round_trip)
{
    for (std::size_t i = 0; i < sol::assets::kModuleFamilyCount; ++i) {
        const auto family = static_cast<sol::assets::ModuleFamily>(i);
        sol::assets::ModuleFamily parsed{};
        SOL_REQUIRE(sol::assets::parseModuleFamily(sol::assets::moduleFamilyName(family), parsed));
        SOL_CHECK(parsed == family);
    }
    for (std::size_t i = 0; i < sol::assets::kStationScreenCount; ++i) {
        const auto screen = static_cast<sol::assets::StationScreen>(i);
        sol::assets::StationScreen parsed{};
        SOL_REQUIRE(sol::assets::parseStationScreen(sol::assets::stationScreenName(screen), parsed));
        SOL_CHECK(parsed == screen);
    }
    for (std::size_t i = 0; i < sol::assets::kGoodsClassCount; ++i) {
        const auto goods = static_cast<sol::assets::GoodsClass>(i);
        sol::assets::GoodsClass parsed{};
        SOL_REQUIRE(sol::assets::parseGoodsClass(sol::assets::goodsClassName(goods), parsed));
        SOL_CHECK(parsed == goods);
    }

    // ⚑ A round trip alone cannot see a ROTATED table - both directions read
    // it, so shifting every entry by one agrees with itself perfectly. Both
    // ends of each list are pinned to gdd.md §12's own words.
    using sol::assets::GoodsClass;
    using sol::assets::ModuleFamily;
    using sol::assets::StationScreen;
    SOL_CHECK(std::strcmp(sol::assets::moduleFamilyName(ModuleFamily::Power), "power") == 0);
    SOL_CHECK(std::strcmp(sol::assets::moduleFamilyName(ModuleFamily::Shadow), "shadow") == 0);
    SOL_CHECK(std::strcmp(sol::assets::stationScreenName(StationScreen::Trade), "trade") == 0);
    SOL_CHECK(std::strcmp(sol::assets::stationScreenName(StationScreen::Refinery), "refinery") == 0);
    SOL_CHECK(std::strcmp(sol::assets::goodsClassName(GoodsClass::Bulk), "bulk") == 0);
    SOL_CHECK(std::strcmp(sol::assets::goodsClassName(GoodsClass::Hazardous), "hazardous") == 0);
    SOL_CHECK(std::strcmp(sol::assets::moduleFamilyName(ModuleFamily::Count), "?") == 0);

    // ⚑⚑ THE SCREEN NAMES ARE HALF OF A PARALLEL PAIR AND THIS IS WHERE THAT
    // IS PINNED FROM THIS SIDE. `game::StationScreenState::Tab` is the other
    // half and Phase 34 stage C owns the mapping; what this layer can promise is
    // that the vocabulary has exactly the entries that tab strip has, in the
    // order the strip draws them - so a mapping written there has something to
    // be checked against.
    //
    // ⚑⚑⚑ NINE SINCE PHASE 35 STAGE A, AND THE COUNT IS PINNED RATHER THAN
    // DERIVED ON PURPOSE. `kStationScreenCount` is `StationScreen::Count`, so a
    // test written as "the count equals the count" would pass on the day
    // somebody adds a screen here and forgets the strip, the label, the hint and
    // the `static_assert` in `station_screen.cpp` that ties the two enums
    // together. A literal is the only form of this check that can fail.
    SOL_CHECK(sol::assets::kStationScreenCount == 9);
    SOL_CHECK(static_cast<std::uint32_t>(StationScreen::Outfitting) == 1);
    SOL_CHECK(static_cast<std::uint32_t>(StationScreen::Survey) == 6);
    // Appended rather than slotted in beside Missions, which is what kept every
    // number above where it was.
    SOL_CHECK(static_cast<std::uint32_t>(StationScreen::Bar) == 8);
    SOL_CHECK(std::strcmp(sol::assets::stationScreenName(StationScreen::Bar), "bar") == 0);
    StationScreen screen{};
    SOL_CHECK(sol::assets::parseStationScreen("bar", screen) && screen == StationScreen::Bar);

    sol::assets::ModuleFamily family{};
    SOL_CHECK(!sol::assets::parseModuleFamily("Power", family)); // spellings are lowercase
    SOL_CHECK(!sol::assets::parseModuleFamily("service", family)); // §12's heading is plural
    SOL_CHECK(!sol::assets::parseModuleFamily("", family));
    sol::assets::GoodsClass goods{};
    SOL_CHECK(!sol::assets::parseGoodsClass("hazmat", goods)); // the id is short, the class is not
}

SOL_TEST(data_defs_module_reads_every_key_gdd_12_asks_for)
{
    DefDatabase db;
    std::string error;
    const char* toml = R"(
[[module]]
id = "sol.mod_test_works"
name = "Test Works"
family = "industry"
produces = ["sol.metal:0.2"]
feedstock = ["sol.ore:0.31"]
consumes = ["sol.food:0.035"]
stores = ["bulk:1200", "hazardous:400"]
power_output = 0.0
power_draw = 12.5
screens = ["trade", "refinery"]
refine_input = "sol.ore"
refine_output = "sol.metal"

[[module]]
id = "sol.mod_test_plant"
name = "Test Plant"
family = "power"
power_output = 30.0
power_draw = 1.0
)";
    SOL_REQUIRE(merge(db, toml, "modules.toml", &error));
    SOL_CHECK(error.empty());

    const sol::assets::ModuleDef* works = db.findModule("sol.mod_test_works");
    SOL_REQUIRE(works != nullptr);
    SOL_CHECK(works->family == sol::assets::ModuleFamily::Industry);
    SOL_REQUIRE(works->produces.size() == 1);
    SOL_CHECK(works->produces[0].commodityId == "sol.metal");
    SOL_CHECK(works->produces[0].rate == 0.2f);
    SOL_REQUIRE(works->feedstock.size() == 1);
    SOL_CHECK(works->feedstock[0].commodityId == "sol.ore");
    SOL_REQUIRE(works->consumes.size() == 1);
    SOL_CHECK(works->consumes[0].commodityId == "sol.food");
    SOL_REQUIRE(works->stores.size() == 2);
    SOL_CHECK(works->stores[0].goods == sol::assets::GoodsClass::Bulk);
    SOL_CHECK(works->stores[0].capacity == 1200.0f);
    SOL_CHECK(works->stores[1].goods == sol::assets::GoodsClass::Hazardous);
    SOL_CHECK(works->stores[1].capacity == 400.0f);
    SOL_CHECK(works->powerDraw == 12.5f);
    SOL_REQUIRE(works->screens.size() == 2);
    SOL_CHECK(works->screens[0] == sol::assets::StationScreen::Trade);
    SOL_CHECK(works->screens[1] == sol::assets::StationScreen::Refinery);
    SOL_CHECK(works->refineInput == "sol.ore");
    SOL_CHECK(works->refineOutput == "sol.metal");

    const sol::assets::ModuleDef* plant = db.findModule("sol.mod_test_plant");
    SOL_REQUIRE(plant != nullptr);
    SOL_CHECK(plant->family == sol::assets::ModuleFamily::Power);
    SOL_CHECK(plant->powerOutput == 30.0f);
    // A module that says nothing about a list has an empty one - there is no
    // default hold, no default screen and no default rate, because a module
    // that quietly held something would compose a station nobody authored.
    SOL_CHECK(plant->stores.empty());
    SOL_CHECK(plant->screens.empty());
    SOL_CHECK(plant->produces.empty());
    SOL_CHECK(plant->refineInput.empty());

    // A later layer replaces a row in place, exactly as every other def kind
    // does - a mod re-points a module the way it re-points a ship.
    const char* mod = R"(
[[module]]
id = "sol.mod_test_plant"
name = "Test Plant II"
family = "power"
power_output = 45.0
)";
    SOL_REQUIRE(merge(db, mod, "mod.toml", &error));
    SOL_REQUIRE(db.modules().size() == 2); // replaced, not appended
    plant = db.findModule("sol.mod_test_plant");
    SOL_REQUIRE(plant != nullptr);
    SOL_CHECK(plant->name == "Test Plant II");
    SOL_CHECK(plant->powerOutput == 45.0f);
}

SOL_TEST(data_defs_module_refuses_what_the_composer_would_have_to_believe)
{
    const auto refused = [](const char* keys, const char* expectedFragment) {
        DefDatabase db;
        std::string error;
        const std::string toml = std::string("[[module]]\nid = \"sol.mod_x\"\nname = \"X\"\n") + keys;
        if (db.mergeToml(toml.c_str(), toml.size(), "modules.toml", &error)) {
            std::printf("  expected a refusal, got a load\n");
            return false;
        }
        if (error.find(expectedFragment) == std::string::npos) {
            std::printf("  error was '%s', expected to mention '%s'\n", error.c_str(), expectedFragment);
            return false;
        }
        return true;
    };

    // ⚑ The family is REQUIRED, not optional-with-a-default: every family is a
    // real answer, so there is no spelling left to mean "nobody said".
    SOL_CHECK(refused("power_draw = 1.0\n", "family"));
    SOL_CHECK(refused("family = \"industrial\"\n", "not a module family"));
    SOL_CHECK(refused("family = 3\n", "'family'"));

    // ⚑⚑ THE RULING, ENFORCED WHERE AN AUTHOR MEETS IT: power is the composer's
    // constraint, so a habitat ring that made its own power would satisfy stage
    // B's validity check by opting out of it.
    SOL_CHECK(refused("family = \"habitat\"\npower_output = 5.0\n", "only the power family makes power"));
    SOL_CHECK(refused("family = \"power\"\npower_output = -1.0\n", ">= 0"));
    SOL_CHECK(refused("family = \"storage\"\npower_draw = -0.5\n", ">= 0"));

    SOL_CHECK(refused("family = \"storage\"\nstores = [\"bulk\"]\n", "class:capacity"));
    SOL_CHECK(refused("family = \"storage\"\nstores = [\"vault:100\"]\n", "no goods class"));
    SOL_CHECK(refused("family = \"storage\"\nstores = [\"bulk:0\"]\n", "capacity > 0"));
    // Two holds of one class on ONE module is a typo; across a station they add
    // up, and that sum is stage D's arithmetic rather than the parser's.
    SOL_CHECK(refused("family = \"storage\"\nstores = [\"bulk:100\", \"bulk:200\"]\n", "twice"));

    SOL_CHECK(refused("family = \"commerce\"\nscreens = [\"market\"]\n", "no dock screen"));
    SOL_CHECK(refused("family = \"commerce\"\nscreens = [\"trade\", \"trade\"]\n", "twice"));

    // Half a refining service is a station that takes your ore and hands back
    // nothing, which is the same rule `[[station]]` has carried since Phase 8f.
    SOL_CHECK(refused("family = \"services\"\nrefine_input = \"sol.ore\"\n", "must be given together"));
    SOL_CHECK(refused("family = \"services\"\nrefine_output = \"sol.metal\"\n", "must be given together"));

    // ⚑⚑ AND THE SYMMETRIC RULE, WHICH IS WHAT LET STAGE C DELETE AN EMPTY TAB.
    // The Refining tab is gated on `screens` while the service it draws comes
    // from the pair, so a row carrying one without the other is either a tab
    // with nothing behind it or a service with no way in. Until stage C that
    // mistake was invisible: the tab was on every station in the galaxy and
    // simply read "(this station refines nothing)".
    SOL_CHECK(refused("family = \"services\"\nscreens = [\"refinery\"]\n", "names no 'refine_input'"));
    SOL_CHECK(refused("family = \"services\"\nrefine_input = \"sol.ore\"\n"
                      "refine_output = \"sol.metal\"\n",
                      "does not offer the 'refinery' screen"));
    // ⚑ The negative control needs no line here: `sol.mod_market_floor` offers
    // `trade` and names no pair, and every test that loads the shipped defs
    // proves the rule leaves a screen list that does not mention refining alone.

    SOL_CHECK(refused("family = \"power\"\nreactor = 4\n", "reactor"));
}

// ⚑⚑⚑ THE SHIPPED VOCABULARY, AND THIS TEST IS THE ONLY READER IT HAS UNTIL
// STAGE B. A file nobody reads cannot go wrong loudly, so what is pinned here
// is exactly what the later stages will lean on: all eight of gdd.md §12's
// families are represented, every screen a module offers is one a station
// actually draws, and the two refining services name the pairs the shipped
// archetypes already offer.
SOL_TEST(data_defs_shipped_modules_cover_every_family_gdd_12_names)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_CHECK(error.empty());
    SOL_REQUIRE(!db.modules().empty());

    bool seen[sol::assets::kModuleFamilyCount] = {};
    for (const sol::assets::ModuleDef& module : db.modules()) {
        seen[static_cast<std::size_t>(module.family)] = true;
    }
    for (std::size_t i = 0; i < sol::assets::kModuleFamilyCount; ++i) {
        if (!seen[i]) {
            std::printf("  no shipped module is '%s'\n",
                        sol::assets::moduleFamilyName(static_cast<sol::assets::ModuleFamily>(i)));
        }
        SOL_CHECK(seen[i]);
    }

    // ⚑ Every screen offered by a module, and every refining pair, names
    // something real. Nothing enforces this at load - an unknown commodity id
    // is a downstream warning by this project's standing rule - so the shipped
    // content is checked here instead, where a typo is a failing test rather
    // than a service that silently refines nothing.
    for (const sol::assets::ModuleDef& module : db.modules()) {
        if (!module.refineInput.empty()) {
            if (db.findCommodity(module.refineInput.c_str()) == nullptr ||
                db.findCommodity(module.refineOutput.c_str()) == nullptr) {
                std::printf("  module '%s' refines '%s' -> '%s', and one of those is not a commodity\n",
                            module.id.c_str(),
                            module.refineInput.c_str(),
                            module.refineOutput.c_str());
            }
            SOL_CHECK(db.findCommodity(module.refineInput.c_str()) != nullptr);
            SOL_CHECK(db.findCommodity(module.refineOutput.c_str()) != nullptr);
        }
    }

    // The two services the shipped archetypes already offer (Phase 8f's ore ->
    // metal, Phase 33 stage C's salvage -> alloy), which is why gdd.md §12's
    // single "refinery service" line is two rows in `modules.toml`.
    const sol::assets::ModuleDef* ore = db.findModule("sol.mod_refinery_service");
    const sol::assets::ModuleDef* salvage = db.findModule("sol.mod_reclaim_service");
    SOL_REQUIRE(ore != nullptr && salvage != nullptr);
    SOL_CHECK(ore->refineInput == "sol.ore" && ore->refineOutput == "sol.metal");
    SOL_CHECK(salvage->refineInput == "sol.salvage" && salvage->refineOutput == "sol.alloy");

    // ⚑⚑⚑ THE ASSERTION STAGE B DELETED, AND WHAT IT LEFT BEHIND. Stage A
    // shipped this file with every rate list EMPTY and a check that said so,
    // because a rate authored before the recipes exist is a number nobody
    // searched. Stage B authored the recipes and the rates together, so the
    // emptiness check is gone as designed - and what replaces it is the shape
    // that survives: only INDUSTRY and HABITAT touch the economy. Industry is
    // the production graph of gdd.md §6; habitat is the food line, by the
    // ruling. A market floor that quietly consumed something, or a casino that
    // produced it, would be an economy nobody could find by reading
    // `stations.toml` - which is exactly what the decomposition must not cost.
    for (const sol::assets::ModuleDef& module : db.modules()) {
        const bool hasRates =
            !module.produces.empty() || !module.consumes.empty() || !module.feedstock.empty();
        const bool mayHaveRates = module.family == sol::assets::ModuleFamily::Industry ||
                                  module.family == sol::assets::ModuleFamily::Habitat;
        if (hasRates && !mayHaveRates) {
            std::printf("  '%s' is %s and carries a rate line; only industry and habitat may\n",
                        module.id.c_str(),
                        sol::assets::moduleFamilyName(module.family));
        }
        SOL_CHECK(!hasRates || mayHaveRates);
    }
    // And the other direction, which is the half that catches a module falling
    // out of a recipe rather than one appearing in it: an industry module with
    // no rates at all is a production line that produces nothing. The Drydock is
    // the one deliberate exception - it repairs, it does not make - so it is
    // named here rather than left to weaken the rule for everybody.
    for (const sol::assets::ModuleDef& module : db.modules()) {
        if (module.family != sol::assets::ModuleFamily::Industry || module.id == "sol.mod_drydock") {
            continue;
        }
        if (module.produces.empty() && module.feedstock.empty()) {
            std::printf("  industry module '%s' neither makes nor consumes anything\n", module.id.c_str());
        }
        SOL_CHECK(!module.produces.empty() || !module.feedstock.empty());
    }

    // Every power figure is somebody's design statement, but a station that
    // cannot be built out of these at all would be a vocabulary with no legal
    // sentence in it: the largest single draw has to be coverable by the
    // largest single plant, or stage B's composer can never satisfy its rule.
    float biggestDraw = 0.0f;
    float biggestOutput = 0.0f;
    for (const sol::assets::ModuleDef& module : db.modules()) {
        biggestDraw = std::max(biggestDraw, module.powerDraw);
        biggestOutput = std::max(biggestOutput, module.powerOutput);
    }
    SOL_CHECK(biggestOutput > biggestDraw);
}
