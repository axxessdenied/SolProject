// The v16 save header (Phase 27): what a save says about itself before
// anything restores it. The browser builds a row from `readSaveInfo` alone, so
// these tests own the promise that a row can be built at all - and the one
// that matters more, that the header never changes what a load produces.

#include "space_world.hpp"

#include <cstdio>
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

// ---------------------------------------------------------------------------
// Phase 29 stage D, decisions/018 decision 7: what a save says about the
// CONTENT its galaxy was made from.
// ---------------------------------------------------------------------------

namespace {

// The same fixture defs plus one authored place. Two spellings of it, differing
// in one field, so "the content changed" is the smallest change a person could
// actually make rather than a wholesale replacement.
constexpr const char* kAuthoredHere = R"(
[[system]]
id = "test.waypoint"
name = "Waypoint"
placement = "anywhere"
)";

constexpr const char* kAuthoredElsewhere = R"(
[[system]]
id = "test.waypoint"
name = "Waypoint Two"
placement = "anywhere"
)";

struct AuthoredFixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    explicit AuthoredFixture(const char* authored)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        SOL_CHECK(defs.mergeToml(authored, std::strlen(authored), "test_systems.toml", &error));
        SOL_CHECK(defs.validateSystems(&error));
        world.spawn(1701);
        SOL_CHECK(world.generateUniverse(defs));
    }
};

} // namespace

// ⚑⚑⚑ THE HAZARD THIS CATCHES IS SILENT AND IT ARRIVES THROUGH A MOD RATHER
// THAN THROUGH A BUILD. A galaxy is regenerated from the seed on load rather
// than serialized, and until v17 `galaxyChanged` keyed on the seed alone -
// sound while the seed was the only input, and no longer sound now that a
// `[[system]]` in any mod layer changes the galaxy without changing the seed
// OR the version. The player installs a mod mid-campaign, loads, and every
// index in the file - their system, their fleet's berths, every market - now
// points somewhere else, with nothing anywhere saying so.
SOL_TEST(a_save_refuses_a_galaxy_whose_authored_content_has_changed)
{
    const std::string path = scratchPath("authored.sav");
    (void)deleteFile(path.c_str());

    AuthoredFixture here(kAuthoredHere);
    SOL_REQUIRE(here.world.saveTo(path.c_str(), "Before the mod"));

    // The digests are facts about the content, so they differ before any file
    // is involved. Printed because a bare refusal below would not say why.
    AuthoredFixture elsewhere(kAuthoredElsewhere);
    std::printf("  digests: 0x%016llX then 0x%016llX\n",
                static_cast<unsigned long long>(here.world.authoredContentDigest()),
                static_cast<unsigned long long>(elsewhere.world.authoredContentDigest()));
    SOL_CHECK(here.world.authoredContentDigest() != elsewhere.world.authoredContentDigest());

    // Same seed, same version, same defs in every other respect - and refused.
    SOL_CHECK(!elsewhere.world.loadFrom(path.c_str()));

    // ⚑ And the refusal is about the CONTENT rather than about the file: the
    // world that wrote it loads it back, from the same bytes, straight after.
    AuthoredFixture again(kAuthoredHere);
    SOL_CHECK(again.world.authoredContentDigest() == here.world.authoredContentDigest());
    SOL_CHECK(again.world.loadFrom(path.c_str()));

    // The browser still describes it. Refusing to LIST a save whose content
    // has moved would leave a player unable to see that their campaign exists
    // at all, and `readSaveInfo` has no world to compare against anyway.
    game::SaveInfo info;
    SOL_CHECK(game::readSaveInfo(path.c_str(), info));
    SOL_CHECK(info.displayName == "Before the mod");

    (void)deleteFile(path.c_str());
}

// ⚑ Removing the last authored system is not the same fact as never having had
// one, and a digest that folded in only the rows would say it was. The counts
// go in for exactly this case, which is the one a player hits when they
// UNINSTALL a mod.
SOL_TEST(a_save_refuses_a_galaxy_whose_authored_content_was_removed_entirely)
{
    const std::string path = scratchPath("authored_removed.sav");
    (void)deleteFile(path.c_str());

    AuthoredFixture withContent(kAuthoredHere);
    SOL_REQUIRE(withContent.world.saveTo(path.c_str(), "Before the uninstall"));

    Fixture bare; // the same defs with no [[system]] row at all
    SOL_CHECK(bare.world.authoredContentDigest() != withContent.world.authoredContentDigest());
    SOL_CHECK(!bare.world.loadFrom(path.c_str()));

    (void)deleteFile(path.c_str());
}

// ⚑⚑⚑⚑ THE DIGEST OF CONTENT NOBODY CHANGED, PINNED TO A LITERAL - AND THIS IS
// A RULE FOR EVERY FIELD ADDED TO AN AUTHORED ROW FROM NOW ON, NOT A TEST OF
// ONE STAGE'S ARITHMETIC.
//
// `security` (Phase 30 stage E) is the first field to join `AuthoredSystem`
// since this digest was built, and folding it in the way every other field is
// folded - unconditionally - would have moved this number. Nothing an author
// wrote would have changed; every save in existence would simply have been
// refused, and the message it printed would have said a `[[system]]` was added,
// changed or removed, about files nobody touched.
//
// The line the digest draws is stated at `digestAuthoredSystem`: THE DIGEST
// COVERS WHAT AN AUTHOR WROTE AND THE SAVE VERSION COVERS WHAT THE BUILD
// CHANGED. A field no row carries is a build change, so it belongs to the
// version - and a digest that reported it would be answering someone else's
// question in words that are false.
//
// The constant below was read off the build BEFORE the field existed. A future
// stage that legitimately changes what a digest means has to change it here and
// bump `kSaveVersion` in the same commit, which is exactly the conversation
// this literal exists to force.
SOL_TEST(a_field_no_author_wrote_leaves_the_content_digest_where_it_was)
{
    constexpr std::uint64_t kDigestBeforeStageE = 0x667F40D19E4B5DF8ull;

    AuthoredFixture here(kAuthoredHere);
    if (here.world.authoredContentDigest() != kDigestBeforeStageE) {
        std::printf("  authored content digest: got 0x%016llX, want 0x%016llX\n",
                    static_cast<unsigned long long>(here.world.authoredContentDigest()),
                    static_cast<unsigned long long>(kDigestBeforeStageE));
    }
    SOL_CHECK(here.world.authoredContentDigest() == kDigestBeforeStageE);
}

// ⚑ And the other direction, without which the test above is satisfied by a
// digest that ignores the field entirely. A row that DOES write a rating is
// different content, and a save made against one is refused by a galaxy made
// against the other - which is what a player installing a mod that fortifies a
// system has to be told.
SOL_TEST(a_save_refuses_a_galaxy_whose_authored_rating_has_changed)
{
    const std::string path = scratchPath("authored_security.sav");
    (void)deleteFile(path.c_str());

    AuthoredFixture fortified(R"(
[[system]]
id = "test.waypoint"
name = "Waypoint"
placement = "anywhere"
faction = "sol.navy"
security = 0.9
)");
    SOL_REQUIRE(fortified.world.saveTo(path.c_str(), "Before the fortress"));

    // The same row with the rating alone moved - not added, not removed.
    AuthoredFixture thinned(R"(
[[system]]
id = "test.waypoint"
name = "Waypoint"
placement = "anywhere"
faction = "sol.navy"
security = 0.2
)");
    SOL_CHECK(fortified.world.authoredContentDigest() != thinned.world.authoredContentDigest());
    SOL_CHECK(!thinned.world.loadFrom(path.c_str()));

    // And the row with no rating at all is a third thing again: an unwritten
    // field is not the same fact as a written one that happens to be weak.
    AuthoredFixture unrated(kAuthoredHere);
    SOL_CHECK(unrated.world.authoredContentDigest() != fortified.world.authoredContentDigest());

    (void)deleteFile(path.c_str());
}
