#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <iostream>

#pragma comment(lib, "PowrProf.lib")

int wmain() {
    wchar_t module_path[MAX_PATH]{};
    HMODULE mod = GetModuleHandleW(L"powrprof.dll");
    if (!mod) {
        std::wcerr << L"powrprof.dll not loaded\n";
        return 2;
    }
    GetModuleFileNameW(mod, module_path, MAX_PATH);
    std::wcout << L"Loaded powrprof: " << module_path << L"\n";

    NTSTATUS status = CallNtPowerInformation(SystemBatteryState, nullptr, 0, nullptr, 0);
    std::wcout << L"Status: 0x" << std::hex << static_cast<unsigned long>(status) << L"\n";
    return status == static_cast<NTSTATUS>(0x12345678L) ? 0 : 3;
}
