// l2ui.dll — Phase 1 of the hollow L2 login overlay.
//
// Loaded into L2.exe by hollow_l2_inject.exe via CreateRemoteThread +
// LoadLibraryW, while L2.exe's main thread is still suspended (so this
// runs BEFORE Themida's TLS callbacks fire on the main thread).
//
// Phase 1 deliverable: prove load by logging to overlay.log and showing
// a one-shot MessageBox. No hooks, no UI yet — that's phases 2+.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include "overlay_ui.h"

const char* kLogPath = "H:\\L2_Essence_542_SamuraiCrow\\hollow_l2\\overlay.log";

// Logf is shared with overlay_ui.cpp via extern "C" linkage.
extern "C" void Logf(const char* fmt, ...) {
    FILE* f = fopen(kLogPath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d pid=%lu tid=%lu] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

namespace {
void LogProcessInfo() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t dllPath[MAX_PATH] = {0};
    GetModuleFileNameW(GetModuleHandleW(L"l2ui.dll"), dllPath, MAX_PATH);
    Logf("=== l2ui.dll ATTACH ===");
    Logf("host exe : %ls", exePath);
    Logf("self dll : %ls", dllPath);
    Logf("cmdline  : %ls", GetCommandLineW());
    Logf("modules visible at attach time:");
    static const wchar_t* const kProbeModules[] = {
        L"kernel32.dll", L"user32.dll",
        L"Core.dll", L"Engine.dll", L"Window.dll",
        L"NWindow.dll", L"D3DDrv.dll", L"steam_api.dll",
    };
    for (size_t i = 0; i < sizeof(kProbeModules)/sizeof(kProbeModules[0]); ++i) {
        HMODULE h = GetModuleHandleW(kProbeModules[i]);
        Logf("  %-14ls -> %p", kProbeModules[i], (void*)h);
    }
}

}  // namespace

// Named export so CFF Explorer (or any IAT editor) can wire L2.exe's
// import table to "l2ui.dll!L2UI_Init". Windows loader only needs to be
// able to FIND the export — it doesn't have to call it before our
// DllMain runs (DllMain is called as part of LoadLibrary). The function
// is therefore a permanent no-op.
extern "C" __declspec(dllexport) void __cdecl L2UI_Init() {
    Logf("L2UI_Init called (someone invoked the named export — usually safe to ignore)");
}

BOOL APIENTRY DllMain(HINSTANCE hDll, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        LogProcessInfo();
        // Phase 2.5: install D3D9 EndScene/Reset hooks. ImGui inits
        // lazily on the first hooked EndScene with L2's real device.
        OverlayInstall();
        Logf("DllMain ATTACH complete (overlay thread spawned)");
    } else if (reason == DLL_PROCESS_DETACH) {
        Logf("=== l2ui.dll DETACH ===");
    }
    return TRUE;
}
