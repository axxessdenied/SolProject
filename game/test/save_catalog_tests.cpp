// The save catalog (Phase 27): campaign folders, slot numbering, the autosave
// ring, and the two deletes. Everything here is filesystem behaviour, so the
// suite writes real files into the build tree's scratch directory - the same
// arrangement platform.unit uses, and for the same reason: the thing under
// test IS the filesystem.

#include "save_catalog.hpp"
#include "space_world.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using game::Campaign;
using game::SaveCatalog;
using game::SaveKind;
using game::SaveSlot;
using sol::assets::DefDatabase;
using sol::platform::createDirectories;
using sol::platform::deleteDirectory;
using sol::platform::listDirectories;
using sol::platform::writeFileBytes;

namespace {

// The commodities are load-bearing here for the same reason they are in
// save_format_tests.cpp: saveTo writes the economy unconditionally and
// loadFrom reads it back only when commodities exist, so a fixture without
// them writes saves that cannot be loaded.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

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

// A world that can write a real save, so the catalog is exercised against the
// files the game actually produces rather than against stand-ins.
struct World
{
    DefDatabase defs;
    game::SpaceWorld world;

    World()
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        world.spawn(1701);
        SOL_CHECK(world.generateUniverse(defs));
    }

    void writeSave(const std::string& path, const char* name)
    {
        SOL_REQUIRE(world.saveTo(path.c_str(), name));
    }
};

// Each test gets its own root, emptied first, so one failing test cannot leave
// residue that changes what the next one sees.
std::string freshRoot(const char* leaf)
{
    const std::string root = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/catalog/" + leaf;
    SOL_CHECK(deleteDirectory(root.c_str()));
    SOL_CHECK(createDirectories(root.c_str()));
    return root;
}

} // namespace

SOL_TEST(a_campaign_is_a_folder_and_two_runs_can_share_a_name)
{
    SaveCatalog catalog;
    catalog.initialize(freshRoot("naming"));
    SOL_CHECK(catalog.empty());

    const Campaign* first = catalog.createCampaign("Ore Run");
    SOL_REQUIRE(first != nullptr);
    SOL_CHECK(first->name == "Ore Run");

    // ⚑ The second run honestly wants the same name and gets " 2" rather than
    // silently landing in the first one's folder, which would mix two runs'
    // saves into one list with no way to tell them apart.
    const Campaign* second = catalog.createCampaign("Ore Run");
    SOL_REQUIRE(second != nullptr);
    SOL_CHECK(second->name == "Ore Run 2");
    SOL_CHECK(catalog.campaigns().size() == 2);

    // Both folders really exist, which is the half a name comparison cannot
    // see: createCampaign returning a Campaign is not the same as a directory.
    SOL_CHECK(listDirectories(catalog.root().c_str()).size() == 2);
}

SOL_TEST(an_empty_campaign_is_still_listed)
{
    // ⚑⚑ THE REASON listDirectories EXISTS. A campaign whose every save has
    // been deleted is still a campaign the player made and can save into
    // again. listFiles cannot see the folder at all, so a catalog built on it
    // would make the campaign vanish the moment its last save went - which
    // reads as "the game deleted my run".
    SaveCatalog catalog;
    catalog.initialize(freshRoot("empty"));
    const Campaign* campaign = catalog.createCampaign("Nothing Yet");
    SOL_REQUIRE(campaign != nullptr);

    catalog.rescan();
    SOL_REQUIRE(catalog.campaigns().size() == 1);
    SOL_CHECK(catalog.campaigns()[0].name == "Nothing Yet");
    SOL_CHECK(catalog.campaigns()[0].empty());
    SOL_CHECK(catalog.campaigns()[0].newest() == nullptr);
    SOL_CHECK(catalog.mostRecentSave() == nullptr);
}

SOL_TEST(a_manual_save_never_overwrites_one_that_is_already_there)
{
    // A player who names a save is saying it is worth keeping, so the game
    // never overwrites one: the next manual path always names a file that does
    // not exist yet.
    //
    // ⚑ It DOES reuse a number freed by a delete, and this test pins that on
    // purpose. The first draft asserted the opposite - "a deleted number is
    // never handed out again" - and the implementation disagreed, which forced
    // the question of which was right. Reuse is: the player never sees a
    // filename (a save's name is in its header), and remembering every number
    // ever issued would need persistent state to answer a question nobody
    // asks. The promise is "no collision", not "no reuse".
    SaveCatalog catalog;
    catalog.initialize(freshRoot("manual"));
    const Campaign* campaign = catalog.createCampaign("Numbering");
    SOL_REQUIRE(campaign != nullptr);
    World world;

    const std::string first = catalog.nextManualPath(*campaign);
    SOL_CHECK(first.find("save_01.sav") != std::string::npos);
    world.writeSave(first, "One");
    catalog.rescan();

    campaign = catalog.find("Numbering");
    SOL_REQUIRE(campaign != nullptr);
    const std::string second = catalog.nextManualPath(*campaign);
    SOL_CHECK(second.find("save_02.sav") != std::string::npos);
    world.writeSave(second, "Two");
    catalog.rescan();

    campaign = catalog.find("Numbering");
    SOL_REQUIRE(campaign != nullptr);
    SOL_REQUIRE(campaign->saves.size() == 2);

    // Delete save_02 and ask again: the number is free, so it comes back - and
    // what matters is that nothing is at that path to be overwritten.
    const SaveSlot* toDelete = nullptr;
    for (const SaveSlot& slot : campaign->saves) {
        if (slot.fileName == "save_02.sav") {
            toDelete = &slot;
        }
    }
    SOL_REQUIRE(toDelete != nullptr);
    SOL_CHECK(catalog.deleteSave(*toDelete));

    campaign = catalog.find("Numbering");
    SOL_REQUIRE(campaign != nullptr);
    SOL_CHECK(campaign->saves.size() == 1);
    const std::string reused = catalog.nextManualPath(*campaign);
    SOL_CHECK(reused.find("save_02.sav") != std::string::npos);

    // The promise, stated directly: whatever number comes back, no save is
    // sitting at that path. This is the assertion that would fail if the
    // numbering ever started colliding, and it does not care which number won.
    game::SaveInfo occupied;
    SOL_CHECK(!game::readSaveInfo(reused.c_str(), occupied));
}

SOL_TEST(the_autosave_ring_fills_before_it_replaces_and_then_takes_the_oldest)
{
    SaveCatalog catalog;
    catalog.initialize(freshRoot("ring"));
    SOL_REQUIRE(catalog.createCampaign("Ring") != nullptr);
    World world;
    constexpr std::uint32_t kRing = 3;

    // Filling: three fresh slots before anything is replaced. A ring that
    // replaced auto_01 every time would leave a campaign with one autosave
    // however high the setting was.
    for (std::uint32_t expected = 1; expected <= kRing; ++expected) {
        const Campaign* campaign = catalog.find("Ring");
        SOL_REQUIRE(campaign != nullptr);
        const std::string path = catalog.nextAutoPath(*campaign, kRing);
        char wanted[32] = {};
        (void)std::snprintf(wanted, sizeof(wanted), "auto_%02u.sav", expected);
        SOL_CHECK(path.find(wanted) != std::string::npos);
        world.writeSave(path, "Autosave");
        catalog.rescan();
    }

    const Campaign* campaign = catalog.find("Ring");
    SOL_REQUIRE(campaign != nullptr);
    SOL_CHECK(campaign->saves.size() == kRing);

    // Full: the next one replaces the OLDEST of the three. The three were
    // written in order, so the oldest is auto_01 - and this is the assertion
    // that would fail if the replacement picked the lowest index by accident
    // rather than by age, so the two are separated below.
    const std::string replaced = catalog.nextAutoPath(*campaign, kRing);
    SOL_CHECK(replaced.find("auto_01.sav") != std::string::npos);
    world.writeSave(replaced, "Autosave");
    catalog.rescan();

    // Now auto_01 is the NEWEST, so the next victim must be auto_02. A "lowest
    // index" implementation returns auto_01 again here and this fails.
    campaign = catalog.find("Ring");
    SOL_REQUIRE(campaign != nullptr);
    SOL_CHECK(campaign->saves.size() == kRing); // still three: it replaced, not added
    SOL_CHECK(catalog.nextAutoPath(*campaign, kRing).find("auto_02.sav") != std::string::npos);
}

SOL_TEST(lowering_the_ring_size_does_not_delete_the_autosaves_above_it)
{
    // ⚑ A setting change must not destroy data. Dropping the ring from four to
    // two means the next autosave lands inside the new ring; auto_03 and
    // auto_04 stay on disk and stay listed until the player deletes them.
    SaveCatalog catalog;
    catalog.initialize(freshRoot("shrink"));
    SOL_REQUIRE(catalog.createCampaign("Shrink") != nullptr);
    World world;

    for (std::uint32_t i = 0; i < 4; ++i) {
        const Campaign* campaign = catalog.find("Shrink");
        SOL_REQUIRE(campaign != nullptr);
        world.writeSave(catalog.nextAutoPath(*campaign, 4), "Autosave");
        catalog.rescan();
    }
    const Campaign* campaign = catalog.find("Shrink");
    SOL_REQUIRE(campaign != nullptr);
    SOL_REQUIRE(campaign->saves.size() == 4);

    const std::string next = catalog.nextAutoPath(*campaign, 2);
    SOL_CHECK(next.find("auto_01.sav") != std::string::npos); // oldest within the new ring
    world.writeSave(next, "Autosave");
    catalog.rescan();

    campaign = catalog.find("Shrink");
    SOL_REQUIRE(campaign != nullptr);
    SOL_CHECK(campaign->saves.size() == 4); // nothing was removed
}

SOL_TEST(the_catalog_reads_each_saves_own_header_and_orders_newest_first)
{
    SaveCatalog catalog;
    catalog.initialize(freshRoot("headers"));
    SOL_REQUIRE(catalog.createCampaign("Reading") != nullptr);
    World world;

    const Campaign* campaign = catalog.find("Reading");
    SOL_REQUIRE(campaign != nullptr);
    world.writeSave(catalog.nextManualPath(*campaign), "First light");
    catalog.rescan();
    campaign = catalog.find("Reading");
    SOL_REQUIRE(campaign != nullptr);
    world.writeSave(catalog.quickPath(*campaign), "Quicksave");
    catalog.rescan();

    campaign = catalog.find("Reading");
    SOL_REQUIRE(campaign != nullptr);
    SOL_REQUIRE(campaign->saves.size() == 2);

    // Both rows carry what the browser prints, straight out of each file.
    bool sawManual = false;
    bool sawQuick = false;
    for (const SaveSlot& slot : campaign->saves) {
        SOL_CHECK(!slot.info.systemName.empty());
        SOL_CHECK(slot.info.universeSeed == 1701);
        if (slot.kind == SaveKind::Manual) {
            sawManual = true;
            SOL_CHECK(slot.info.displayName == "First light");
        }
        if (slot.kind == SaveKind::Quick) {
            sawQuick = true;
            SOL_CHECK(slot.fileName == "quick.sav");
        }
    }
    SOL_CHECK(sawManual);
    SOL_CHECK(sawQuick);

    // And the campaign is the most recent one, which is what Continue resumes.
    SOL_REQUIRE(catalog.mostRecentSave() != nullptr);
    SOL_REQUIRE(catalog.mostRecentCampaign() != nullptr);
    SOL_CHECK(catalog.mostRecentCampaign()->name == "Reading");
}

SOL_TEST(a_stray_file_in_a_campaign_folder_is_ignored_rather_than_listed)
{
    // Players open these folders. A README, a screenshot, a backup someone
    // renamed - none of them is a save, and none of them should produce a row
    // or a warning storm.
    SaveCatalog catalog;
    catalog.initialize(freshRoot("strays"));
    const Campaign* campaign = catalog.createCampaign("Strays");
    SOL_REQUIRE(campaign != nullptr);
    const std::string directory = campaign->directory;

    SOL_REQUIRE(writeFileBytes((directory + "/notes.txt").c_str(), "hello", 5));
    SOL_REQUIRE(writeFileBytes((directory + "/save_backup.bak").c_str(), "hello", 5));
    SOL_REQUIRE(writeFileBytes((directory + "/save_.sav").c_str(), "hello", 5)); // no number
    catalog.rescan();

    campaign = catalog.find("Strays");
    SOL_REQUIRE(campaign != nullptr);
    SOL_CHECK(campaign->empty());
}

SOL_TEST(deleting_a_campaign_goes_through_the_catalog_and_refuses_a_name_it_does_not_know)
{
    // ⚑⚑ THE GUARD ON THE ONE RECURSIVE DELETE IN THE GAME. deleteCampaign
    // takes a NAME and looks the path up itself, so a caller cannot hand it a
    // path at all - which is what makes it safe to wire to a menu button.
    SaveCatalog catalog;
    catalog.initialize(freshRoot("delete"));
    SOL_REQUIRE(catalog.createCampaign("Doomed") != nullptr);
    SOL_REQUIRE(catalog.createCampaign("Keeper") != nullptr);
    World world;

    const Campaign* doomed = catalog.find("Doomed");
    SOL_REQUIRE(doomed != nullptr);
    world.writeSave(catalog.nextManualPath(*doomed), "Goodbye");
    catalog.rescan();

    SOL_CHECK(!catalog.deleteCampaign("No Such Campaign"));
    SOL_CHECK(!catalog.deleteCampaign("")); // an empty name is not the root
    SOL_CHECK(catalog.campaigns().size() == 2);

    SOL_CHECK(catalog.deleteCampaign("Doomed"));
    SOL_REQUIRE(catalog.campaigns().size() == 1);
    SOL_CHECK(catalog.campaigns()[0].name == "Keeper");
    SOL_CHECK(listDirectories(catalog.root().c_str()).size() == 1);
}

SOL_TEST(a_campaign_name_is_reduced_to_something_a_folder_can_be_called)
{
    using game::sanitizeCampaignName;

    SOL_CHECK(sanitizeCampaignName("Ore Run") == "Ore Run");
    SOL_CHECK(sanitizeCampaignName("  Ore   Run  ") == "Ore Run"); // trimmed and collapsed
    SOL_CHECK(sanitizeCampaignName("Nyx/../etc") == "Nyxetc");     // no traversal survives
    SOL_CHECK(sanitizeCampaignName("C:\\Windows") == "CWindows");
    SOL_CHECK(sanitizeCampaignName("run?*<>|\"") == "run");
    SOL_CHECK(sanitizeCampaignName("keep-me_2") == "keep-me_2");

    // A name that strips to nothing still has to name a folder.
    SOL_CHECK(sanitizeCampaignName("") == "Campaign");
    SOL_CHECK(sanitizeCampaignName("???") == "Campaign");
    SOL_CHECK(sanitizeCampaignName("   ") == "Campaign");

    // ⚑ Windows silently strips trailing spaces and dots from directory names,
    // so a folder created as "Run " is a folder called "Run" - which the
    // catalog would then fail to find under the name it asked for. Refusing to
    // produce one is cheaper than reconciling the difference afterwards.
    SOL_CHECK(sanitizeCampaignName("Run ") == "Run");
    SOL_CHECK(sanitizeCampaignName("Run...") == "Run");

    // Capped, and the cap is a real one rather than an assumption: a campaign
    // name is joined onto a saves directory that already sits under a user
    // profile path.
    SOL_CHECK(sanitizeCampaignName(std::string(200, 'x')).size() == 64);
}

SOL_TEST(playtime_and_date_are_formatted_for_a_row_and_refuse_to_invent_one)
{
    using game::formatPlaytime;
    using game::formatSaveDate;

    SOL_CHECK(formatPlaytime(0.0) == "0m");
    SOL_CHECK(formatPlaytime(59.0) == "0m");
    SOL_CHECK(formatPlaytime(60.0) == "1m");
    SOL_CHECK(formatPlaytime(3600.0) == "1h 0m");
    SOL_CHECK(formatPlaytime(12000.0) == "3h 20m");
    SOL_CHECK(formatPlaytime(-5.0) == "0m"); // a negative clock is not a crash

    // ⚑ A save with no stamp prints NOTHING rather than 1970. An empty cell is
    // honest about not knowing; "1970-01-01" is a wrong answer that looks like
    // a right one, which is the failure mode a browser row can least afford.
    SOL_CHECK(formatSaveDate(0).empty());

    const std::string date = formatSaveDate(1'700'000'000ull);
    SOL_REQUIRE(date.size() == 16); // "YYYY-MM-DD HH:MM"
    SOL_CHECK(date.compare(0, 8, "2023-11-") == 0);
}
