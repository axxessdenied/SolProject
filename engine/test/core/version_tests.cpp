#include "sol/core/version.hpp"
#include "sol/test/test.hpp"

#include <cstdio>
#include <cstring>

SOL_TEST(engineVersionIsPreRelease)
{
    const sol::core::Version v = sol::core::engineVersion();
    SOL_CHECK(v.major == 0);
}

SOL_TEST(versionStringMatchesComponents)
{
    const sol::core::Version v = sol::core::engineVersion();

    char expected[32] = {};
    std::snprintf(expected,
                  sizeof(expected),
                  "%u.%u.%u",
                  static_cast<unsigned>(v.major),
                  static_cast<unsigned>(v.minor),
                  static_cast<unsigned>(v.patch));

    SOL_CHECK(std::strcmp(expected, sol::core::engineVersionString()) == 0);
}
