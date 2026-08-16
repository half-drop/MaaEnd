#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <iostream>
#include <array>
#include <cstring>

#pragma comment(lib, "PowrProf.lib")

using NtPowerInformationFn = LONG (WINAPI*)(
    POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG);

int wmain() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto native = ntdll
        ? reinterpret_cast<NtPowerInformationFn>(
              GetProcAddress(ntdll, "NtPowerInformation"))
        : nullptr;
    if (!native) {
        std::wcerr << L"NtPowerInformation not found\n";
        return 2;
    }

    const POWER_INFORMATION_LEVEL levels[] = {
        SystemBatteryState,
        LastWakeTime,
        LastSleepTime,
        ProcessorInformation,
    };

    for (auto level : levels) {
        std::array<unsigned char, 16384> via_powrprof{};
        std::array<unsigned char, 16384> via_ntdll{};

        const LONG a = CallNtPowerInformation(
            level, nullptr, 0,
            via_powrprof.data(), static_cast<ULONG>(via_powrprof.size()));
        const LONG b = native(
            level, nullptr, 0,
            via_ntdll.data(), static_cast<ULONG>(via_ntdll.size()));

        std::wcout << L"level=" << static_cast<int>(level)
                   << L" powrprof=0x" << std::hex << static_cast<unsigned long>(a)
                   << L" ntdll=0x" << static_cast<unsigned long>(b)
                   << std::dec << L"\n";

        if (a != b) {
            std::wcerr << L"status mismatch at level "
                       << static_cast<int>(level) << L"\n";
            return 3;
        }
    }

    return 0;
}
