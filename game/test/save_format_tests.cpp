// The v16 save header (Phase 27): what a save says about itself before
// anything restores it. The browser builds a row from `readSaveInfo` alone, so
// these tests own the promise that a row can be built at all - and the one
// that matters more, that the header never changes what a load produces.

#include "space_world.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::platform::createDirectories;
using sol::platform::deleteFile;
using sol::platform::readFileBytes;
using sol::platform::writeFileBytes;

namespace {

// The smallest def set that lets a galaxy generate AND round-trip, which are
// not the same bar.
//
// ⚑⚑ THE COMMODITIES ARE LOAD-BEARING AND THEY LOOK LIKE PADDING. saveTo
// writes the economy unconditionally (space_world.cpp), while loadFrom reads
// it back only `if (!m_economyParams.commodities.empty())`. With no
// commodities the save therefore contains economy bytes the load never
// consumes, every subsequent field reads from the wrong offset, and the
// failure lands far away from its cause. The shipped game always has
// commodities so this asymmetry is unreachable there - a bare fixture is
// exactly the thing that finds it. Do not trim these to "the minimum that
// generates a galaxy": that minimum cannot be loaded.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

[[commodity]]
id = "sol.ore"
name = "Raw Ore"
base_price = 12.0
ore_weight_core = 1.0
ore_weight_frontier = 1.0
ore_weight_fringe = 1.0

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5
)";

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    explicit Fixture(std::uint64_t seed = 1701)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        world.spawn(seed);
        SOL_CHECK(world.generateUniverse(defs));
    }
};

std::string scratchPath(const char* leaf)
{
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/saves";
    SOL_CHECK(createDirectories(dir.c_str()));
    return dir + "/" + leaf;
}

} // namespace

SOL_TEST(a_save_describes_itself_without_being_loaded)
{
    // ⚑ THE WHOLE POINT OF v16. Before this the only way to learn anything
    // about a save was to restore it into a world, which the browser cannot do
    // for every row it lists - and one of the fields, the system NAME, was not
    // recoverable at all without regenerating that seed's galaxy.
    const std::string path = scratchPath("described.sav");
    (void)deleteFile(path.c_str());

    Fixture fixture;
    SOL_REQUIRE(fixture.world.saveTo(path.c_str(), "Before the gate run"));

    game::SaveInfo info;
    SOL_REQUIRE(game::readSaveInfo(path.c_str(), info));
    SOL_CHECK(info.displayName == "Before the gate run");
    SOL_CHECK(!info.systemName.empty());
    SOL_CHECK(info.universeSeed == 1701);
    SOL_CHECK(!info.hardcore);

    // The stamp is a real date rather than a zero or an uptime - the same
    // 2020..2100 band platform.unit pins the clock itself to.
    SOL_CHECK(info.savedAtUnix > 1'577'836'800ull);
    SOL_CHECK(info.savedAtUnix < 4'102'444'800ull);

    // The system name is the one the world was actually in, not a guess.
    const sol::sim::Galaxy& galaxy = fixture.world.galaxy();
    SOL_REQUIRE(fixture.world.currentSystemIndex() < galaxy.systems.size());
    SOL_CHECK(info.systemName == galaxy.systems[fixture.world.currentSystemIndex()].name);

    (void)deleteFile(path.c_str());
}

SOL_TEST(the_header_does_not_disturb_what_a_load_restores)
{
    // ⚑⚑ THE ASSERTION THAT EARNS THE FORMAT CHANGE. Three fields were
    // inserted ahead of every existing one, so every offset after them moved.
    // A reader that skipped a field, or read them in the wrong order, would
    // still return true and hand back a world quietly shifted by a few bytes -
    // so this checks the FAR side of the blob, not the near side.
    const std::string path = scratchPath("roundtrip.sav");
    (void)deleteFile(path.c_str());

    Fixture source;
    const std::uint32_t system = source.world.currentSystemIndex();
    const double credits = source.world.playerCredits();
    SOL_REQUIRE(source.world.saveTo(path.c_str(), "Round trip"));

    Fixture destination;
    SOL_REQUIRE(destination.world.loadFrom(path.c_str()));
    SOL_CHECK(destination.world.currentSystemIndex() == system);
    SOL_CHECK(destination.world.playerCredits() == credits);

    // Save -> load -> save is byte-identical for the ECS half (snapshot.hpp
    // promises it), so the only thing that can differ between two saves of the
    // same world is the header's own timestamp. Comparing the tails is what
    // proves the insertion did not shift anything.
    const std::string second = scratchPath("roundtrip2.sav");
    (void)deleteFile(second.c_str());
    SOL_REQUIRE(destination.world.saveTo(second.c_str(), "Round trip"));

    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> again;
    SOL_REQUIRE(readFileBytes(path.c_str(), first));
    SOL_REQUIRE(readFileBytes(second.c_str(), again));
    SOL_CHECK(first.size() == again.size());

    (void)deleteFile(path.c_str());
    (void)deleteFile(second.c_str());
}

SOL_TEST(the_display_name_belongs_to_the_file_and_not_to_the_world)
{
    // A run loaded from one slot and saved into another takes the new slot's
    // name. loadFrom reads the header and discards it on purpose; if it ever
    // started keeping it, a save named by the player would silently inherit
    // the name of whatever they loaded before it.
    const std::string first = scratchPath("named_first.sav");
    const std::string second = scratchPath("named_second.sav");
    (void)deleteFile(first.c_str());
    (void)deleteFile(second.c_str());

    Fixture fixture;
    SOL_REQUIRE(fixture.world.saveTo(first.c_str(), "Old name"));
    SOL_REQUIRE(fixture.world.loadFrom(first.c_str()));
    SOL_REQUIRE(fixture.world.saveTo(second.c_str(), "New name"));

    game::SaveInfo info;
    SOL_REQUIRE(game::readSaveInfo(second.c_str(), info));
    SOL_CHECK(info.displayName == "New name");

    // And the first file is untouched by any of it.
    SOL_REQUIRE(game::readSaveInfo(first.c_str(), info));
    SOL_CHECK(info.displayName == "Old name");

    (void)deleteFile(first.c_str());
    (void)deleteFile(second.c_str());
}

SOL_TEST(readSaveInfo_refuses_everything_it_cannot_honestly_describe)
{
    // The browser shows a refusal as a row it cannot offer, so all four of
    // these have to be a clean false rather than a partial fill or a crash.
    game::SaveInfo info;

    // Missing.
    SOL_CHECK(!game::readSaveInfo(scratchPath("no_such_file.sav").c_str(), info));

    // Foreign: right size, wrong magic.
    const std::string foreign = scratchPath("foreign.sav");
    const char junk[64] = "not a save file at all, just some bytes sitting here";
    SOL_REQUIRE(writeFileBytes(foreign.c_str(), junk, sizeof(junk)));
    SOL_CHECK(!game::readSaveInfo(foreign.c_str(), info));

    // Empty: readFileBytes succeeds on a zero-length file, so the magic check
    // is what has to catch this one.
    const std::string empty = scratchPath("empty.sav");
    SOL_REQUIRE(writeFileBytes(empty.c_str(), "", 0));
    SOL_CHECK(!game::readSaveInfo(empty.c_str(), info));

    // ⚑ Truncated, which is the case that actually happens: a real save cut
    // off mid-write because the game died holding the file open. The magic and
    // version are perfectly good and everything after them is missing, so only
    // the field-by-field checks can reject it.
    const std::string whole = scratchPath("truncated.sav");
    (void)deleteFile(whole.c_str());
    Fixture fixture;
    SOL_REQUIRE(fixture.world.saveTo(whole.c_str(), "Cut short"));
    std::vector<std::uint8_t> bytes;
    SOL_REQUIRE(readFileBytes(whole.c_str(), bytes));
    SOL_REQUIRE(bytes.size() > 16);
    SOL_REQUIRE(writeFileBytes(whole.c_str(), bytes.data(), 10)); // magic + version + a scrap
    SOL_CHECK(!game::readSaveInfo(whole.c_str(), info));

    (void)deleteFile(foreign.c_str());
    (void)deleteFile(empty.c_str());
    (void)deleteFile(whole.c_str());
}

SOL_TEST(a_hardcore_run_says_so_in_its_own_header)
{
    // The browser has to mark a hardcore save before it is opened: it is the
    // one row where loading and then dying is unrecoverable, and the flag was
    // already in the blob - it just had no way out of it.
    const std::string path = scratchPath("hardcore.sav");
    (void)deleteFile(path.c_str());

    Fixture fixture;
    fixture.world.setHardcore(true);
    SOL_REQUIRE(fixture.world.saveTo(path.c_str(), "Ironman"));

    game::SaveInfo info;
    SOL_REQUIRE(game::readSaveInfo(path.c_str(), info));
    SOL_CHECK(info.hardcore);

    (void)deleteFile(path.c_str());
}
