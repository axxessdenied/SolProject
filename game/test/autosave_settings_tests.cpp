// The autosave settings (Phase 27): their round trip through settings.toml and
// the clamps that stop a hand-edited file from producing behaviour the sliders
// cannot ask for.
//
// This is the first test this project has over Settings::load/save at all. It
// exists because the four new keys include the two shapes this repo has
// already been bitten by: an INTEGER key written by a float-only writer
// (Phase 25 stage E), and a value whose out-of-range spelling is harmless in a
// file and catastrophic in the frame loop (an interval of zero autosaves every
// frame).

#include "menu_screens.hpp"

#include <string>
#include <vector>

#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using game::Settings;
using sol::platform::createDirectories;
using sol::platform::deleteFile;
using sol::platform::readFileBytes;
using sol::platform::writeFileBytes;

namespace {

std::string settingsPath(const char* leaf)
{
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/settings";
    SOL_CHECK(createDirectories(dir.c_str()));
    return dir + "/" + leaf;
}

std::string readText(const std::string& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readFileBytes(path.c_str(), bytes)) {
        return {};
    }
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

SOL_TEST(the_autosave_settings_survive_a_round_trip)
{
    const std::string path = settingsPath("roundtrip.toml");
    (void)deleteFile(path.c_str());

    Settings written;
    written.autosaveEnabled = false;
    written.autosaveMinutes = 12.0f;
    written.autosaveOnDock = false;
    written.autosaveKeep = 7;
    SOL_REQUIRE(written.save(path.c_str()));

    Settings read;
    SOL_REQUIRE(read.load(path.c_str()));
    SOL_CHECK(!read.autosaveEnabled);
    SOL_CHECK(read.autosaveMinutes == 12.0f);
    SOL_CHECK(!read.autosaveOnDock);
    SOL_CHECK(read.autosaveKeep == 7);

    // ⚑ The COUNT is written as an integer, not as "7.000". This repo has
    // shipped the other spelling before (a write-a-number helper that always
    // emitted a float could not write an integer key), and settings.toml is a
    // file people hand-edit: "autosave_keep = 7.000" reads as though a
    // fractional number of autosaves were something you could ask for.
    const std::string text = readText(path);
    SOL_CHECK(text.find("autosave_keep = 7\n") != std::string::npos);
    SOL_CHECK(text.find("autosave_keep = 7.000") == std::string::npos);

    // And the four keys did not push the older ones off the end of the
    // writer's buffer, which snprintf would do by TRUNCATING rather than
    // failing - silently, and only once the file got long enough.
    SOL_CHECK(text.find("master_volume") != std::string::npos);
    SOL_CHECK(text.find("[bindings]") != std::string::npos);

    (void)deleteFile(path.c_str());
}

SOL_TEST(a_hand_edited_autosave_interval_cannot_ask_for_every_frame)
{
    // ⚑⚑ THE CLAMP THAT MATTERS MOST, AND ITS BAD VALUE IS THE INNOCENT-LOOKING
    // ONE. `autosave_minutes = 0` is a perfectly ordinary thing to type, and
    // the frame loop compares elapsed >= interval - so zero means "due every
    // frame", i.e. a 17 KB write per frame forever. The reader clamps to the
    // slider's own range rather than trusting the file.
    const std::string path = settingsPath("hand_edited.toml");
    // The length comes from the literal rather than being counted by hand: the
    // first draft said 64 for a 65-byte string and quietly dropped the final
    // newline, which TOML happens to forgive - so the test passed while
    // testing a slightly different file than the one it reads as.
    const char* edited = "autosave_enabled = true\n"
                         "autosave_minutes = 0.0\n"
                         "autosave_keep = 0\n";
    SOL_REQUIRE(writeFileBytes(path.c_str(), edited, std::char_traits<char>::length(edited)));

    Settings settings;
    SOL_REQUIRE(settings.load(path.c_str()));
    SOL_CHECK(settings.autosaveMinutes >= 1.0f);
    SOL_CHECK(settings.autosaveKeep >= 1);

    (void)deleteFile(path.c_str());
}

SOL_TEST(a_missing_or_broken_autosave_key_keeps_the_shipped_default)
{
    // A settings file must never be the reason the game misbehaves - the same
    // rule the bindings loader already follows. An absent key, a key of the
    // wrong type, and an out-of-range one all land on something playable.
    const std::string path = settingsPath("partial.toml");
    const char* text = "master_volume = 0.5\nautosave_minutes = \"soon\"\nautosave_keep = 999\n";
    SOL_REQUIRE(writeFileBytes(path.c_str(), text, std::char_traits<char>::length(text)));

    const Settings defaults;
    Settings settings;
    SOL_REQUIRE(settings.load(path.c_str()));

    SOL_CHECK(settings.masterVolume == 0.5f);                        // the one real key
    SOL_CHECK(settings.autosaveEnabled == defaults.autosaveEnabled); // absent
    SOL_CHECK(settings.autosaveMinutes == defaults.autosaveMinutes); // wrong type
    SOL_CHECK(settings.autosaveOnDock == defaults.autosaveOnDock);   // absent
    SOL_CHECK(settings.autosaveKeep <= 10);                          // clamped

    // ⚑ Autosave defaults to ON, and that is a decision rather than an
    // oversight: the players this feature protects are the ones who have not
    // thought about saving, and they will not go looking for a switch.
    SOL_CHECK(defaults.autosaveEnabled);

    (void)deleteFile(path.c_str());
}
