#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnt.h>
#include <cstdio>

using PowerDeterminePlatformRoleFn = POWER_PLATFORM_ROLE (WINAPI*)();
using PowerDeterminePlatformRoleExFn = POWER_PLATFORM_ROLE (WINAPI*)(ULONG);

int wmain() {
    HMODULE module = LoadLibraryW(L"powrprof.dll");
    if (!module) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wprintf(L"Loaded powrprof: %ls\n", path);

    auto role = reinterpret_cast<PowerDeterminePlatformRoleFn>(
        GetProcAddress(module, "PowerDeterminePlatformRole"));
    auto role_ex = reinterpret_cast<PowerDeterminePlatformRoleExFn>(
        GetProcAddress(module, "PowerDeterminePlatformRoleEx"));

    if (!role || !role_ex) {
        std::printf("Missing platform role export(s)\n");
        return 2;
    }

    const auto value = role();
    const auto value_ex = role_ex(2);
    std::printf("PowerDeterminePlatformRole=%d PowerDeterminePlatformRoleEx=%d\n",
                static_cast<int>(value), static_cast<int>(value_ex));

    if (value < PlatformRoleUnspecified || value >= PlatformRoleMaximum) {
        std::printf("Invalid role value\n");
        return 3;
    }
    if (value_ex < PlatformRoleUnspecified || value_ex >= PlatformRoleMaximum) {
        std::printf("Invalid role-ex value\n");
        return 4;
    }

    return 0;
}
