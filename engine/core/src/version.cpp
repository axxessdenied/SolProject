#include "sol/core/version.hpp"

#include <cstdio>

namespace sol::core {

Version engineVersion()
{
    return Version{SOL_VERSION_MAJOR, SOL_VERSION_MINOR, SOL_VERSION_PATCH};
}

const char* engineVersionString()
{
    static char buffer[32] = {};
    if (buffer[0] == '\0') {
        const Version v = engineVersion();
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%u.%u.%u",
                      static_cast<unsigned>(v.major),
                      static_cast<unsigned>(v.minor),
                      static_cast<unsigned>(v.patch));
    }
    return buffer;
}

} // namespace sol::core
