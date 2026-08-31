#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sol::platform {

// Reads an entire binary file; returns false and leaves outBytes empty on failure.
[[nodiscard]] bool readFileBytes(const char* path, std::vector<std::uint8_t>& outBytes);

// Writes a whole binary file, replacing any existing content.
[[nodiscard]] bool writeFileBytes(const char* path, const void* data, std::size_t size);

// Absolute directory containing the running executable, with a trailing
// separator. '/' separators on every platform, like listFiles below.
//
// ⚑ Both halves of that sentence are load-bearing and both have cost a bug.
// Dropping the trailing separator silently moves the save file, the settings
// and the cooked directory one level up, because every caller concatenates
// straight onto it. And returning '\' on Windows silently broke the one caller
// that prefix-matches this against a listFiles result (Phase 22).
[[nodiscard]] std::string executableDirectory();

// Per-user writable directory for `appName`, created if missing, with a
// trailing '/'. Empty means unusable - the OS gave no location, or the
// directory could not be created - and the caller decides what to do about it
// rather than being handed a path that will fail on first write.
//
// ⚑ Each backend applies its own platform's convention to `appName`:
//   Windows  <%LOCALAPPDATA%>/<appName>
//   Linux    <$XDG_DATA_HOME>/<slug>, else <$HOME>/.local/share/<slug>
// (each with the trailing separator this function promises). ⚑ Written with
// forward slashes on purpose: a '\' at the end of a // comment continues it
// onto the next line, which GCC warns about and MSVC does not - so the first
// draft of these two lines silently ate the third.
// where <slug> keeps alphanumerics lowercased, treats whitespace as a word
// break joined with '-', and DISCARDS everything else. So "The Stars Don't
// Wait" reaches disk as itself on Windows and as "the-stars-dont-wait" on
// Linux, which is what each platform expects to see. ⚑ Discarding rather than
// separating is why the apostrophe does not yield "don-t".
//
// ⚑⚑ `appName` is a PARAMETER rather than a constant in each backend, and the
// reason is not style: `sol::platform` is engine, so it must not know the
// game's title (AGENTS.md 4), and the Forge links this same library - a baked
// name would silently hand a tool the game's save directory. The game passes
// the same string it already gives ContextDesc::appName and WindowDesc::title.
[[nodiscard]] std::string userDataDirectory(const char* appName);

// Copies `fromPath` to `toPath` only when nothing exists at `toPath`; true if
// `toPath` holds content afterwards, including when it already did.
//
// ⚑ The "only when absent" half is the whole point and is the destructive
// direction if inverted: this exists to migrate a file to a new home ONCE,
// and a version that copied unconditionally would overwrite a live save with
// a stale one on every launch. Copy rather than move is also deliberate - see
// engine plan Phase 22 decision 3 - so the old location keeps working.
[[nodiscard]] bool copyFileIfAbsent(const char* fromPath, const char* toPath);

// Last-write time as an opaque monotonic-comparable value; 0 if the file is missing.
[[nodiscard]] std::uint64_t fileModificationTime(const char* path);

// Full paths of all regular files under directory (recursive). '/' separators.
//
// ⚑⚑⚑ AN EMPTY OR NULL DIRECTORY NAME YIELDS AN EMPTY LIST, AND SAYING SO IS
// PHASE 33 STAGE A's, BECAUSE WIN32 USED TO ANSWER WITH THE WHOLE DRIVE. Both
// listings there built a `directory + "\\*"` pattern, so an unnamed directory
// became `"\*"` - the root of the current drive - and this one is recursive:
// measured at 1,503,635 files in 26 seconds. It never threw and never crashed,
// it simply returned a plausible answer to a question about somewhere else,
// which is why it sat undetected in two Forge tests worth a third of the whole
// `ctest --preset dev` runtime. Passing "" for "I have no such directory" is a
// reasonable thing for a caller to do and both platforms now honour it.
[[nodiscard]] std::vector<std::string> listFiles(const char* directory);

// Full paths of the IMMEDIATE subdirectories of `directory`. '/' separators,
// prefixed by `directory` exactly as given, like listFiles. A missing
// directory yields an empty list rather than an error, also like listFiles.
//
// ⚑⚑ TWO DELIBERATE DIFFERENCES FROM `listFiles`, AND BOTH ARE THE POINT.
// That one is RECURSIVE and returns FILES ONLY, descending into directories
// without ever naming them - so an EMPTY directory is invisible to it. This
// one is one level deep and returns directories, which is what lets a caller
// see a folder that has nothing in it yet. `game/mods/` documents the same
// trap from the other side: it ships empty-but-present precisely because
// listFiles cannot tell a missing directory from an empty one.
//
// ⚑ The unnamed-directory rule above applies here too, for the same reason and
// with the same fix - one level deep made it far less expensive to get wrong,
// not any less wrong.
[[nodiscard]] std::vector<std::string> listDirectories(const char* directory);

// Creates the directory and any missing parents; true if it exists afterwards.
[[nodiscard]] bool createDirectories(const char* path);

// Deletes a directory and everything under it; true if it no longer exists
// afterwards (including when it was already missing), matching deleteFile's
// idempotence.
//
// ⚑⚑ THE MOST DESTRUCTIVE FUNCTION IN THIS HEADER, AND THE ONLY RECURSIVE
// ONE. It removes files the caller never named. Every caller owes the same
// two things: the path must be one this program built, and it must be checked
// for emptiness of MEANING before the call, not after - there is no undo and
// no confirmation inside here. It refuses a path that is not a directory, so
// a caller that passes a file by mistake deletes nothing.
[[nodiscard]] bool deleteDirectory(const char* path);

// Deletes a regular file; true if it no longer exists afterwards (including
// when it was already missing).
[[nodiscard]] bool deleteFile(const char* path);

// Moves a regular file, replacing any file already at the destination. Parent
// directories of the destination are not created. Prefer this over
// read-write-delete: a move is one operation that either happened or did not,
// and a half-written copy beside a deleted original is worse than either.
[[nodiscard]] bool moveFile(const char* fromPath, const char* toPath);

// Runs a command line synchronously; returns the process exit code, or -1
// if the process could not be started.
[[nodiscard]] int runProcess(const char* commandLine);

} // namespace sol::platform
