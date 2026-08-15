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
