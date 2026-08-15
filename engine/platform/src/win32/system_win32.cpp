#include "sol/platform/file_io.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace sol::platform {

const char* platformName()
{
    return "Windows (Win32)";
}

double timeSeconds()
{
    static const double secondsPerTick = [] {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        return 1.0 / static_cast<double>(frequency.QuadPart);
    }();

    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * secondsPerTick;
}

void sleepMilliseconds(std::uint32_t milliseconds)
{
    Sleep(milliseconds);
}

std::string executableDirectory()
{
    wchar_t widePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, widePath, MAX_PATH);

    // Trim the executable name, keep the trailing separator.
    DWORD directoryLength = length;
    while (directoryLength > 0 && widePath[directoryLength - 1] != L'\\') {
        --directoryLength;
    }

    char utf8Path[MAX_PATH * 4] = {};
    const int utf8Length = WideCharToMultiByte(
        CP_UTF8, 0, widePath, static_cast<int>(directoryLength), utf8Path, sizeof(utf8Path) - 1, nullptr, nullptr);

    return std::string(utf8Path, static_cast<std::size_t>(utf8Length));
}

} // namespace sol::platform
