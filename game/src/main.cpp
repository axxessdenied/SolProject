#include "sol/core/version.hpp"
#include "sol/platform/platform.hpp"

#include <cstdio>

int main()
{
    std::printf("Sol Engine %s on %s - Phase 0 scaffold\n",
                sol::core::engineVersionString(),
                sol::platform::platformName());
    return 0;
}
