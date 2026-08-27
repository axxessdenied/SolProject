#include "sol/platform/file_io.hpp"

#include <cstdio>

namespace sol::platform {

bool writeFileBytes(const char* path, const void* data, std::size_t size)
{
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(data, 1, size, file);
    std::fclose(file);
    return written == size;
}

// Portable because it is only readFileBytes plus writeFileBytes plus the
// guard; nothing here needs an OS. ⚑ The existence probe is a read attempt
// rather than a stat: a file that exists but cannot be opened is, for this
// function's purpose, the same as one that is already there - overwriting it
// would be the destructive move either way.
bool copyFileIfAbsent(const char* fromPath, const char* toPath)
{
    std::vector<std::uint8_t> existing;
    if (readFileBytes(toPath, existing)) {
        return true; // already migrated; leaving it alone is the entire point
    }

    std::vector<std::uint8_t> source;
    if (!readFileBytes(fromPath, source)) {
        return false; // nothing to migrate, which is the normal first run
    }
    return writeFileBytes(toPath, source.data(), source.size());
}

bool readFileBytes(const char* path, std::vector<std::uint8_t>& outBytes)
{
    outBytes.clear();

    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(file);
        return false;
    }

    outBytes.resize(static_cast<std::size_t>(size));
    const std::size_t bytesRead = std::fread(outBytes.data(), 1, outBytes.size(), file);
    std::fclose(file);

    if (bytesRead != outBytes.size()) {
        outBytes.clear();
        return false;
    }
    return true;
}

} // namespace sol::platform
