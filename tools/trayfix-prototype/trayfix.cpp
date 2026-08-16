#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winnt.h>
#include <cstring>

#pragma comment(lib, "kernel32.lib")

extern "C" __declspec(dllimport) DWORD WINAPI K32GetModuleFileNameExW(
    HANDLE hProcess,
    HMODULE hModule,
    LPWSTR lpFilename,
    DWORD nSize);

using ShellNotifyIconWFn = BOOL (WINAPI*)(DWORD, PNOTIFYICONDATAW);
using ExitProcessFn = VOID (WINAPI*)(UINT);
using TerminateProcessFn = BOOL (WINAPI*)(HANDLE, UINT);

static ShellNotifyIconWFn g_shell_notify = nullptr;
static ExitProcessFn g_exit_process = nullptr;
static TerminateProcessFn g_terminate_process = nullptr;
static SRWLOCK g_tray_lock = SRWLOCK_INIT;
static NOTIFYICONDATAW g_tray_nid{};
static bool g_have_tray = false;
static HANDLE g_main_thread = nullptr;

static void CleanupTray() {
    NOTIFYICONDATAW nid{};
    ShellNotifyIconWFn notify = nullptr;
    bool should_delete = false;

    AcquireSRWLockExclusive(&g_tray_lock);
    if (g_have_tray && g_shell_notify) {
        nid = g_tray_nid;
        notify = g_shell_notify;
        g_have_tray = false;
        should_delete = true;
    }
    ReleaseSRWLockExclusive(&g_tray_lock);

    if (should_delete) {
        notify(NIM_DELETE, &nid);
    }
}

static BOOL WINAPI HookShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data) {
    if (data) {
        if (message == NIM_ADD) {
            AcquireSRWLockExclusive(&g_tray_lock);
            std::memset(&g_tray_nid, 0, sizeof(g_tray_nid));
            const SIZE_T copy_size =
                data->cbSize < sizeof(g_tray_nid) ? data->cbSize : sizeof(g_tray_nid);
            std::memcpy(&g_tray_nid, data, copy_size);
            g_have_tray = true;
            ReleaseSRWLockExclusive(&g_tray_lock);
        } else if (message == NIM_DELETE) {
            AcquireSRWLockExclusive(&g_tray_lock);
            if (g_have_tray && g_tray_nid.hWnd == data->hWnd && g_tray_nid.uID == data->uID) {
                g_have_tray = false;
            }
            ReleaseSRWLockExclusive(&g_tray_lock);
        }
    }
    return g_shell_notify ? g_shell_notify(message, data) : FALSE;
}

static VOID WINAPI HookExitProcess(UINT code) {
    CleanupTray();
    g_exit_process(code);
}

static BOOL WINAPI HookTerminateProcess(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId()) {
        CleanupTray();
    }
    return g_terminate_process(process, code);
}

static bool PatchImport(const char* dll_name, const char* function_name, void* replacement, void** original_out) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* imported_dll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(imported_dll, dll_name) != 0) continue;

        auto* orig = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
            : reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; orig->u1.AddressOfData; ++orig, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(name->Name), function_name) != 0) continue;

            DWORD old_protect = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old_protect)) return false;
            if (original_out && !*original_out) {
                *original_out = reinterpret_cast<void*>(iat->u1.Function);
            }
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), old_protect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), &iat->u1.Function, sizeof(ULONG_PTR));
            return true;
        }
    }
    return false;
}

static DWORD WINAPI MainThreadWatcher(void*) {
    if (g_main_thread) {
        WaitForSingleObject(g_main_thread, INFINITE);
        CleanupTray();
        CloseHandle(g_main_thread);
        g_main_thread = nullptr;
    }
    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI GetModuleFileNameExW(
    HANDLE process,
    HMODULE module,
    LPWSTR filename,
    DWORD size) {
    return K32GetModuleFileNameExW(process, module, filename, size);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        DuplicateHandle(
            GetCurrentProcess(), GetCurrentThread(),
            GetCurrentProcess(), &g_main_thread,
            SYNCHRONIZE, FALSE, 0);

        PatchImport("shell32.dll", "Shell_NotifyIconW",
                    reinterpret_cast<void*>(&HookShellNotifyIconW),
                    reinterpret_cast<void**>(&g_shell_notify));
        PatchImport("kernel32.dll", "ExitProcess",
                    reinterpret_cast<void*>(&HookExitProcess),
                    reinterpret_cast<void**>(&g_exit_process));
        PatchImport("kernel32.dll", "TerminateProcess",
                    reinterpret_cast<void*>(&HookTerminateProcess),
                    reinterpret_cast<void**>(&g_terminate_process));

        HANDLE watcher = CreateThread(nullptr, 0, MainThreadWatcher, nullptr, 0, nullptr);
        if (watcher) CloseHandle(watcher);
    }
    return TRUE;
}
