#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winnt.h>
#include <cstring>

using ShellNotifyIconWFn = BOOL (WINAPI*)(DWORD, PNOTIFYICONDATAW);
using ExitProcessFn = VOID (WINAPI*)(UINT);
using TerminateProcessFn = BOOL (WINAPI*)(HANDLE, UINT);
using NtPowerInformationFn = LONG (WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);

static ShellNotifyIconWFn g_shell_notify = nullptr;
static ExitProcessFn g_exit_process = nullptr;
static TerminateProcessFn g_terminate_process = nullptr;
static NtPowerInformationFn g_nt_power_information = nullptr;

static SRWLOCK g_tray_lock = SRWLOCK_INIT;
static NOTIFYICONDATAW g_tray_nid{};
static bool g_have_tray = false;
static HANDLE g_main_thread = nullptr;

static void WriteTestMarker(const wchar_t* state) {
    wchar_t marker_path[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(
        L"MAAEND_TRAYFIX_TEST_MARKER", marker_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    HANDLE file = CreateFileW(
        marker_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(file, state,
              static_cast<DWORD>(wcslen(state) * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(file);
}

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
        WriteTestMarker(L"tray-cleanup-called");
    }
}

static BOOL WINAPI HookShellNotifyIconW(DWORD message, PNOTIFYICONDATAW data) {
    if (!g_shell_notify) return FALSE;

    const BOOL result = g_shell_notify(message, data);

    if (data && message == NIM_ADD) {
        AcquireSRWLockExclusive(&g_tray_lock);
        std::memset(&g_tray_nid, 0, sizeof(g_tray_nid));
        const SIZE_T copy_size =
            data->cbSize < sizeof(g_tray_nid) ? data->cbSize : sizeof(g_tray_nid);
        std::memcpy(&g_tray_nid, data, copy_size);
        g_have_tray = true;
        ReleaseSRWLockExclusive(&g_tray_lock);
    } else if (data && message == NIM_DELETE) {
        AcquireSRWLockExclusive(&g_tray_lock);
        if (g_have_tray &&
            g_tray_nid.hWnd == data->hWnd &&
            g_tray_nid.uID == data->uID) {
            g_have_tray = false;
        }
        ReleaseSRWLockExclusive(&g_tray_lock);
    }

    return result;
}

static VOID WINAPI HookExitProcess(UINT code) {
    CleanupTray();
    if (g_exit_process) {
        g_exit_process(code);
    }

    // Should be unreachable, but keep a hard fallback if the original IAT
    // pointer was unavailable for an unexpected binary layout.
    auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto fallback = kernel32
        ? reinterpret_cast<ExitProcessFn>(GetProcAddress(kernel32, "ExitProcess"))
        : nullptr;
    if (fallback) fallback(code);
    for (;;) Sleep(INFINITE);
}

static BOOL WINAPI HookTerminateProcess(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() ||
        (process && GetProcessId(process) == GetCurrentProcessId())) {
        CleanupTray();
    }

    if (g_terminate_process) {
        return g_terminate_process(process, code);
    }

    auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto fallback = kernel32
        ? reinterpret_cast<TerminateProcessFn>(
              GetProcAddress(kernel32, "TerminateProcess"))
        : nullptr;
    return fallback ? fallback(process, code) : FALSE;
}

static bool PatchImport(
    const char* dll_name,
    const char* function_name,
    void* replacement,
    void** original_out) {

    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + dir.VirtualAddress);

    for (; desc->Name; ++desc) {
        const char* imported_dll =
            reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(imported_dll, dll_name) != 0) continue;

        auto* orig = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                  base + desc->OriginalFirstThunk)
            : reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + desc->FirstThunk);

        for (; orig->u1.AddressOfData; ++orig, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;

            auto* import_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + orig->u1.AddressOfData);
            if (std::strcmp(
                    reinterpret_cast<const char*>(import_name->Name),
                    function_name) != 0) {
                continue;
            }

            DWORD old_protect = 0;
            if (!VirtualProtect(
                    &iat->u1.Function,
                    sizeof(ULONG_PTR),
                    PAGE_READWRITE,
                    &old_protect)) {
                return false;
            }

            if (original_out && !*original_out) {
                *original_out = reinterpret_cast<void*>(iat->u1.Function);
            }
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);

            DWORD ignored = 0;
            VirtualProtect(
                &iat->u1.Function,
                sizeof(ULONG_PTR),
                old_protect,
                &ignored);
            FlushInstructionCache(
                GetCurrentProcess(),
                &iat->u1.Function,
                sizeof(ULONG_PTR));
            return true;
        }
    }

    return false;
}

static void InstallHooks() {
    PatchImport(
        "shell32.dll", "Shell_NotifyIconW",
        reinterpret_cast<void*>(&HookShellNotifyIconW),
        reinterpret_cast<void**>(&g_shell_notify));

    PatchImport(
        "kernel32.dll", "ExitProcess",
        reinterpret_cast<void*>(&HookExitProcess),
        reinterpret_cast<void**>(&g_exit_process));

    PatchImport(
        "kernel32.dll", "TerminateProcess",
        reinterpret_cast<void*>(&HookTerminateProcess),
        reinterpret_cast<void**>(&g_terminate_process));
}

static DWORD WINAPI PatchWorker(void*) {
    // This worker starts after DLL_PROCESS_ATTACH returns, so hook setup does
    // not perform VirtualProtect work while the loader lock is held.
    InstallHooks();

    if (g_main_thread) {
        WaitForSingleObject(g_main_thread, INFINITE);
        CleanupTray();
        CloseHandle(g_main_thread);
        g_main_thread = nullptr;
    }
    return 0;
}

extern "C" __declspec(dllexport) LONG WINAPI CallNtPowerInformation(
    int information_level,
    PVOID input_buffer,
    ULONG input_buffer_length,
    PVOID output_buffer,
    ULONG output_buffer_length) {

    if (!g_nt_power_information) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        g_nt_power_information = ntdll
            ? reinterpret_cast<NtPowerInformationFn>(
                  GetProcAddress(ntdll, "NtPowerInformation"))
            : nullptr;
    }

    if (!g_nt_power_information) {
        return static_cast<LONG>(0xC000007AL); // STATUS_PROCEDURE_NOT_FOUND
    }

    return g_nt_power_information(
        information_level,
        input_buffer,
        input_buffer_length,
        output_buffer,
        output_buffer_length);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        DuplicateHandle(
            GetCurrentProcess(), GetCurrentThread(),
            GetCurrentProcess(), &g_main_thread,
            SYNCHRONIZE, FALSE, 0);

        HANDLE worker = CreateThread(
            nullptr, 0, PatchWorker, nullptr, 0, nullptr);
        if (worker) {
            CloseHandle(worker);
        }
    }
    return TRUE;
}
