// The cast (engine plan Phase 35 stage C): who is in each room, which half of
// them somebody wrote, and what the save remembers about them.
//
// ⚑⚑⚑⚑ THIS FILE CARRIES THE ONE CLAIM `validateCharacters` CANNOT MAKE, AND IT
// IS THE CLAIM THE STAGE IS MOST LIKELY TO BREAK. The load-time check refuses an
// anchor that names a faction or a module that does not exist - a question about
// the DEFS. Whether an anchor that resolves perfectly finds a FREE SEAT is a
// question about a GALAXY, and it can only be asked where one is in hand. A
// character starved of a seat by somebody written above them is not an error
// anywhere: they are simply not in the world, which is the exact failure mode
// `characters.toml`'s own header warns about and which nothing else in the tree
// would notice.
//
// ⚑⚑ AND THE SECOND CLAIM IS THE ENGINE HALF: every one of the galaxy's rooms
// has somebody in it. Six authored people across 62 rooms would have left 56 of
// them with the anonymous house voice, and the phase's whole open question -
// whether a bar reads as a room or as a feed - would have stayed unanswered for
// nine rooms out of ten.

#include "content.hpp"
#include "space_world.hpp"
#include "station_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/sim/pilot_tips.hpp>
#include <sol/test/test.hpp>

using game::SpaceWorld;
using sol::assets::CharacterDef;
using sol::assets::DefDatabase;
using sol::assets::ModuleFamily;
using sol::sim::CastCandidate;

namespace {

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] bool buildShippedGalaxy(const DefDatabase& defs, SpaceWorld& world)
{
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    if (!world.generateUniverse(defs)) {
        std::printf("  generateUniverse refused the shipped defs\n");
        return false;
    }
    return true;
}

std::string scratchPath(const char* leaf)
{
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/cast";
    SOL_CHECK(sol::platform::createDirectories(dir.c_str()));
    return dir + "/" + leaf;
}

} // namespace

// ⚑⚑⚑ THE ENGINE HALF, AND THE FLOOR IS THE POINT. A placement pass that
// silently stopped running would leave every room with an empty seat, and the
// only thing in the game that would say so is the bar tab's heading - which
// nobody reads in a test. Both directions are checked, because seating somebody
// where there is no room is the same bug from the other side and would put a
// name on a station with no bar tab to draw it in.
SOL_TEST(every_room_in_the_galaxy_has_somebody_in_it_and_nowhere_else_does)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    int rooms = 0;
    int seated = 0;
    int seatedWithoutARoom = 0;
    int roomsWithNobody = 0;
    int nameless = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const bool hasRoom = game::stationRoom(world, defs, s, t) != nullptr;
            const SpaceWorld::CastSeat* seat = world.stationCast(s, t);
            rooms += hasRoom ? 1 : 0;
            seated += seat != nullptr ? 1 : 0;
            seatedWithoutARoom += (seat != nullptr && !hasRoom) ? 1 : 0;
            roomsWithNobody += (seat == nullptr && hasRoom) ? 1 : 0;
            if (seat != nullptr && (seat->name.empty() || seat->trade.empty())) {
                ++nameless;
            }
        }
    }
    std::printf("  %d room(s), %d seat(s) filled; %d seated with no room, %d rooms with nobody\n",
                rooms,
                seated,
                seatedWithoutARoom,
                roomsWithNobody);
    SOL_REQUIRE(rooms > 0); // and the whole file is not measuring an empty galaxy
    SOL_CHECK(seated == rooms);
    SOL_CHECK(seatedWithoutARoom == 0);
    SOL_CHECK(roomsWithNobody == 0);
    // Everybody has both halves of an identity. A seat with a name and no trade
    // would draw as a bare name and read as a bug rather than as a person.
    SOL_CHECK(nameless == 0);
}

// ⚑⚑⚑⚑ THE STARVATION GUARD, AND IT IS THIS FILE'S REASON FOR EXISTING.
// `characters.toml` seats people in FIRST-DEFINITION ORDER and each takes a free
// seat its anchors allow, so a tight anchor written after a loose one can find
// its only seat already taken - the resort has exactly ONE seat in the whole
// galaxy. Nothing refuses that: the character is simply not in the world, which
// is indistinguishable from a character nobody wrote, and no other test in this
// tree would notice. Failing BY NAME is the whole value here.
SOL_TEST(every_authored_character_found_a_seat_in_the_shipped_galaxy)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    // The file exists and has people in it. Without this the checks below pass
    // vacuously the day `characters.toml` is renamed or emptied.
    std::printf("  %zu authored character(s) in the shipped defs\n", defs.characters().size());
    SOL_REQUIRE(!defs.characters().empty());

    std::vector<int> seats(defs.characters().size(), 0);
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const SpaceWorld::CastSeat* seat = world.stationCast(s, t);
            if (seat == nullptr || seat->character == SpaceWorld::kNoCharacter) {
                continue;
            }
            SOL_REQUIRE(seat->character < seats.size());
            seats[seat->character] += 1;
            // The name and the trade come from the def rather than from the
            // generator, which is what makes them content an author can edit.
            SOL_CHECK(seat->name == defs.characters()[seat->character].name);
            SOL_CHECK(seat->trade == defs.characters()[seat->character].trade);
        }
    }
    for (std::size_t i = 0; i < seats.size(); ++i) {
        if (seats[i] != 1) {
            std::printf("  '%s' is seated %d time(s) in the shipped galaxy\n",
                        defs.characters()[i].id.c_str(),
                        seats[i]);
        }
        SOL_CHECK(seats[i] == 1);
    }
}

// ⚑⚑ EVERY ANCHOR THE SHIPPED CAST USES IS HONOURED, CHECKED AGAINST THE GALAXY
// RATHER THAN AGAINST THE RULE. `castSeatSuits` is checked directly below; this
// asks the different question of whether the pass actually calls it with the
// right seat, which a rule test cannot reach.
SOL_TEST(an_anchored_character_sits_where_their_anchor_says)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t clanBase =
        static_cast<std::uint32_t>(world.factions().size() - world.galaxy().clans.size());
    int checked = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const SpaceWorld::CastSeat* seat = world.stationCast(s, t);
            if (seat == nullptr || seat->character == SpaceWorld::kNoCharacter) {
                continue;
            }
            const CharacterDef& who = defs.characters()[seat->character];
            ++checked;
            if (!who.factionId.empty()) {
                // ⚑⚑ THE *FOUNDING* CLAIM, WHICH IS THE OPPOSITE OF WHAT STAGE E
                // ASKS OF THE SAME FIELD, AND THE TEST SAYS SO RATHER THAN
                // LEAVING IT TO LOOK LIKE AN OVERSIGHT. A person does not move
                // when a border does. `systemOwnerFaction` would pass here at
                // t=0 too - that is the one moment the two agree - which is
                // exactly why the assertion names the field it means.
                const sol::assets::FactionDef* faction = defs.findFaction(who.factionId.c_str());
                SOL_REQUIRE(faction != nullptr);
                std::uint32_t index = 0;
                for (std::uint32_t f = 0; f < defs.factions().size(); ++f) {
                    if (defs.factions()[f].id == who.factionId) {
                        index = f;
                    }
                }
                SOL_CHECK(system.factionIndex == index);
            }
            if (who.lawless) {
                SOL_CHECK(system.factionIndex != sol::sim::kNoFaction);
                SOL_CHECK(system.factionIndex >= clanBase);
            }
            if (!who.region.empty()) {
                const char* names[] = {"core", "frontier", "fringe"};
                SOL_CHECK(who.region == names[static_cast<std::size_t>(system.region)]);
            }
            if (!who.moduleId.empty()) {
                const sol::assets::ModuleDef* room = game::stationRoom(world, defs, s, t);
                SOL_REQUIRE(room != nullptr);
                SOL_CHECK(room->id == who.moduleId);
            }
        }
    }
    std::printf("  %d anchored seat(s) checked against the galaxy\n", checked);
    SOL_REQUIRE(checked > 0);
}

// ⚑⚑⚑ THE RULE ITSELF, HANDED THE CASES THE SHIPPED GALAXY DECLINES TO PRODUCE -
// the reason `shadowOperatorFor` and `stationTabOnStrip` are public functions
// rather than four lines inside their loops. NO character in `characters.toml`
// uses `archetype` or `shadow`, so the galaxy-level test above proves nothing
// about either: a rule that ignored both entirely would leave this whole suite
// green. Stage E paid for that lesson with a mutation that did not fail.
SOL_TEST(the_seating_rule_honours_anchors_the_shipped_cast_never_uses)
{
    constexpr std::uint32_t kClanBase = 5;
    SpaceWorld::CastSeatFacts seat;
    seat.founder = 2; // a major
    seat.archetype = 7;
    seat.room = 11;
    seat.region = static_cast<std::uint32_t>(sol::sim::Region::Frontier);
    seat.hasShadow = false;

    // No anchors at all: anywhere will do, which is the cheapest row an author
    // can write and has to keep working.
    SOL_CHECK(SpaceWorld::castSeatSuits({}, seat, kClanBase));

    // Archetype, both ways.
    SpaceWorld::CastEntry entry;
    entry.archetype = 7;
    SOL_CHECK(SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    entry.archetype = 8;
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase));

    // The room module, both ways.
    entry = {};
    entry.room = 11;
    SOL_CHECK(SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    entry.room = 12;
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase));

    // ⚑ The fence anchor, which selects TWO rooms in the whole shipped galaxy
    // and is therefore the anchor a galaxy test is least able to speak for.
    entry = {};
    entry.shadow = true;
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    seat.hasShadow = true;
    SOL_CHECK(SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    seat.hasShadow = false;

    // Region, both ways.
    entry = {};
    entry.region = static_cast<std::uint32_t>(sol::sim::Region::Frontier);
    SOL_CHECK(SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    entry.region = static_cast<std::uint32_t>(sol::sim::Region::Core);
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase));

    // Faction, and `lawless` as its complement: a major's space is not lawless
    // and a clan's is, with the ownerless case refused by both.
    entry = {};
    entry.faction = 2;
    SOL_CHECK(SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    entry.faction = 3;
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase));

    entry = {};
    entry.lawless = true;
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase)); // founder 2 is a major
    seat.founder = kClanBase + 1;
    SOL_CHECK(SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    seat.founder = sol::sim::kNoFaction;
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase)); // nobody founded it: not a clan's

    // ⚑ ANCHORS ARE ANDed, WHICH THE SHIPPED CAST ALSO NEVER EXERCISES: no row
    // in `characters.toml` writes two of them, so nothing else here would catch
    // a rule that returned on the first match.
    seat.founder = kClanBase + 1;
    entry = {};
    entry.lawless = true;
    entry.region = static_cast<std::uint32_t>(sol::sim::Region::Core);
    SOL_CHECK(!SpaceWorld::castSeatSuits(entry, seat, kClanBase));
    entry.region = static_cast<std::uint32_t>(sol::sim::Region::Frontier);
    SOL_CHECK(SpaceWorld::castSeatSuits(entry, seat, kClanBase));
}

// ⚑⚑ A REGULAR'S TRADE IS READ OFF THE STATION, NOT ROLLED, and that is what
// makes the generated half content rather than filler. The claim is checked in
// the direction that can actually fail: a Fixer is somebody who works a back
// room, so every Fixer in the galaxy must be standing on a dock that has one.
SOL_TEST(a_regular_does_the_job_the_station_they_are_standing_on_has)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::map<std::string, int> trades;
    int fixers = 0;
    int fixersWithNoBackRoom = 0;
    int yardHands = 0;
    int yardHandsWithNoBerths = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const SpaceWorld::CastSeat* seat = world.stationCast(s, t);
            if (seat == nullptr || seat->character != SpaceWorld::kNoCharacter) {
                continue; // an authored trade is content, not a derivation
            }
            trades[seat->trade] += 1;
            bool fence = false;
            for (const std::uint32_t module : world.stationModules(s, t)) {
                if (module < defs.modules().size() && defs.modules()[module].family == ModuleFamily::Shadow) {
                    fence = true;
                }
            }
            const bool berths =
                (world.stationScreens(s, t) &
                 (1u << static_cast<std::uint32_t>(sol::assets::StationScreen::Shipyard))) != 0;
            if (seat->trade == "Fixer") {
                ++fixers;
                fixersWithNoBackRoom += fence ? 0 : 1;
            }
            if (seat->trade == "Yard hand") {
                ++yardHands;
                yardHandsWithNoBerths += berths ? 0 : 1;
            }
        }
    }
    for (const auto& [trade, count] : trades) {
        std::printf("  %-14s %d\n", trade.c_str(), count);
    }
    // ⚑ MORE THAN ONE TRADE IN THE GALAXY. A derivation that collapsed to its
    // fallback would put "Hauler" behind every bar and would still pass every
    // other check in this file.
    SOL_REQUIRE(trades.size() > 1);
    SOL_REQUIRE(fixers > 0); // and the two claims below are not vacuous
    SOL_REQUIRE(yardHands > 0);
    SOL_CHECK(fixersWithNoBackRoom == 0);
    SOL_CHECK(yardHandsWithNoBerths == 0);
}

// ⚑⚑ THE SEATING IS DERIVED FROM THE SEED, WHICH IS WHY IT IS NOT IN THE SAVE.
// Two galaxies at one seed seat the same people, and re-composing an existing
// one does not shuffle anybody - the second is what `loadFrom` relies on, since
// it re-runs `composeStations` against a regenerated galaxy and expects to find
// the same person behind the same bar.
SOL_TEST(the_same_seed_seats_the_same_people)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld a;
    SOL_REQUIRE(buildShippedGalaxy(defs, a));
    SpaceWorld b;
    SOL_REQUIRE(buildShippedGalaxy(defs, b));

    int compared = 0;
    int drift = 0;
    for (std::uint32_t s = 0; s < a.galaxy().systems.size(); ++s) {
        for (std::uint32_t t = 0; t < a.galaxy().systems[s].stations.size(); ++t) {
            const SpaceWorld::CastSeat* left = a.stationCast(s, t);
            const SpaceWorld::CastSeat* right = b.stationCast(s, t);
            if ((left == nullptr) != (right == nullptr)) {
                ++drift;
                continue;
            }
            if (left == nullptr) {
                continue;
            }
            ++compared;
            if (left->name != right->name || left->trade != right->trade ||
                left->character != right->character) {
                ++drift;
            }
        }
    }
    std::printf("  %d seat(s) compared across two galaxies at seed %llu, %d differ\n",
                compared,
                static_cast<unsigned long long>(game::kDefaultUniverseSeed),
                drift);
    SOL_REQUIRE(compared > 0);
    SOL_CHECK(drift == 0);
}

// ⚑⚑⚑ A UNIQUE IS A PERSON AND A REGULAR IS A CHAIR, WHICH IS THE ONE DESIGN
// DECISION IN THE SAVE FORMAT AND IS NOT OBVIOUS FROM READING IT. An authored
// character is keyed by a hash of their id, so a later build whose recipes move
// them to another dock finds them still remembering the player. A regular is
// keyed by the SEAT, because there is nobody to follow: a generated name that
// changed under a save would be a different person wearing somebody's memory.
SOL_TEST(a_character_is_keyed_by_who_they_are_and_a_regular_by_where_they_sit)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    // The two key spaces cannot collide: a seat key always has the top bit set
    // and a character's hash never does.
    SOL_CHECK((SpaceWorld::castKeyForSeat(3, 1) & SpaceWorld::kSeatKey) != 0);
    SOL_CHECK((SpaceWorld::castKeyForCharacter("sol.char_bekker") & SpaceWorld::kSeatKey) == 0);
    // Different people, different keys; different seats, different keys.
    SOL_CHECK(SpaceWorld::castKeyForCharacter("sol.char_bekker") !=
              SpaceWorld::castKeyForCharacter("sol.char_soto"));
    SOL_CHECK(SpaceWorld::castKeyForSeat(3, 1) != SpaceWorld::castKeyForSeat(1, 3));

    int authored = 0;
    int regulars = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        for (std::uint32_t t = 0; t < world.galaxy().systems[s].stations.size(); ++t) {
            const SpaceWorld::CastSeat* seat = world.stationCast(s, t);
            if (seat == nullptr) {
                SOL_CHECK(world.castKeyAt(s, t) == 0);
                continue;
            }
            const std::uint64_t key = world.castKeyAt(s, t);
            SOL_CHECK(key != 0);
            if (seat->character == SpaceWorld::kNoCharacter) {
                ++regulars;
                SOL_CHECK(key == SpaceWorld::castKeyForSeat(s, t));
            } else {
                ++authored;
                SOL_CHECK(key ==
                          SpaceWorld::castKeyForCharacter(defs.characters()[seat->character].id.c_str()));
            }
        }
    }
    std::printf("  %d authored key(s), %d seat key(s)\n", authored, regulars);
    SOL_REQUIRE(authored > 0);
    SOL_REQUIRE(regulars > 0);
}

// ⚑⚑ WHAT THE PLAYER DID SURVIVES A SAVE, AND ONLY WHAT THE PLAYER DID. The
// seating chart is derived and is deliberately absent from the file; the memory
// is sparse, so a galaxy of 62 rooms costs nothing until somebody walks into
// one. `kSaveVersion` moved 32 -> 33 for this section and for nothing else.
SOL_TEST(a_save_remembers_who_you_have_talked_to_and_not_who_sits_where)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    // Nobody has been met, so the table is empty - the sparseness that keeps
    // this section four bytes on a fresh save.
    SOL_CHECK(world.castMemories().empty());

    std::uint64_t person = 0;
    std::uint64_t chair = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && (person == 0 || chair == 0); ++s) {
        for (std::uint32_t t = 0; t < world.galaxy().systems[s].stations.size(); ++t) {
            const SpaceWorld::CastSeat* seat = world.stationCast(s, t);
            if (seat == nullptr) {
                continue;
            }
            if (seat->character == SpaceWorld::kNoCharacter) {
                if (chair == 0) {
                    chair = world.castKeyAt(s, t);
                }
            } else if (person == 0) {
                person = world.castKeyAt(s, t);
            }
        }
    }
    SOL_REQUIRE(person != 0);
    SOL_REQUIRE(chair != 0);

    world.noteCastVisit(person);
    world.noteCastVisit(person);
    world.noteCastVisit(chair);
    world.adjustCastRegard(person, 3);
    SOL_REQUIRE(world.castMemory(person) != nullptr);
    SOL_CHECK(world.castMemory(person)->visits == 2);
    SOL_CHECK(world.castMemory(person)->regard == 3);
    SOL_REQUIRE(world.castMemory(chair) != nullptr);
    SOL_CHECK(world.castMemory(chair)->visits == 1);
    SOL_CHECK(world.castMemory(chair)->regard == 0);
    SOL_CHECK(world.castMemories().size() == 2); // and nobody else got a row

    const std::string path = scratchPath("cast.sav");
    (void)sol::platform::deleteFile(path.c_str());
    SOL_REQUIRE(world.saveTo(path.c_str(), "Met a few people"));

    SpaceWorld restored;
    SOL_REQUIRE(buildShippedGalaxy(defs, restored));
    SOL_CHECK(restored.castMemories().empty()); // a fresh run has met nobody
    SOL_REQUIRE(restored.loadFrom(path.c_str()));
    SOL_REQUIRE(restored.castMemory(person) != nullptr);
    SOL_CHECK(restored.castMemory(person)->visits == 2);
    SOL_CHECK(restored.castMemory(person)->regard == 3);
    SOL_REQUIRE(restored.castMemory(chair) != nullptr);
    SOL_CHECK(restored.castMemory(chair)->visits == 1);
    SOL_CHECK(restored.castMemories().size() == 2);
}

// ⚑⚑⚑⚑ THE STAGE'S EXIT CRITERION, END TO END, THROUGH THE REAL HOOK: *a named
// character is still there, and still remembers you, on a later visit.* Every
// other test in this file reaches `noteCastVisit` by calling it, which proves
// the table and proves nothing about the WIRING - and the wiring is the half
// that can silently not happen, because the visit is counted in
// `GameContent::tick` and the heading is worded in a fill that runs sixty times
// a second. Deleting the one call would leave every check above green and every
// barkeep in the game a permanent stranger.
SOL_TEST(a_barkeep_you_have_met_before_says_so_when_you_walk_back_in)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed); // registers the component storages
    game::GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    // The first room in the galaxy, whichever it is: the claim is about any
    // room, so naming one would be a claim about the seed instead.
    std::uint32_t roomSystem = 0xffff'ffffu;
    std::uint32_t roomStation = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && roomSystem == 0xffff'ffffu; ++s) {
        for (std::uint32_t t = 0; t < world.galaxy().systems[s].stations.size(); ++t) {
            if (world.stationCast(s, t) != nullptr) {
                roomSystem = s;
                roomStation = t;
                break;
            }
        }
    }
    SOL_REQUIRE(roomSystem != 0xffff'ffffu);

    const auto walkIn = [&]() {
        SOL_REQUIRE(world.enterSystem(roomSystem));
        SOL_REQUIRE(world.warpToStationOffset(roomStation, {100.0, 0.0, 0.0}));
        SOL_REQUIRE(world.tryDockNearestStation(1000.0));
        content.tick(1.0 / 30.0);
    };

    walkIn();
    const std::string first = content.barKeeper();
    std::printf("  first visit:  %s\n", first.c_str());
    SOL_REQUIRE(!first.empty());
    // Somebody is named, and the room knows this is the first time.
    SOL_CHECK(first.find("(you have met)") != std::string::npos);
    SOL_CHECK(first.find("familiar") == std::string::npos);

    SOL_REQUIRE(world.undock());
    content.tick(1.0 / 30.0);
    SOL_CHECK(content.barKeeper()[0] == 0); // walking out clears the room

    walkIn();
    const std::string second = content.barKeeper();
    std::printf("  second visit: %s\n", second.c_str());
    // ⚑ The SAME PERSON - the seating is derived from the seed and does not
    // move - saying something different, which is the whole of what v33 bought.
    SOL_REQUIRE(!second.empty());
    SOL_CHECK(second.substr(0, second.find(" (")) == first.substr(0, first.find(" (")));
    SOL_CHECK(second.find("(a familiar face)") != std::string::npos);
    SOL_CHECK(second != first);

    // And the save agrees with the screen.
    const std::uint64_t who = world.castKeyAt(roomSystem, roomStation);
    SOL_REQUIRE(world.castMemory(who) != nullptr);
    SOL_CHECK(world.castMemory(who)->visits == 2);
}

// ⚑⚑ A MOD THAT ADDS A BARKEEP DOES NOT REFUSE EVERY SAVE ON THE DISK, and this
// is here because it is a DECISION that looks exactly like an oversight. A
// `[[character]]` row is authored content, and `m_authoredDigest` covers
// authored content - but what that digest actually guards is INDEX STABILITY: a
// save whose system index would now point somewhere else. A cast row moves no
// index. The cost is one dock's stale visit count (see `kSaveVersion`'s note),
// against refusing every existing save, and it is not close. If a later stage
// folds the cast into the digest it has to move this test with it.
SOL_TEST(adding_a_character_does_not_invalidate_a_save)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld before;
    SOL_REQUIRE(buildShippedGalaxy(defs, before));
    const std::uint64_t was = before.authoredContentDigest();

    const char* newcomer = R"(
[[character]]
id = "sol.char_test_only"
name = "Somebody Else"
trade = "Regular"
)";
    std::string error;
    SOL_REQUIRE(defs.mergeToml(newcomer, std::strlen(newcomer), "extra.toml", &error));
    SpaceWorld after;
    SOL_REQUIRE(buildShippedGalaxy(defs, after));

    // The cast really did grow - without this the digest check below is a
    // sentence about a merge that did nothing.
    SOL_REQUIRE(after.stationCast(0, 0) != nullptr);
    int seated = 0;
    for (std::uint32_t s = 0; s < after.galaxy().systems.size(); ++s) {
        for (std::uint32_t t = 0; t < after.galaxy().systems[s].stations.size(); ++t) {
            const SpaceWorld::CastSeat* seat = after.stationCast(s, t);
            seated += (seat != nullptr && seat->name == "Somebody Else") ? 1 : 0;
        }
    }
    std::printf("  the newcomer took %d seat(s); authored digest 0x%016llX -> 0x%016llX\n",
                seated,
                static_cast<unsigned long long>(was),
                static_cast<unsigned long long>(after.authoredContentDigest()));
    SOL_CHECK(seated == 1);
    SOL_CHECK(after.authoredContentDigest() == was);
}

// ⚑⚑⚑⚑ THE STAGE'S HEADLINE MEASUREMENT, AND IT IS THE ONE THAT CAUGHT THE
// FIRST VERSION OF THE RULE. A source is only worth a slot in `bar_talk`'s
// scarcity order if it is actually scarce, and "somebody to go and see" was
// written to include generated regulars first - which made it sayable at 62
// rooms of 62 and, measured, still 61 of 62 after the player had met FORTY
// people. It never went quiet, so by the order's own rule it would have had to
// be spent last, which is the same as never. Restricted to the authored cast it
// is a real curve. The bands are asserted rather than the counts, for the reason
// `station_bar_tests.cpp` gives: a count sheet goes stale one commit later.
SOL_TEST(the_cast_line_is_scarce_and_goes_quiet_once_you_have_met_everybody)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::vector<CastCandidate> people;
    std::vector<std::uint64_t> everybody;
    int rooms = 0;
    int canPoint = 0;
    int inReach = 0;
    std::uint32_t pick = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        for (std::uint32_t t = 0; t < world.galaxy().systems[s].stations.size(); ++t) {
            if (world.stationCast(s, t) == nullptr) {
                continue;
            }
            ++rooms;
            everybody.push_back(world.castKeyAt(s, t));
            world.castCandidates(s, people);
            inReach += static_cast<int>(people.size());
            if (sol::sim::chooseCastTalk(people, &pick)) {
                ++canPoint;
                // Never this system, and never a regular - both are the rule's,
                // and both are asserted here against real galaxy data rather
                // than against the synthetic span above.
                SOL_CHECK(people[pick].jumps > 0);
                SOL_CHECK(people[pick].authored);
            }
        }
    }
    std::printf("  %d room(s) can name somebody worth going to see, of %d; %.1f people in reach on average\n",
                canPoint,
                rooms,
                rooms > 0 ? static_cast<double>(inReach) / rooms : 0.0);
    SOL_REQUIRE(rooms > 0);
    // It says something in a good half of the galaxy's rooms...
    SOL_CHECK(canPoint > rooms / 3);
    // ...and NOT in all of them, which is the whole of what earns it its slot.
    SOL_CHECK(canPoint < rooms);

    // Now meet everybody. Tier two still speaks - a room that has run out of
    // strangers talks about somebody you know rather than going silent - but
    // nothing it names is a stranger any more.
    for (const std::uint64_t who : everybody) {
        world.noteCastVisit(who);
    }
    int strangers = 0;
    int stillPoints = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        for (std::uint32_t t = 0; t < world.galaxy().systems[s].stations.size(); ++t) {
            if (world.stationCast(s, t) == nullptr) {
                continue;
            }
            world.castCandidates(s, people);
            if (sol::sim::chooseCastTalk(people, &pick)) {
                ++stillPoints;
                strangers += people[pick].visits == 0 ? 1 : 0;
            }
        }
    }
    std::printf("  after meeting everybody: %d room(s) still point, %d of them at a stranger\n",
                stillPoints,
                strangers);
    SOL_CHECK(strangers == 0);
    SOL_CHECK(stillPoints > 0); // and it did not become a refusal
}

// ⚑ THE HEADING'S THREE STATES, WHICH IS WHERE THE SAVE BECOMES VISIBLE. A
// counter would have been easier and is exactly what a person would never say:
// "your fourth visit" is a number a save file has and a barkeep does not.
SOL_TEST(the_room_says_whether_it_knows_you_without_counting)
{
    SOL_CHECK(game::composeKeeperLine("Ines Farrow", "Hauler", 0) == "Ines Farrow - Hauler");
    SOL_CHECK(game::composeKeeperLine("Ines Farrow", "Hauler", 1) == "Ines Farrow - Hauler (you have met)");
    SOL_CHECK(game::composeKeeperLine("Ines Farrow", "Hauler", 2) ==
              "Ines Farrow - Hauler (a familiar face)");
    SOL_CHECK(game::composeKeeperLine("Ines Farrow", "Hauler", 40) ==
              "Ines Farrow - Hauler (a familiar face)");
    // A trade is required of a `[[character]]` and derived for a regular, so an
    // empty one only ever comes from a caller with no seat - and a bare name is
    // better than a name with a dangling dash after it.
    SOL_CHECK(game::composeKeeperLine("Ines Farrow", "", 0) == "Ines Farrow");
    SOL_CHECK(game::composeKeeperLine("", "Hauler", 0).empty());
}

// ⚑⚑ `validateCharacters` REFUSES, AND THE CONTROL IS THE HALF THAT MATTERS.
// A validator that refused everything would pass every negative case here and
// would break the whole game; the shipped defs going through cleanly is what
// says the refusals below are about the thing they name.
SOL_TEST(a_character_anchored_to_something_that_does_not_exist_is_refused)
{
    // The control, first: the shipped cast validates.
    {
        DefDatabase defs;
        SOL_REQUIRE(loadShippedDefs(defs));
        std::string error;
        SOL_CHECK(defs.validateCharacters(&error));
    }

    const auto refuses = [](const char* toml, const char* because) {
        DefDatabase defs;
        std::string error;
        // The row itself must PARSE - otherwise this tests the parser, not the
        // validator - unless the refusal under test is the parser's own.
        const char* base = R"(
[[faction]]
id = "sol.real"
name = "Real"
color = [1.0, 1.0, 1.0]
kind = "major"

[[module]]
id = "sol.mod_bar"
name = "Bar"
family = "recreation"
power_draw = 2.0

[[module]]
id = "sol.mod_fence"
name = "Fence"
family = "shadow"
power_draw = 1.0

[[station]]
id = "sol.station_real"
name = "Real Station"
weight_core = 1.0
weight_frontier = 1.0
weight_fringe = 1.0
)";
        if (!defs.mergeToml(base, std::strlen(base), "base.toml", &error)) {
            std::printf("  base defs did not parse: %s\n", error.c_str());
            SOL_CHECK(false);
            return;
        }
        error.clear();
        const bool parsed = defs.mergeToml(toml, std::strlen(toml), "cast.toml", &error);
        const bool valid = parsed && defs.validateCharacters(&error);
        if (valid) {
            std::printf("  ACCEPTED a character that %s\n", because);
        } else {
            std::printf("  refused (%s): %s\n", because, error.c_str());
        }
        SOL_CHECK(!valid);
    };

    refuses(R"(
[[character]]
id = "sol.char_ghost"
name = "Ghost"
trade = "Nobody"
faction = "sol.not_a_faction"
)",
            "names a faction that does not exist");

    refuses(R"(
[[character]]
id = "sol.char_ghost"
name = "Ghost"
trade = "Nobody"
archetype = "sol.station_nowhere"
)",
            "names a station archetype that does not exist");

    refuses(R"(
[[character]]
id = "sol.char_ghost"
name = "Ghost"
trade = "Nobody"
room = "sol.mod_nowhere"
)",
            "names a room module that does not exist");

    // ⚑ A ROOM THAT IS NOT A ROOM. This one parses, names a real module, and
    // would select nothing forever - the seat a character takes is by
    // construction a recreation module, because that is the only thing
    // `stationRoom` can return.
    refuses(R"(
[[character]]
id = "sol.char_ghost"
name = "Ghost"
trade = "Nobody"
room = "sol.mod_fence"
)",
            "sits in a room that is not a recreation module");

    refuses(R"(
[[character]]
id = "sol.char_ghost"
name = "Ghost"
trade = "Nobody"
region = "outer"
)",
            "names a region the generator does not have");

    refuses(R"(
[[character]]
id = "sol.char_ghost"
name = "Ghost"
trade = "Nobody"
faction = "sol.real"
lawless = true
)",
            "claims a major's space and a clan's at once");

    // And the positive control for the anchors that DO resolve, so the six
    // refusals above are not just "this validator says no".
    {
        DefDatabase defs;
        std::string error;
        const char* good = R"(
[[faction]]
id = "sol.real"
name = "Real"
color = [1.0, 1.0, 1.0]
kind = "major"

[[module]]
id = "sol.mod_bar"
name = "Bar"
family = "recreation"
power_draw = 2.0

[[station]]
id = "sol.station_real"
name = "Real Station"
weight_core = 1.0
weight_frontier = 1.0
weight_fringe = 1.0

[[character]]
id = "sol.char_real"
name = "Real Person"
trade = "Broker"
faction = "sol.real"
archetype = "sol.station_real"
room = "sol.mod_bar"
region = "core"
)";
        SOL_REQUIRE(defs.mergeToml(good, std::strlen(good), "good.toml", &error));
        SOL_CHECK(defs.validateCharacters(&error));
        SOL_REQUIRE(defs.characters().size() == 1);
        SOL_CHECK(defs.findCharacter("sol.char_real") != nullptr);
        SOL_CHECK(defs.findCharacter("sol.char_nobody") == nullptr);
    }
}
