// Linux half of the non-windowing platform surface (Phase 21): the clock, the
// sleep, and every file operation `file_io.hpp` declares but `file_io.cpp` does
// not implement. Its Win32 counterpart is `win32/system_win32.cpp`, and the
// contracts below are copied from that file's behaviour rather than from its
// signatures - several of them are only obvious once you read the Win32 code.

#include "sol/platform/file_io.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sol::platform {

const char* platformName()
{
    return "Linux (Wayland)";
}

double timeSeconds()
{
    // steady_clock rather than system_clock: `timeSeconds` is documented as
    // monotonic since an arbitrary epoch, and the frame loop subtracts two of
    // them. A wall clock stepping backwards over an NTP correction would hand
    // the simulation a negative delta.
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point epoch = Clock::now();
    return std::chrono::duration<double>(Clock::now() - epoch).count();
}

void sleepMilliseconds(std::uint32_t milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

std::uint64_t fileModificationTime(const char* path)
{
    // The VALUE is meaningless across platforms and nothing compares it to a
    // wall clock - every caller only asks "is this different from last time".
    // But ZERO IS A SENTINEL: `cooker/src/mesh.cpp:107` reads 0 as "the file is
    // not there" to decide whether to sweep a stale LOD, and the hot-reload
    // watchers read it as "nothing to load yet". A real file must therefore
    // never, ever produce 0.
    //
    // ⚑⚑ THAT IS WHY THIS USES stat() AND NOT std::filesystem::last_write_time.
    // libstdc++'s file_clock has its epoch at 2174-01-01, so time_since_epoch()
    // is NEGATIVE for every file that exists today. The obvious defensive
    // clamp - `ns > 0 ? ns : 0` - therefore reports every real file as missing,
    // which is exactly the bug the first Linux run of `cooker.unit` caught: the
    // stray-level sweep skipped every file it was supposed to delete. st_mtim
    // is plain seconds and nanoseconds since 1970 and is positive for anything
    // a cook could have produced.
    struct stat info = {};
    if (::stat(path, &info) != 0) {
        return 0; // no file, which is the sentinel every caller expects
    }
    const auto seconds = static_cast<std::uint64_t>(info.st_mtim.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(info.st_mtim.tv_nsec);
    const std::uint64_t stamp = seconds * 1000000000ull + nanoseconds;
    // A file stamped exactly at the epoch is not missing. Costs one compare and
    // removes the last way a real file can collide with the sentinel.
    return stamp != 0 ? stamp : 1;
}

std::vector<std::string> listFiles(const char* directory)
{
    // ⚑ RECURSIVE, and that is load-bearing rather than incidental: stage L
    // put the Blender inbox outside `assets/` because of it, and stage R's
    // `forgeIsPendingDrop` exists to filter what this returns. Files only -
    // directories are descended into, never listed.
    //
    // Paths come back with FORWARD slashes and the caller's own prefix, which
    // costs nothing here and is what the Win32 side goes out of its way to
    // produce. `generic_string` keeps that true if a caller ever passes a path
    // with a backslash in it.
    std::vector<std::string> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(directory, error);
    if (error) {
        return files; // a missing directory is empty, not an error - as Win32
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(error)) {
        if (error) {
            break;
        }
        if (it->is_directory(error) || error) {
            continue;
        }
        files.push_back(it->path().generic_string());
    }
    return files;
}

bool deleteFile(const char* path)
{
    // Idempotent by contract: a file that was already gone counts as deleted.
    // `remove` returns false for both "did not exist" and "could not remove",
    // so the error code is the only thing that separates them.
    std::error_code error;
    if (std::filesystem::remove(path, error)) {
        return true;
    }
    return !error;
}

bool moveFile(const char* fromPath, const char* toPath)
{
    // Mirrors MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED. POSIX rename
    // already replaces an existing destination, so the only Win32 behaviour
    // that needs building by hand is COPY_ALLOWED: rename cannot cross a
    // filesystem boundary and fails with EXDEV, which is reachable here for
    // real (a repo on /mnt/c and a build under $HOME are different mounts).
    //
    // ⚑ It must NOT create the destination directory - `platform.unit` pins
    // that, because stage R's archive move depends on a missing directory
    // being a failure rather than a silent success somewhere unexpected.
    std::error_code error;
    std::filesystem::rename(fromPath, toPath, error);
    if (!error) {
        return true;
    }
    if (error != std::errc::cross_device_link) {
        return false;
    }
    std::filesystem::copy_file(fromPath, toPath, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return false;
    }
    std::error_code removeError;
    std::filesystem::remove(fromPath, removeError);
    return true; // the content arrived; a stranded source is not a failed move
}

bool createDirectories(const char* path)
{
    // Returns "is there a directory here now", not "did I create one" - an
    // existing directory is a success on both platforms, and `create_directories`
    // reports false for that case.
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return std::filesystem::is_directory(path, error);
}

int runProcess(const char* commandLine)
{
    // -1 for "could not start it at all", matching the Win32 CreateProcess
    // failure path; otherwise the child's exit status. `system` runs it through
    // a shell, which is the closest thing to CreateProcessW taking one already
    // -joined command line - the callers here build shader-compiler command
    // lines as single strings, so re-splitting them would be the lossy option.
    if (commandLine == nullptr) {
        return -1;
    }
    const int status = std::system(commandLine);
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1; // killed by a signal: not an exit code, so do not invent one
}

std::string executableDirectory()
{
    // ⚑ KEEPS THE TRAILING SEPARATOR. Every caller concatenates directly onto
    // it (`executableDir + "settings.toml"`), so dropping it silently moves
    // the save file, the settings and the cooked directory one level up.
    std::error_code error;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return "./";
    }
    return exe.parent_path().generic_string() + "/";
}

namespace {

// "The Stars Don't Wait" -> "the-stars-dont-wait".
//
// ⚑ The apostrophe is DROPPED, not turned into a separator. Mapping every
// non-alphanumeric to '-' would give "the-stars-don-t-wait", which is the
// obvious implementation and the wrong answer: a word is not split by the
// punctuation inside it. So alphanumerics are kept and lowercased, whitespace
// (and '-'/'_', already separators) opens a new word, and everything else is
// discarded. Runs collapse and the ends are trimmed, so no input can produce
// a leading, trailing or doubled '-'.
std::string slugify(const char* name)
{
    std::string slug;
    bool pendingSeparator = false;
    for (const char* c = name; *c != '\0'; ++c) {
        const unsigned char ch = static_cast<unsigned char>(*c);
        if (std::isalnum(ch) != 0) {
            if (pendingSeparator && !slug.empty()) {
                slug += '-';
            }
            pendingSeparator = false;
            slug += static_cast<char>(std::tolower(ch));
        } else if (std::isspace(ch) != 0 || ch == '-' || ch == '_') {
            pendingSeparator = true;
        }
        // anything else: dropped, and it does NOT open a new word
    }
    return slug;
}

} // namespace

std::string userDataDirectory(const char* appName)
{
    if (appName == nullptr) {
        return {};
    }
    const std::string slug = slugify(appName);
    if (slug.empty()) {
        return {}; // a name with no alphanumerics has no directory to offer
    }

    std::string base;
    const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
    // ⚑ The XDG spec says a RELATIVE XDG_DATA_HOME is invalid and must be
    // ignored rather than resolved against the working directory - which for a
    // game launched from a desktop file is somewhere nobody intended.
    if (xdgDataHome != nullptr && xdgDataHome[0] == '/') {
        base = xdgDataHome;
    } else {
        const char* home = std::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            return {};
        }
        base = std::string(home) + "/.local/share";
    }

    const std::string directory = base + "/" + slug + "/";
    if (!createDirectories(directory.c_str())) {
        return {}; // empty means unusable, and the caller decides the policy
    }
    return directory;
}

} // namespace sol::platform
