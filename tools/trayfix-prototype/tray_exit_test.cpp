#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "PowrProf.lib")

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Force the proxy import to be used and give its post-loader worker time
    // to install the IAT hooks before the tray icon is registered.
    CallNtPowerInformation(SystemBatteryState, nullptr, 0, nullptr, 0);
    Sleep(500);

    const wchar_t* klass = L"MaaEndTrayFixTestWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = klass;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, klass, L"trayfix-test", WS_OVERLAPPED,
        0, 0, 100, 100,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 2;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 4242;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"MaaEnd TrayFix test", ARRAYSIZE(nid.szTip));

    // The hosted runner may not have a normal interactive Explorer shell.
    // The proxy records the registration request itself, so this still tests
    // that Shell_NotifyIconW was hooked and that ExitProcess invokes cleanup.
    Shell_NotifyIconW(NIM_ADD, &nid);

    ExitProcess(0);
}
