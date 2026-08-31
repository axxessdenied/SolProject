// Whose law you are standing in, and what it says about what you are carrying
// (engine plan Phase 33 stage D, gdd.md §13).
//
// ⚑⚑⚑⚑ THE PHASE SPEC SAID TO READ `SystemSpec::factionIndex` AND THAT FIELD IS
// THE FOUNDING CLAIM. It is written once by the galaxy generator and never
// moves; Phase 8u made ownership dynamic in `FactionSim`, and the galaxy hands
// systems back and forth several times a minute. So a legality table keyed on
// it would tell a player that a system the Hegemony took this morning still
// runs Compact law — and NO TEST THAT NEVER TRANSFERS A SYSTEM COULD SEE IT,
// because at t=0 the two fields agree exactly. That is why the third test below
// exists and why it is the one that matters: it is the only assertion in this
// file that can tell the two readings apart.
//
// ⚑⚑ AND THE SECOND HALF OF THE FEATURE IS AN ABSENCE, WHICH IS WHY IT GETS ITS
// OWN TEST TOO. "The faction holding this place lists nothing" and "nobody holds
// this place" produce identical verdicts for every good in the galaxy, and
// gdd.md §13 turns on the difference — a smuggler's whole route planning is the
// second answer. `Legality::Unpoliced` is the only thing that carries it.

#include "space_world.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::Legality;

namespace {

// Two majors with opposite opinions about one good, plus a third that has none.
// The station and ship rows are the minimum a world needs to generate.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
tier = "consumer"
base_price = 8.0

[[commodity]]
id = "sol.salvage"
name = "Salvage"
tier = "raw"
base_price = 11.0

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0

[[faction]]
id = "sol.hegemony"
name = "Ironstar Hegemony"
color = [0.75, 0.22, 0.28]
kind = "major"
contraband = ["sol.salvage"]

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"
restricted = ["sol.salvage"]

[[faction]]
id = "sol.compact"
name = "Frontier Compact"
color = [0.35, 0.8, 0.45]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5
produces = ["sol.food:0.26"]
consumes = ["sol.salvage:0.01"]
stock_capacity = 2500
)";

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    Fixture()
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        SOL_CHECK(defs.validateLegality(&error));
        world.spawn(1701);
        // ⚑⚑ `applyDefs` BEFORE `generateUniverse`, which is the order the game
        // boots in (`content.cpp`) and is load-bearing rather than tidy:
        // `generateUniverse` calls `initializeFactions`, which reads the def
        // database and returns immediately when there is none. Handing the defs
        // over afterwards leaves the faction table EMPTY and every system in
        // the galaxy unowned - which a legality test reads as "nobody polices
        // anywhere", the answer it is here to distinguish from.
        world.applyDefs(defs);
        SOL_CHECK(world.generateUniverse(defs));
    }

    // The first system this galaxy gave to the named faction, or kNoIndex.
    [[nodiscard]] std::uint32_t systemHeldBy(const char* factionName) const
    {
        for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
            const sol::assets::FactionDef* law = world.jurisdictionOf(i);
            if (law != nullptr && law->name == factionName) {
                return i;
            }
        }
        return 0xffff'ffffu;
    }
};

} // namespace

SOL_TEST(the_same_crate_is_contraband_in_one_faction_space_and_licensed_in_another)
{
    Fixture fixture;
    const std::uint32_t salvage = fixture.world.commodityIndex("sol.salvage");
    const std::uint32_t food = fixture.world.commodityIndex("sol.food");
    SOL_REQUIRE(salvage != 0xffff'ffffu && food != 0xffff'ffffu);

    const std::uint32_t hegemony = fixture.systemHeldBy("Ironstar Hegemony");
    const std::uint32_t navy = fixture.systemHeldBy("Solar Navy");
    const std::uint32_t compact = fixture.systemHeldBy("Frontier Compact");
    SOL_REQUIRE(hegemony != 0xffff'ffffu);
    SOL_REQUIRE(navy != 0xffff'ffffu);
    SOL_REQUIRE(compact != 0xffff'ffffu);

    // The phase's exit sentence, as three reads of one commodity index.
    SOL_CHECK(fixture.world.commodityLegality(hegemony, salvage) == Legality::Contraband);
    SOL_CHECK(fixture.world.commodityLegality(navy, salvage) == Legality::Restricted);
    SOL_CHECK(fixture.world.commodityLegality(compact, salvage) == Legality::Legal);

    // ⚑ And nobody has an opinion about groceries anywhere, which is what stops
    // this passing for a reason that has nothing to do with the table — a query
    // that answered `Contraband` for every good in Hegemony space would satisfy
    // the three lines above perfectly.
    SOL_CHECK(fixture.world.commodityLegality(hegemony, food) == Legality::Legal);
    SOL_CHECK(fixture.world.commodityLegality(navy, food) == Legality::Legal);
}

// ⚑⚑⚑ A JURISDICTION WITH NO TABLE AND NO JURISDICTION AT ALL ARE DIFFERENT
// ANSWERS TO THE SAME QUESTION, and the Frontier Compact system above already
// proves the first. This proves the second is distinguishable from it.
SOL_TEST(a_system_nobody_holds_answers_unpoliced_rather_than_legal)
{
    Fixture fixture;
    const std::uint32_t salvage = fixture.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);

    // ⚑ A REAL UNOWNED SYSTEM, NOT AN OUT-OF-RANGE INDEX. These defs carry no
    // pirate template, so `spawnClans` has nothing to hand a lawless
    // neighbourhood to and it stays at `kNoFaction` - which is exactly the
    // shape the shipped galaxy has at `sol.lantern`, the one authored
    // `lawless = true` system and the only dock in the whole game where this
    // answer comes back. Asserting it against a bounds check instead would
    // prove the guard clause and nothing about the galaxy.
    std::uint32_t unowned = 0xffff'ffffu;
    for (std::uint32_t i = 0; i < fixture.world.galaxy().systems.size(); ++i) {
        if (fixture.world.jurisdictionOf(i) == nullptr) {
            unowned = i;
            break;
        }
    }
    SOL_REQUIRE(unowned != 0xffff'ffffu);
    SOL_CHECK(fixture.world.commodityLegality(unowned, salvage) == Legality::Unpoliced);
    // Every good, not one: an unpoliced system has no table at all rather than
    // an empty one, so nothing it is asked about can come back any other way.
    SOL_CHECK(fixture.world.commodityLegality(unowned, fixture.world.commodityIndex("sol.food")) ==
              Legality::Unpoliced);

    // The distinction the enum exists for, stated as an assertion rather than
    // as a comment: the Compact holds space and objects to nothing, and that
    // must NOT read the same as nobody holding it.
    const std::uint32_t compact = fixture.systemHeldBy("Frontier Compact");
    SOL_REQUIRE(compact != 0xffff'ffffu);
    SOL_CHECK(fixture.world.commodityLegality(compact, salvage) != Legality::Unpoliced);
}

// ⚑⚑⚑⚑ THE ONE THAT WOULD HAVE CAUGHT THE SPEC. Take a system off its founding
// owner and the law standing on it has to change with it — which is true of a
// query reading `systemOwnerFaction` and false of one reading
// `SystemSpec::factionIndex`, and the two are indistinguishable until the
// moment a border moves.
SOL_TEST(the_law_follows_the_flag_when_a_system_changes_hands)
{
    Fixture fixture;
    const std::uint32_t salvage = fixture.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);

    const std::uint32_t system = fixture.systemHeldBy("Frontier Compact");
    SOL_REQUIRE(system != 0xffff'ffffu);
    SOL_REQUIRE(fixture.world.commodityLegality(system, salvage) == Legality::Legal);
    // The founding claim is what the generator wrote and it does not move.
    const std::uint32_t founding = fixture.world.galaxy().systems[system].factionIndex;

    // Whoever the Hegemony is in sim order, taken from a system it already
    // holds rather than from a hard-coded index — the faction table is majors
    // then clans and this test has no business knowing that.
    const std::uint32_t hegemonySystem = fixture.systemHeldBy("Ironstar Hegemony");
    SOL_REQUIRE(hegemonySystem != 0xffff'ffffu);
    const std::uint32_t hegemony = fixture.world.systemOwnerFaction(hegemonySystem);
    SOL_REQUIRE(fixture.world.factionSim().flipSystem(system, hegemony));

    // Not one byte of the generated spec changed, and the answer did.
    SOL_CHECK(fixture.world.galaxy().systems[system].factionIndex == founding);
    SOL_CHECK(fixture.world.commodityLegality(system, salvage) == Legality::Contraband);
}

// ⚑⚑⚑⚑ AND THE SAME SENTENCE AGAINST THE GAME'S OWN COMMITTED DATA, WHICH IS
// THE PHASE'S EXIT AND NOT A UNIT TEST. Everything above proves the mechanism
// on defs written to exercise it; this proves that somebody actually AUTHORED
// a jurisdiction that objects to something, in the galaxy that ships. The two
// fail for completely different reasons - one for a broken query, one for an
// empty `factions.toml` - and a stage that shipped the first without the second
// would be a feature no player could ever meet.
//
// ⚑⚑ IT ALSO ASSERTS THE THING THE SHIPPED GALAXY IS UNIQUELY ABLE TO SAY:
// `sol.lantern` is the one authored `lawless = true` system, and `spawnClans`
// hands every other unclaimed neighbourhood to a pirate clan - which IS a
// jurisdiction, one with no table. So there is exactly one dock in the whole
// game where `Unpoliced` comes back, and if that ever stops being true this is
// where it will be noticed.
SOL_TEST(the_shipped_galaxy_has_somewhere_salvage_is_a_crime_and_one_place_nobody_asks)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(defs.validateLegality(&error));

    game::SpaceWorld world;
    world.spawn(1701);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    const std::uint32_t salvage = world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);

    std::uint32_t contraband = 0;
    std::uint32_t restricted = 0;
    std::uint32_t legal = 0;
    std::uint32_t unpoliced = 0;
    std::uint32_t lantern = 0xffff'ffffu;
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        switch (world.commodityLegality(i, salvage)) {
        case Legality::Contraband:
            ++contraband;
            break;
        case Legality::Restricted:
            ++restricted;
            break;
        case Legality::Legal:
            ++legal;
            break;
        case Legality::Unpoliced:
            ++unpoliced;
            break;
        }
        if (world.galaxy().systems[i].authoredId == "sol.lantern") {
            lantern = i;
        }
    }
    if (contraband == 0 || restricted == 0 || legal == 0 || unpoliced != 1) {
        std::printf("  shipped salvage law: %u contraband, %u restricted, %u legal, %u unpoliced\n",
                    contraband,
                    restricted,
                    legal,
                    unpoliced);
    }
    // All four answers exist in the galaxy that ships, which is the exit.
    SOL_CHECK(contraband > 0);
    SOL_CHECK(restricted > 0);
    SOL_CHECK(legal > 0);
    // ⚑ Exactly one, not "at least one". `spawnClans` sweeping up every other
    // lawless neighbourhood is what makes The Slow Lantern the singular place
    // it is, and a second unpoliced system would mean that sweep had stopped
    // working - a change nothing else in the suite would report.
    SOL_CHECK(unpoliced == 1);
    SOL_REQUIRE(lantern != 0xffff'ffffu);
    SOL_CHECK(world.commodityLegality(lantern, salvage) == Legality::Unpoliced);
    SOL_CHECK(world.jurisdictionOf(lantern) == nullptr);

    // And a hull plate is a different question from a salvaged one, so the
    // table is a table rather than one commodity's special case.
    const std::uint32_t plate = world.commodityIndex("sol.hull_plate");
    SOL_REQUIRE(plate != 0xffff'ffffu);
    bool plateRestrictedSomewhere = false;
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        plateRestrictedSomewhere =
            plateRestrictedSomewhere || world.commodityLegality(i, plate) == Legality::Restricted;
    }
    SOL_CHECK(plateRestrictedSomewhere);
}

// ⚑⚑⚑ THE NAME ON THE DOCK IS THE HOLDER'S, AND THE TABLE IS ITS TEMPLATE'S -
// TWO DIFFERENT FACTIONS FOR A THIRD OF THE GALAXY (found by the Phase 33 exit
// flight). `jurisdictionOf` returns the def, which for a generated clan is the
// pirate TEMPLATE it was rolled from, and the trade panel printed that def's
// name: a dock held by `Queunth Corsairs` announced itself as being under
// `Reaver Kindred` law, a faction that appears under that name nowhere else in
// the game - not in `sol.territory`, not in the raid log, not on the map.
//
// ⚑⚑ AND THE NAME CANNOT BE READ AS A HINT AT THE TEMPLATE, BECAUSE THE TWO
// DRAWS ARE INDEPENDENT. `spawnClans` names a clan `<system> <random suffix>`
// and picks its template with a separate `rng.range` - and the suffix list
// contains BOTH template names, so a clan called "... Corsairs" being governed
// by the Reaver Kindred table is ordinary rather than a mix-up.
//
// The law lookup is unchanged and deliberately so: ten Reaver clans consult one
// Reaver Kindred table, which `jurisdictionOf` says of itself. Only the words
// moved.
SOL_TEST(a_clan_dock_names_the_clan_that_holds_it_and_not_the_template_it_rolled)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    game::SpaceWorld world;
    world.spawn(1701);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    std::uint32_t clanSystems = 0;
    std::uint32_t namedAfterTemplate = 0;
    const char* firstClan = nullptr;
    const char* firstTemplate = nullptr;
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        const sol::assets::FactionDef* law = world.jurisdictionOf(i);
        if (law == nullptr || law->kind != sol::assets::FactionKind::Pirate) {
            continue;
        }
        ++clanSystems;
        const char* shown = world.jurisdictionName(i);
        SOL_REQUIRE(shown != nullptr);
        // The holder is a generated clan, so the panel must not be showing the
        // template's name. `Corsairs` / `Reaver Kindred` are the two templates.
        if (law->name == shown) {
            ++namedAfterTemplate;
        }
        if (firstClan == nullptr) {
            firstClan = shown;
            firstTemplate = law->name.c_str();
        }
    }
    if (clanSystems == 0 || namedAfterTemplate != 0) {
        std::printf("  %u clan system(s), %u still showing the template name\n",
                    clanSystems,
                    namedAfterTemplate);
    }
    // Clans hold a large minority of the shipped galaxy; if that ever stops
    // being true this test is measuring nothing and should say so.
    SOL_REQUIRE(clanSystems > 10);
    SOL_CHECK(namedAfterTemplate == 0);
    if (firstClan != nullptr) {
        std::printf("  a clan dock reads '%s' where the table is '%s'\n", firstClan, firstTemplate);
    }

    // ⚑ And a MAJOR is untouched: for a faction that is its own def, the holder
    // and the table are the same string, so nothing about Navy or Hegemony
    // space moved. This is the half that would break if `jurisdictionName` had
    // been keyed on something other than the live faction table.
    std::uint32_t majorSystems = 0;
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        const sol::assets::FactionDef* law = world.jurisdictionOf(i);
        if (law == nullptr || law->kind == sol::assets::FactionKind::Pirate) {
            continue;
        }
        ++majorSystems;
        SOL_CHECK(law->name == world.jurisdictionName(i));
    }
    SOL_REQUIRE(majorSystems > 10);
}
