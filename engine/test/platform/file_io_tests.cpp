#include <algorithm>
#include <string>
#include <vector>

#include <sol/platform/file_io.hpp>
#include <sol/platform/time.hpp>
#include <sol/test/test.hpp>

using sol::platform::copyFileIfAbsent;
using sol::platform::createDirectories;
using sol::platform::deleteDirectory;
using sol::platform::deleteFile;
using sol::platform::listDirectories;
using sol::platform::listFiles;
using sol::platform::moveFile;
using sol::platform::readFileBytes;
using sol::platform::writeFileBytes;

namespace {

// The suite writes real files, because the thing under test IS the filesystem.
// Everything lands under one directory that each test makes and empties itself,
// so a failed run leaves no residue for the next one to trip over.
std::string scratchRoot()
{
    return std::string(SOL_PLATFORM_TEST_SCRATCH_DIR) + "/file_io";
}

bool writeText(const std::string& path, const std::string& text)
{
    return writeFileBytes(path.c_str(), text.data(), text.size());
}

std::string readText(const std::string& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readFileBytes(path.c_str(), bytes)) {
        return {};
    }
    return std::string(bytes.begin(), bytes.end());
}

bool exists(const std::string& path)
{
    std::vector<std::uint8_t> bytes;
    return readFileBytes(path.c_str(), bytes);
}

} // namespace

SOL_TEST(moveFileTakesTheContentAndLeavesNothingBehind)
{
    const std::string dir = scratchRoot();
    SOL_REQUIRE(createDirectories(dir.c_str()));
    const std::string from = dir + "/move_from.txt";
    const std::string to = dir + "/move_to.txt";
    (void)deleteFile(from.c_str());
    (void)deleteFile(to.c_str());

    SOL_REQUIRE(writeText(from, "cargo"));
    SOL_CHECK(moveFile(from.c_str(), to.c_str()));

    // Both halves matter: a copy would pass the first check and fail the second.
    SOL_CHECK(readText(to) == "cargo");
    SOL_CHECK(!exists(from));

    (void)deleteFile(to.c_str());
}

SOL_TEST(moveFileReplacesWhateverIsAlreadyAtTheDestination)
{
    // A re-send from Blender archives a file whose name is already in the
    // archive, so refusing here would strand the newer drop in the inbox.
    const std::string dir = scratchRoot();
    SOL_REQUIRE(createDirectories(dir.c_str()));
    const std::string from = dir + "/replace_from.txt";
    const std::string to = dir + "/replace_to.txt";

    SOL_REQUIRE(writeText(to, "older"));
    SOL_REQUIRE(writeText(from, "newer"));
    SOL_CHECK(moveFile(from.c_str(), to.c_str()));
    SOL_CHECK(readText(to) == "newer");
    SOL_CHECK(!exists(from));

    (void)deleteFile(to.c_str());
}

SOL_TEST(moveFileFailsWhenTheSourceIsMissing)
{
    // Unlike deleteFile, which reports success for an already-absent file,
    // a move that moved nothing has not done what the caller asked.
    const std::string dir = scratchRoot();
    SOL_REQUIRE(createDirectories(dir.c_str()));
    const std::string from = dir + "/not_here_at_all.txt";
    const std::string to = dir + "/never_written.txt";
    (void)deleteFile(from.c_str());
    (void)deleteFile(to.c_str());

    SOL_CHECK(!moveFile(from.c_str(), to.c_str()));
    SOL_CHECK(!exists(to));
}

SOL_TEST(moveFileDoesNotCreateTheDestinationDirectory)
{
    // Documented behaviour rather than an accident: the caller creates the
    // archive directory once, instead of every move re-deriving it.
    const std::string dir = scratchRoot();
    SOL_REQUIRE(createDirectories(dir.c_str()));
    const std::string from = dir + "/needs_a_home.txt";
    const std::string to = dir + "/no_such_dir/needs_a_home.txt";

    SOL_REQUIRE(writeText(from, "homeless"));
    SOL_CHECK(!moveFile(from.c_str(), to.c_str()));
    SOL_CHECK(exists(from)); // a failed move must not consume the source

    (void)deleteFile(from.c_str());
}

SOL_TEST(listFilesDescendsIntoSubdirectories)
{
    // The Forge's inbox archive lives in a subdirectory of the directory it
    // watches, so this recursion is load-bearing for stage R's filter rather
    // than an incidental property. Asserted here so a change to listFiles
    // fails in the layer that owns it.
    const std::string dir = scratchRoot() + "/recurse";
    const std::string nested = dir + "/nested";
    SOL_REQUIRE(createDirectories(nested.c_str()));
    const std::string top = dir + "/top.txt";
    const std::string deep = nested + "/deep.txt";
    SOL_REQUIRE(writeText(top, "t"));
    SOL_REQUIRE(writeText(deep, "d"));

    const std::vector<std::string> files = listFiles(dir.c_str());
    const auto has = [&files](const char* needle) {
        return std::any_of(files.begin(), files.end(), [needle](const std::string& path) {
            return path.find(needle) != std::string::npos;
        });
    };
    SOL_CHECK(has("top.txt"));
    SOL_CHECK(has("deep.txt"));

    (void)deleteFile(top.c_str());
    (void)deleteFile(deep.c_str());
}

// ⚑⚑ Phase 22. The two promises in executableDirectory's header comment, and
// both of them have now cost a bug rather than being hypothetical.
//
// The trailing separator is documented in both backends already: every caller
// concatenates straight onto the result (`executableDir + "world.sav"`), so
// dropping it moves the save file, the settings and the cooked directory one
// level up without any error anywhere.
//
// The separator CHARACTER is the one Phase 22 found. listFiles has always
// promised '/', executableDirectory returned Win32's '\', and the caller that
// combines them - GameContent::initialize, stripping `modsDirectory + "/"` off
// each listFiles result to recover the mod name - therefore never matched its
// prefix and produced a mod layer literally named "C:". It needed a shipping
// layout AND a mods directory that exists, so it hid until this phase created
// game/mods. A mutation restoring either backslashes or the missing separator
// fails this test on Windows and is invisible on Linux, which is exactly why
// the assertion is written here and not left to a Windows-only reviewer.
SOL_TEST(executableDirectoryKeepsItsSeparatorPromises)
{
    const std::string dir = sol::platform::executableDirectory();
    SOL_REQUIRE(!dir.empty());
    SOL_CHECK(dir.back() == '/');
    SOL_CHECK(dir.find('\\') == std::string::npos);
}

// The other half of the same contract: whatever listFiles hands back must be
// recoverable by stripping the directory it was given as a plain string
// prefix. That is the operation content.cpp performs on the mods directory,
// and it is the one that broke.
//
// ⚑ This test does NOT call executableDirectory, and the distinction matters
// enough to name: disabling the Win32 normalisation fails the test above and
// leaves this one green, because this one supplies its own '/' directory. It
// guards listFiles' end of the promise, not the pair of them together.
SOL_TEST(listFilesResultsArePrefixedByTheDirectoryAsGiven)
{
    const std::string dir = scratchRoot() + "/separators";
    SOL_REQUIRE(createDirectories(dir.c_str()));
    const std::string file = dir + "/marker.txt";
    SOL_REQUIRE(writeText(file, "m"));

    const std::vector<std::string> files = listFiles(dir.c_str());
    SOL_REQUIRE(!files.empty());
    for (const std::string& path : files) {
        SOL_CHECK(path.find('\\') == std::string::npos);
        SOL_CHECK(path.rfind(dir + "/", 0) == 0);
    }

    (void)deleteFile(file.c_str());
}

// ⚑⚑ Phase 22 stage B. userDataDirectory carries the SAME two promises
// executableDirectory does, and it carries them somewhere worse to get wrong:
// this is where a player's save goes. A missing trailing separator turns
// ".../The Stars Don't Wait/" + "world.sav" into a FILE named
// "The Stars Don't Waitworld.sav" one level up, which no error anywhere would
// report - the write succeeds.
//
// The contract is also "non-empty means usable": the function creates the
// directory and returns empty rather than handing back a path whose first
// write will fail. So asserting a file can be written there is asserting the
// contract, not testing the filesystem.
// ⚑ The one test in this suite that CANNOT keep its mess inside the scratch
// directory: the function under test is the one that decides where per-user
// files go, so exercising it honestly means touching the real location. It
// deletes the file it writes and leaves behind an empty directory named
// "Sol Engine Test Scratch" (%LOCALAPPDATA% on Windows, ~/.local/share as
// "sol-engine-test-scratch" on Linux). Named so it is obvious what dropped it,
// and deliberately NOT the game's own name, so a test run can never collide
// with a real save.
SOL_TEST(userDataDirectoryIsUsableAndKeepsItsSeparatorPromise)
{
    const std::string dir = sol::platform::userDataDirectory("Sol Engine Test Scratch");
    SOL_REQUIRE(!dir.empty());
    SOL_CHECK(dir.back() == '/');
    SOL_CHECK(dir.find('\\') == std::string::npos);

    // Non-empty is a promise that the directory exists, so this must not need
    // a createDirectories of its own.
    const std::string probe = dir + "probe.txt";
    SOL_REQUIRE(writeText(probe, "probe"));
    SOL_CHECK(readText(probe) == "probe");
    (void)deleteFile(probe.c_str());
}

// A name with nothing usable in it has no directory to offer, and the honest
// answer is the empty string rather than a directory named after punctuation.
SOL_TEST(userDataDirectoryRefusesANameItCannotUse)
{
    SOL_CHECK(sol::platform::userDataDirectory("").empty());
    SOL_CHECK(sol::platform::userDataDirectory(nullptr).empty());
}

// ⚑⚑⚑ THE DESTRUCTIVE DIRECTION, AND THE ONLY TEST HERE THAT GUARDS AGAINST
// DATA LOSS. copyFileIfAbsent exists to move a save and a settings file to
// their new home exactly once. Inverted - copying whenever the SOURCE exists
// rather than only when the DESTINATION does not - it would overwrite the
// live save with the stale one on every single launch, and the player would
// lose their game a little at a time while everything appeared to work.
//
// Both directions are asserted because only the pair pins the predicate: the
// first case alone passes for an unconditional copy, and the second alone
// passes for a function that never copies at all.
SOL_TEST(copyFileIfAbsentSeedsAnEmptyDestinationAndNeverOverwritesALiveOne)
{
    const std::string dir = scratchRoot() + "/migrate";
    SOL_REQUIRE(createDirectories(dir.c_str()));
    const std::string legacy = dir + "/legacy.toml";
    const std::string fresh = dir + "/fresh.toml";
    (void)deleteFile(fresh.c_str());

    // Destination absent: the content arrives, and the source stays put -
    // this is a copy, not a move, so an older build keeps working.
    SOL_REQUIRE(writeText(legacy, "effects_volume = 0.209"));
    SOL_CHECK(copyFileIfAbsent(legacy.c_str(), fresh.c_str()));
    SOL_CHECK(readText(fresh) == "effects_volume = 0.209");
    SOL_CHECK(readText(legacy) == "effects_volume = 0.209");

    // Destination present: it is left exactly as it was, even though the
    // source is still there and still different. Reporting success is correct
    // - the destination does hold content - and it is what lets the caller
    // run this on every launch without a flag of its own.
    SOL_REQUIRE(writeText(fresh, "effects_volume = 1.0"));
    SOL_CHECK(copyFileIfAbsent(legacy.c_str(), fresh.c_str()));
    SOL_CHECK(readText(fresh) == "effects_volume = 1.0");

    // No source and no destination is the ordinary first run on a clean
    // machine: nothing to migrate is a false, not a failure to report.
    const std::string missing = dir + "/absent.toml";
    const std::string target = dir + "/target.toml";
    (void)deleteFile(target.c_str());
    SOL_CHECK(!copyFileIfAbsent(missing.c_str(), target.c_str()));

    (void)deleteFile(legacy.c_str());
    (void)deleteFile(fresh.c_str());
}

// --- Phase 33 stage A: the unnamed directory --------------------------------

// ⚑⚑⚑ THE ONE THAT WAS A THIRD OF `ctest --preset dev` AND NOBODY KNEW.
// Win32 built both listings' search pattern as `directory + "\\*"`, so an unnamed
// directory became `"\*"` - the root of the current drive - and `listFiles` is
// RECURSIVE. `listFiles("")` walked the whole of C: and came back with
// 1,503,635 files in 26 seconds. Two Forge tests call
// `listMeshes(meshDirectory, /*cooked=*/"")` because a test has no cooked
// directory, and between them they were 59 s of a 171 s suite.
//
// ⚑⚑ AND THE RUNTIME IS THE CHEAP HALF. That call returned 168 mesh entries
// from a directory holding 14, the other 154 being every `.smesh` anywhere on
// the machine, and `measureModelMeshes` resolves a model's mesh stem against
// exactly that list - so a def naming a mesh this project does not ship would
// have bound to whatever file of that name sat on the developer's disk. The
// failure had no symptom: no throw, no empty result, just a confident answer
// about somewhere else.
//
// ⚑ Asserted for BOTH functions and for null as well as empty, because the
// two platforms arrived at this from opposite ends - libstdc++ set an error and
// yielded nothing, Win32 enumerated a disk - and only a test keeps them agreed.
SOL_TEST(anUnnamedDirectoryIsEmptyRatherThanTheRootOfTheDrive)
{
    SOL_CHECK(listFiles("").empty());
    SOL_CHECK(listDirectories("").empty());
    SOL_CHECK(listFiles(nullptr).empty());
    SOL_CHECK(listDirectories(nullptr).empty());

    // The contrast that makes the above a rule rather than a coincidence: a
    // named directory that is really there still answers with its contents.
    const std::string dir = scratchRoot() + "/unnamed";
    (void)deleteDirectory(dir.c_str());
    SOL_REQUIRE(createDirectories((dir + "/child").c_str()));
    SOL_REQUIRE(writeText(dir + "/there.txt", "x"));
    SOL_CHECK(listFiles(dir.c_str()).size() == 1);
    SOL_CHECK(listDirectories(dir.c_str()).size() == 1);

    (void)deleteDirectory(dir.c_str());
}

// --- Phase 27: the directory half of this surface ---------------------------

SOL_TEST(listDirectoriesSeesAnEmptyDirectoryThatListFilesCannot)
{
    // ⚑⚑ THE WHOLE REASON THIS FUNCTION EXISTS, ASSERTED AS A CONTRAST.
    // listFiles is recursive and files-only, so a directory holding nothing is
    // indistinguishable from one that is not there - which is exactly the trap
    // game/mods/ ships an empty-but-present directory to work around. A save
    // campaign whose every save has been deleted is still a campaign, and the
    // browser has to keep showing it.
    const std::string dir = scratchRoot() + "/dirs";
    const std::string empty = dir + "/empty-campaign";
    const std::string full = dir + "/full-campaign";
    (void)deleteDirectory(dir.c_str());
    SOL_REQUIRE(createDirectories(empty.c_str()));
    SOL_REQUIRE(createDirectories(full.c_str()));
    SOL_REQUIRE(writeText(full + "/a.sav", "x"));

    const std::vector<std::string> files = listFiles(dir.c_str());
    SOL_CHECK(files.size() == 1); // the empty one contributes nothing at all

    const std::vector<std::string> dirs = listDirectories(dir.c_str());
    const auto has = [&dirs](const char* needle) {
        return std::any_of(dirs.begin(), dirs.end(), [needle](const std::string& path) {
            return path.find(needle) != std::string::npos;
        });
    };
    SOL_CHECK(dirs.size() == 2);
    SOL_CHECK(has("empty-campaign"));
    SOL_CHECK(has("full-campaign"));

    (void)deleteDirectory(dir.c_str());
}

SOL_TEST(listDirectoriesIsOneLevelDeepAndPrefixedLikeListFiles)
{
    // Two promises copied from listFiles, because a caller that prefix-matches
    // one result against the other depends on them agreeing - the defect Phase
    // 22 shipped, which produced a mod layer named "C:".
    const std::string dir = scratchRoot() + "/dirs_shallow";
    const std::string child = dir + "/child";
    const std::string grandchild = child + "/grandchild";
    (void)deleteDirectory(dir.c_str());
    SOL_REQUIRE(createDirectories(grandchild.c_str()));

    const std::vector<std::string> dirs = listDirectories(dir.c_str());
    SOL_REQUIRE(dirs.size() == 1); // grandchild is NOT reported: one level only
    SOL_CHECK(dirs[0] == dir + "/child");
    SOL_CHECK(dirs[0].find('\\') == std::string::npos); // '/' on every platform

    // A missing directory is empty, not an error - as listFiles.
    SOL_CHECK(listDirectories((dir + "/nope").c_str()).empty());

    (void)deleteDirectory(dir.c_str());
}

SOL_TEST(deleteDirectoryTakesTheWholeTreeAndIsIdempotent)
{
    const std::string dir = scratchRoot() + "/delete_tree";
    const std::string nested = dir + "/nested/deeper";
    SOL_REQUIRE(createDirectories(nested.c_str()));
    SOL_REQUIRE(writeText(dir + "/top.sav", "t"));
    SOL_REQUIRE(writeText(nested + "/deep.sav", "d"));

    SOL_CHECK(deleteDirectory(dir.c_str()));
    SOL_CHECK(!exists(dir + "/top.sav"));
    SOL_CHECK(!exists(nested + "/deep.sav"));
    SOL_CHECK(listDirectories(dir.c_str()).empty());

    // Already gone counts as deleted, exactly as deleteFile promises.
    SOL_CHECK(deleteDirectory(dir.c_str()));
}

SOL_TEST(deleteDirectoryRefusesAFileAndLeavesItAlone)
{
    // ⚑⚑ THE GUARD THAT MAKES THIS FUNCTION SAFE TO CALL FROM A UI. The save
    // browser deletes a campaign by path; a bug that handed it a save FILE
    // instead of the campaign DIRECTORY must delete nothing rather than
    // quietly removing the file. std::filesystem::remove_all would take it.
    const std::string dir = scratchRoot() + "/delete_guard";
    SOL_REQUIRE(createDirectories(dir.c_str()));
    const std::string file = dir + "/not_a_directory.sav";
    SOL_REQUIRE(writeText(file, "precious"));

    SOL_CHECK(!deleteDirectory(file.c_str()));
    SOL_CHECK(readText(file) == "precious"); // untouched, which is the point

    (void)deleteDirectory(dir.c_str());
}

// --- Phase 27: the wall clock ----------------------------------------------

SOL_TEST(wallClockSecondsIsARealDateAndNotTheMonotonicClock)
{
    // No golden value is possible - the answer changes every second - so this
    // pins the two things that are true forever: it is far past the epoch, and
    // it is not the same quantity as timeSeconds(). The second half is the one
    // worth having: both are "seconds", both are plausible-looking numbers,
    // and swapping them compiles silently.
    const std::uint64_t now = sol::platform::wallClockSeconds();

    // 2020-01-01, comfortably behind any machine that can build this, and
    // 2100-01-01, comfortably ahead of one. A clock reading zero, or reading
    // an uptime, fails the first.
    SOL_CHECK(now > 1'577'836'800ull);
    SOL_CHECK(now < 4'102'444'800ull);

    // ⚑⚑ THE HALF WORTH HAVING: timeSeconds() must not read as a wall clock.
    // Both functions return "seconds", both return plausible-looking numbers,
    // and pointing one at the other's clock compiles silently - so the bound
    // is the same 2020 line used above, and the claim is "this is not a date".
    //
    // ⚑ The first draft asserted `< 1'000'000.0` on the theory that
    // timeSeconds counts from process start. That is true on LINUX, where the
    // backend subtracts a static epoch captured on first call - and false on
    // WINDOWS, where QueryPerformanceCounter counts from system BOOT and is
    // returned unshifted. It failed on a machine with 16 days of uptime
    // (1,417,016 s) and would have passed on the other platform, which is the
    // worst way for an assumption to be wrong. "Arbitrary fixed epoch" in
    // time.hpp is exactly as loose as it says it is; do not read more into it.
    SOL_CHECK(sol::platform::timeSeconds() < 1'577'836'800.0);
}

SOL_TEST(localCalendarTimeBreaksDownAStampAndReportsItsOwnFailure)
{
    // A known instant, checked in a way no time zone can break. The zone can
    // shift the hour and even the calendar day, so the DATE is asserted as a
    // range of one day either side of the UTC answer rather than as an
    // equality that would fail for whoever runs this in Auckland.
    sol::platform::CalendarTime broken;
    SOL_REQUIRE(sol::platform::localCalendarTime(1'700'000'000ull, broken)); // 2023-11-14T22:13:20Z
    SOL_CHECK(broken.year == 2023);
    SOL_CHECK(broken.month == 11);
    SOL_CHECK(broken.day >= 13 && broken.day <= 15);
    SOL_CHECK(broken.hour >= 0 && broken.hour <= 23);
    SOL_CHECK(broken.minute == 13); // no real zone offsets by a fraction of ten minutes
    SOL_CHECK(broken.second == 20);

    // The struct is 1-based where tm is 0-based, and full-year where tm counts
    // from 1900. Both conversions are one line each and both have exactly one
    // chance to be wrong, so both are pinned above rather than assumed.
    const sol::platform::CalendarTime untouched;
    sol::platform::CalendarTime out;
    // ⚑⚑ THIS ASSERTION WAS TRUE BY ACCIDENT UNTIL PHASE 33 STAGE A RAN IT ON
    // LINUX. `~0ull` is not a huge time, it is -1 once the platform's SIGNED
    // `time_t` has it: Win32 refused it and glibc answered 1969-12-31. The
    // implementations now both reject anything that will not survive the trip,
    // because an UNSIGNED seconds-since-epoch can never mean a pre-epoch instant.
    SOL_CHECK(!sol::platform::localCalendarTime(~0ull, out)); // wraps negative
    SOL_CHECK(out.year == untouched.year);                    // left alone, as promised
    // The other end, which the C library rejects rather than the guard: a year
    // no `tm_year` can hold. Both refusals have to leave `out` alone.
    SOL_CHECK(!sol::platform::localCalendarTime(1ull << 62, out));
    SOL_CHECK(out.year == untouched.year);
}
