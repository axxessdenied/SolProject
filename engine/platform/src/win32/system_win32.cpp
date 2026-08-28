#include "sol/platform/file_io.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"

#include <ctime>

// ⚑ clang-format off, and it is load-bearing: `IncludeBlocks: Regroup` sorts
// across blank lines, and <shlobj.h> sorts ABOVE <windows.h> alphabetically -
// which does not compile. Three other files in this repo carry the same guard
// for the same reason.
// clang-format off
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
// clang-format on

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

std::uint64_t wallClockSeconds()
{
    // FILETIME is 100 ns ticks since 1601-01-01; the Unix epoch is a fixed
    // 11,644,473,600 seconds later. Doing the subtraction in SECONDS rather
    // than in ticks keeps the intermediate inside a comfortable range and
    // makes the constant one a reader can check against a calendar.
    FILETIME fileTime = {};
    GetSystemTimeAsFileTime(&fileTime);
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
    constexpr std::uint64_t kTicksPerSecond = 10'000'000ull;
    constexpr std::uint64_t kSecondsFrom1601To1970 = 11'644'473'600ull;
    const std::uint64_t seconds = ticks / kTicksPerSecond;
    return seconds > kSecondsFrom1601To1970 ? seconds - kSecondsFrom1601To1970 : 0;
}

bool localCalendarTime(std::uint64_t unixSeconds, CalendarTime& out)
{
    // _localtime64_s over the CRT's own 64-bit time_t, so this does not stop
    // working in 2038 the way the 32-bit variant would.
    const __time64_t stamp = static_cast<__time64_t>(unixSeconds);
    struct tm broken = {};
    if (_localtime64_s(&broken, &stamp) != 0) {
        return false; // out is untouched, as the header promises
    }
    out.year = broken.tm_year + 1900; // tm counts from 1900 and months from 0;
    out.month = broken.tm_mon + 1;    // the struct this fills does neither.
    out.day = broken.tm_mday;
    out.hour = broken.tm_hour;
    out.minute = broken.tm_min;
    out.second = broken.tm_sec;
    return true;
}

void sleepMilliseconds(std::uint32_t milliseconds)
{
    Sleep(milliseconds);
}

namespace {

std::wstring utf8ToWide(const char* utf8)
{
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wideLength > 0 ? wideLength - 1 : 0), L'\0');
    if (wideLength > 1) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), wideLength);
    }
    return wide;
}

std::string wideToUtf8(const wchar_t* wide, int length)
{
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    if (utf8Length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8.data(), utf8Length, nullptr, nullptr);
    }
    return utf8;
}

void listFilesRecursive(const std::wstring& directory, std::vector<std::string>& out)
{
    WIN32_FIND_DATAW findData = {};
    HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring fullPath = directory + L"\\" + name;
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            listFilesRecursive(fullPath, out);
        } else {
            std::string utf8 = wideToUtf8(fullPath.c_str(), static_cast<int>(fullPath.size()));
            for (char& c : utf8) {
                if (c == '\\') {
                    c = '/';
                }
            }
            out.push_back(std::move(utf8));
        }
    } while (FindNextFileW(find, &findData) != 0);
    FindClose(find);
}

} // namespace

std::uint64_t fileModificationTime(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExW(utf8ToWide(path).c_str(), GetFileExInfoStandard, &attributes) == 0) {
        return 0;
    }
    return (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
           attributes.ftLastWriteTime.dwLowDateTime;
}

std::vector<std::string> listFiles(const char* directory)
{
    std::vector<std::string> files;
    listFilesRecursive(utf8ToWide(directory), files);
    return files;
}

std::vector<std::string> listDirectories(const char* directory)
{
    std::vector<std::string> directories;
    const std::wstring wideDirectory = utf8ToWide(directory);
    WIN32_FIND_DATAW findData = {};
    HANDLE find = FindFirstFileW((wideDirectory + L"\\*").c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return directories; // a missing directory is empty, not an error
    }
    do {
        const std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        // The caller's own prefix, forward-slashed, exactly as listFiles
        // promises - a caller that prefix-matches one against the other is
        // relying on the two agreeing (Phase 22 found that out the hard way).
        std::string full = std::string(directory);
        if (!full.empty() && full.back() != '/' && full.back() != '\\') {
            full.push_back('/');
        }
        full += wideToUtf8(name.c_str(), static_cast<int>(name.size()));
        for (char& c : full) {
            if (c == '\\') {
                c = '/';
            }
        }
        directories.push_back(std::move(full));
    } while (FindNextFileW(find, &findData) != 0);
    FindClose(find);
    return directories;
}

bool deleteDirectory(const char* path)
{
    const std::wstring widePath = utf8ToWide(path);
    const DWORD attributes = GetFileAttributesW(widePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true; // already gone, which is the contract
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false; // a file is not a directory; delete nothing and say so
    }
    // Depth first: a directory cannot be removed until it is empty, and
    // RemoveDirectoryW does not recurse.
    WIN32_FIND_DATAW findData = {};
    HANDLE find = FindFirstFileW((widePath + L"\\*").c_str(), &findData);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = findData.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = widePath + L"\\" + name;
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                (void)deleteDirectory(wideToUtf8(child.c_str(), static_cast<int>(child.size())).c_str());
            } else {
                // Clear read-only first: a file that refuses to go would
                // otherwise leave the parent un-removable with no explanation.
                if ((findData.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    SetFileAttributesW(child.c_str(), findData.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &findData) != 0);
        FindClose(find);
    }
    RemoveDirectoryW(widePath.c_str());
    return GetFileAttributesW(widePath.c_str()) == INVALID_FILE_ATTRIBUTES;
}

bool deleteFile(const char* path)
{
    const std::wstring widePath = utf8ToWide(path);
    if (DeleteFileW(widePath.c_str()) != 0) {
        return true;
    }
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool moveFile(const char* fromPath, const char* toPath)
{
    // MOVEFILE_REPLACE_EXISTING so a re-send overwrites its own previous
    // archive rather than failing; COPY_ALLOWED because the destination can sit
    // on another volume, which a bare rename cannot cross.
    return MoveFileExW(utf8ToWide(fromPath).c_str(),
                       utf8ToWide(toPath).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

bool createDirectories(const char* path)
{
    const std::wstring widePath = utf8ToWide(path);
    std::wstring partial;
    partial.reserve(widePath.size());
    for (std::size_t i = 0; i <= widePath.size(); ++i) {
        const wchar_t c = (i < widePath.size()) ? widePath[i] : L'\0';
        if (c == L'\\' || c == L'/' || c == L'\0') {
            if (!partial.empty() && partial.back() != L':') {
                CreateDirectoryW(partial.c_str(), nullptr);
            }
        }
        if (c != L'\0') {
            partial.push_back(c == L'/' ? L'\\' : c);
        }
    }
    const DWORD attributes = GetFileAttributesW(widePath.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int runProcess(const char* commandLine)
{
    std::wstring wideCommand = utf8ToWide(commandLine); // mutable buffer required by CreateProcessW

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};

    if (CreateProcessW(nullptr,
                       wideCommand.data(),
                       nullptr,
                       nullptr,
                       FALSE,
                       CREATE_NO_WINDOW,
                       nullptr,
                       nullptr,
                       &startupInfo,
                       &processInfo) == 0) {
        return -1;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return static_cast<int>(exitCode);
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
    const int utf8Length = WideCharToMultiByte(CP_UTF8,
                                               0,
                                               widePath,
                                               static_cast<int>(directoryLength),
                                               utf8Path,
                                               sizeof(utf8Path) - 1,
                                               nullptr,
                                               nullptr);

    std::string path(utf8Path, static_cast<std::size_t>(utf8Length));
    // ⚑⚑ Phase 22. NORMALISED TO '/', to match what listFiles has always
    // promised in this same header. Win32 file APIs and the CRT accept either,
    // so this costs nothing - but a CALLER that combines the two functions is
    // silently broken when they disagree, and one was: GameContent::initialize
    // strips `modsDirectory + "/"` off each listFiles result to find the mod
    // name, and with a backslash directory the prefix never matched, so the
    // name came out as everything up to the first '/' - the string "C:".
    //
    // It survived because it needs BOTH a shipping-layout path (dev builds bake
    // a CMake path, which is already '/') AND a mods directory that exists,
    // and game/mods did not exist until this phase. The Forge had already met
    // the same mismatch and worked around it locally (`normalisedPath` in
    // mesh_library.cpp, whose comment states the cause exactly). Fixing the
    // source rather than adding a second workaround is the point.
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    return path;
}

std::string userDataDirectory(const char* appName)
{
    if (appName == nullptr || appName[0] == '\0') {
        return {};
    }

    // ⚑ SHGetKnownFolderPath rather than _wgetenv("LOCALAPPDATA"), even though
    // the env var is simpler and set in every interactive session: this decides
    // where a player's save lives, and the known-folder API is the one that
    // honours folder redirection and survives a scrubbed environment block.
    // shell32 has exactly the standing ole32 already has here (AGENTS.md 5).
    PWSTR wideBase = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wideBase))) {
        CoTaskMemFree(wideBase); // documented as required even on failure
        return {};
    }

    char utf8Base[MAX_PATH * 4] = {};
    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, 0, wideBase, -1, utf8Base, sizeof(utf8Base) - 1, nullptr, nullptr);
    CoTaskMemFree(wideBase);
    if (utf8Length <= 1) { // 1 would be the terminator alone
        return {};
    }

    // utf8Length counts the terminator because the input was -1 terminated.
    std::string directory(utf8Base, static_cast<std::size_t>(utf8Length - 1));
    for (char& c : directory) {
        if (c == '\\') {
            c = '/'; // same promise executableDirectory makes, for the same reason
        }
    }
    // The name goes on verbatim: Windows convention is the display name, and
    // the apostrophe in "The Stars Don't Wait" is perfectly legal on NTFS. It
    // IS a quoting hazard for any shell or script that later touches the path.
    directory += '/';
    directory += appName;
    directory += '/';

    if (!createDirectories(directory.c_str())) {
        return {}; // empty means unusable, and the caller decides the policy
    }
    return directory;
}

} // namespace sol::platform
