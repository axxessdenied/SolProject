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

    return std::string(utf8Path, static_cast<std::size_t>(utf8Length));
}

} // namespace sol::platform
