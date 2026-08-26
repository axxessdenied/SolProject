#include <algorithm>
#include <string>
#include <vector>

#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using sol::platform::createDirectories;
using sol::platform::deleteFile;
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
