#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sol::platform {

// Reads an entire binary file; returns false and leaves outBytes empty on failure.
[[nodiscard]] bool readFileBytes(const char* path, std::vector<std::uint8_t>& outBytes);

// Writes a whole binary file, replacing any existing content.
[[nodiscard]] bool writeFileBytes(const char* path, const void* data, std::size_t size);

// Absolute directory containing the running executable, with a trailing separator.
[[nodiscard]] std::string executableDirectory();

// Last-write time as an opaque monotonic-comparable value; 0 if the file is missing.
[[nodiscard]] std::uint64_t fileModificationTime(const char* path);

// Full paths of all regular files under directory (recursive). '/' separators.
[[nodiscard]] std::vector<std::string> listFiles(const char* directory);

// Creates the directory and any missing parents; true if it exists afterwards.
[[nodiscard]] bool createDirectories(const char* path);

// Deletes a regular file; true if it no longer exists afterwards (including
// when it was already missing).
[[nodiscard]] bool deleteFile(const char* path);

// Runs a command line synchronously; returns the process exit code, or -1
// if the process could not be started.
[[nodiscard]] int runProcess(const char* commandLine);

} // namespace sol::platform
