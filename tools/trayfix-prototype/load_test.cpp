#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>
#include <iostream>

#pragma comment(lib, "psapi.lib")

int wmain() {
    wchar_t module_path[MAX_PATH]{};
    HMODULE psapi = GetModuleHandleW(L"psapi.dll");
    if (!psapi) {
        std::wcerr << L"psapi.dll not loaded\n";
        return 2;
    }
    GetModuleFileNameW(psapi, module_path, MAX_PATH);
    std::wcout << L"Loaded psapi: " << module_path << L"\n";

    wchar_t exe_path[MAX_PATH]{};
    DWORD len = GetModuleFileNameExW(GetCurrentProcess(), nullptr, exe_path, MAX_PATH);
    if (!len) {
        std::wcerr << L"GetModuleFileNameExW failed: " << GetLastError() << L"\n";
        return 3;
    }
    std::wcout << L"Current exe: " << exe_path << L"\n";
    return 0;
}
