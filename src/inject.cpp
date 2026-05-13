// hollow_l2_inject.exe — Phase 1 injector.
//
// Spawns L2.exe paused, calls LoadLibraryW("l2ui.dll") inside it via
// CreateRemoteThread, then resumes the main thread and waits for the
// game to exit. Zero modification to L2's System_en folder.
//
// Usage:
//   hollow_l2_inject.exe [--exe <l2.exe path>] [--dll <l2ui.dll path>] [--args "<L2 args>"]
//
// Without args, uses hard-coded defaults pointing at this workspace.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {

const wchar_t* kDefaultExe =
    L"H:\\L2_Essence_542_SamuraiCrow\\LINEAGE2-ESSENCE-SAMURAI-CROW-541-EU\\System_en\\L2.exe";
const wchar_t* kDefaultDll =
    L"H:\\L2_Essence_542_SamuraiCrow\\hollow_l2\\build\\overlay\\Release\\l2ui.dll";

void Die(const wchar_t* what, DWORD err = GetLastError()) {
    wprintf(L"[FATAL] %s (err=%lu)\n", what, err);
    ExitProcess(1);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* exePath = kDefaultExe;
    const wchar_t* dllPath = kDefaultDll;
    const wchar_t* extraArgs = L"";

    for (int i = 1; i + 1 < argc; ++i) {
        if      (wcscmp(argv[i], L"--exe")  == 0) exePath  = argv[++i];
        else if (wcscmp(argv[i], L"--dll")  == 0) dllPath  = argv[++i];
        else if (wcscmp(argv[i], L"--args") == 0) extraArgs = argv[++i];
    }

    wprintf(L"=== hollow_l2_inject ===\n");
    wprintf(L"exe : %s\n", exePath);
    wprintf(L"dll : %s\n", dllPath);
    wprintf(L"args: \"%s\"\n", extraArgs);

    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES)
        Die(L"L2.exe not found at the specified path", 0);
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
        Die(L"l2ui.dll not found at the specified path", 0);

    // Working directory must be the System dir so the engine finds its
    // packages via relative paths.
    wchar_t cwd[MAX_PATH] = {0};
    wcscpy_s(cwd, MAX_PATH, exePath);
    if (wchar_t* slash = wcsrchr(cwd, L'\\')) *slash = 0;

    // Build command line: "<exePath>" <extraArgs>
    wchar_t cmdLine[MAX_PATH * 4];
    swprintf_s(cmdLine, MAX_PATH * 4, L"\"%s\" %s", exePath, extraArgs);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    wprintf(L"\nSpawning L2.exe CREATE_SUSPENDED in %s ...\n", cwd);
    if (!CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, cwd, &si, &pi)) {
        Die(L"CreateProcess");
    }
    wprintf(L"  pid=%lu  main tid=%lu\n", pi.dwProcessId, pi.dwThreadId);

    // Allocate memory in target for the DLL path string.
    SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remoteAddr = VirtualAllocEx(pi.hProcess, nullptr, pathBytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteAddr) {
        TerminateProcess(pi.hProcess, 1);
        Die(L"VirtualAllocEx");
    }
    wprintf(L"\nAllocated %zu bytes in remote at 0x%p\n", pathBytes, remoteAddr);

    SIZE_T written = 0;
    if (!WriteProcessMemory(pi.hProcess, remoteAddr, dllPath, pathBytes, &written)) {
        TerminateProcess(pi.hProcess, 1);
        Die(L"WriteProcessMemory");
    }
    wprintf(L"Wrote DLL path (%zu bytes) to remote\n", written);

    // Resolve LoadLibraryW in OUR kernel32 — same base as target's because
    // kernel32 is loaded at a stable address per-boot.
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    auto pLoadLibraryW = (LPTHREAD_START_ROUTINE)GetProcAddress(hK32, "LoadLibraryW");
    if (!pLoadLibraryW) {
        TerminateProcess(pi.hProcess, 1);
        Die(L"GetProcAddress(LoadLibraryW)");
    }
    wprintf(L"LoadLibraryW @ 0x%p\n", pLoadLibraryW);

    // Spawn remote thread: LoadLibraryW(remoteAddr) — Windows loader maps
    // our DLL and runs its DllMain on this thread. The main thread of
    // L2.exe is still suspended, so Themida's TLS callbacks haven't run.
    wprintf(L"\nCreateRemoteThread -> LoadLibraryW(<dll path>)\n");
    HANDLE hThread = CreateRemoteThread(pi.hProcess, nullptr, 0,
                                         pLoadLibraryW, remoteAddr, 0, nullptr);
    if (!hThread) {
        TerminateProcess(pi.hProcess, 1);
        Die(L"CreateRemoteThread");
    }

    wprintf(L"Waiting for DllMain to return inside L2.exe...\n");
    WaitForSingleObject(hThread, INFINITE);
    DWORD libBase = 0;
    GetExitCodeThread(hThread, &libBase);  // LoadLibraryW returns HMODULE
    CloseHandle(hThread);

    if (libBase == 0) {
        wprintf(L"[FATAL] LoadLibraryW returned NULL inside L2.exe — DLL load failed\n");
        TerminateProcess(pi.hProcess, 1);
        ExitProcess(2);
    }
    wprintf(L"DLL mapped at 0x%lx in L2.exe (DllMain has returned)\n", libBase);

    // Reclaim remote memory; DLL path string no longer needed.
    VirtualFreeEx(pi.hProcess, remoteAddr, 0, MEM_RELEASE);

    wprintf(L"\nResuming L2.exe main thread — game boot starts now\n");
    ResumeThread(pi.hThread);

    wprintf(L"Waiting for L2.exe to exit...\n");
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    wprintf(L"\nL2.exe exited with code %lu (0x%lx)\n", exitCode, exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
