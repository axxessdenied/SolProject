#include "Sol/Proto/Harness/HostInfo.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <immintrin.h>
#include <intrin.h>

namespace sol::proto {
namespace {

constexpr unsigned kEcxOsxsaveBit = 1u << 27;
constexpr unsigned kEcxAvxBit = 1u << 28;
constexpr unsigned kEcxFmaBit = 1u << 12;
constexpr unsigned kEbxAvx2Bit = 1u << 5;
constexpr unsigned kXcr0XmmYmmMask = 0x6u;

/// Trims trailing spaces and NULs that CPUID brand strings are padded with.
std::string trimPadding(const char* text, std::size_t length) {
    std::size_t end = 0;
    for (std::size_t index = 0; index < length; ++index) {
        if (text[index] == '\0') {
            break;
        }
        if (text[index] != ' ') {
            end = index + 1;
        }
    }
    std::size_t begin = 0;
    while (begin < end && text[begin] == ' ') {
        ++begin;
    }
    return std::string(text + begin, end - begin);
}

} // namespace

CpuInfo captureCpuInfo() noexcept {
    CpuInfo info;

    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    info.logicalProcessorCount = systemInfo.dwNumberOfProcessors;

    std::array<int, 4> registers{};

    __cpuid(registers.data(), 0);
    const int highestBasicLeaf = registers[0];
    {
        std::array<char, 13> vendor{};
        std::memcpy(vendor.data() + 0, &registers[1], 4);
        std::memcpy(vendor.data() + 4, &registers[3], 4);
        std::memcpy(vendor.data() + 8, &registers[2], 4);
        info.vendor = trimPadding(vendor.data(), 12);
    }

    bool osSavesYmm = false;
    if (highestBasicLeaf >= 1) {
        __cpuid(registers.data(), 1);
        const auto ecx = static_cast<unsigned>(registers[2]);
        info.hasFma = (ecx & kEcxFmaBit) != 0;

        const bool cpuAvx = (ecx & kEcxAvxBit) != 0;
        const bool osxsave = (ecx & kEcxOsxsaveBit) != 0;
        if (osxsave) {
            // AVX state is only usable if the OS enabled XMM/YMM saving in XCR0. Reporting
            // AVX2 from CPUID alone would claim a capability the process cannot use.
            const unsigned long long xcr0 = _xgetbv(0);
            osSavesYmm = (xcr0 & kXcr0XmmYmmMask) == kXcr0XmmYmmMask;
        }
        info.hasAvx = cpuAvx && osSavesYmm;
    }

    if (highestBasicLeaf >= 7) {
        __cpuidex(registers.data(), 7, 0);
        const auto ebx = static_cast<unsigned>(registers[1]);
        info.hasAvx2 = ((ebx & kEbxAvx2Bit) != 0) && osSavesYmm;
    }

    __cpuid(registers.data(), 0x80000000);
    if (static_cast<unsigned>(registers[0]) >= 0x80000004u) {
        std::array<char, 49> brand{};
        for (unsigned leaf = 0; leaf < 3; ++leaf) {
            __cpuid(registers.data(), static_cast<int>(0x80000002u + leaf));
            std::memcpy(brand.data() + leaf * 16, registers.data(), 16);
        }
        info.brand = trimPadding(brand.data(), 48);
    }

    return info;
}

OsInfo captureOsInfo() noexcept {
    OsInfo info;

    // GetVersionEx reports a shimmed version unless the executable carries a compatibility
    // manifest. RtlGetVersion reports the real build, which is what provenance needs.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return info;
    }

    const auto rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtlGetVersion == nullptr) {
        return info;
    }

    RTL_OSVERSIONINFOW versionInfo{};
    versionInfo.dwOSVersionInfoSize = sizeof(versionInfo);
    if (rtlGetVersion(&versionInfo) != 0) {
        return info;
    }

    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%lu.%lu.%lu",
                  versionInfo.dwMajorVersion, versionInfo.dwMinorVersion,
                  versionInfo.dwBuildNumber);
    info.version = text.data();
    return info;
}

std::string captureUtcTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (::gmtime_s(&utc, &now) != 0) {
        return "unknown";
    }

    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
    return text.data();
}

} // namespace sol::proto
