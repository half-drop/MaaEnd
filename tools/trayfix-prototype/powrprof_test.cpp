#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>

extern "C" __declspec(dllexport) NTSTATUS WINAPI CallNtPowerInformation(
    POWER_INFORMATION_LEVEL,
    PVOID,
    ULONG,
    PVOID,
    ULONG) {
    return static_cast<NTSTATUS>(0x12345678L);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
