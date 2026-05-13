// Phase 2.5 — Account data model + DPAPI storage. The actual UI lives in
// d3d9_hook.cpp now (ImGui rendered inside L2's framebuffer); this file
// is the data layer that the hook calls into.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include "overlay_ui.h"

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")

extern "C" void Logf(const char* fmt, ...);
extern void D3D9HookInstall();    // d3d9_hook.cpp

namespace {

Account g_slots[kNumSlots];
LoginCoords g_coords;

std::wstring GetStoragePath() {
    wchar_t appdata[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata);
    std::wstring p = appdata;
    p += L"\\hollow_l2_overlay";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\accounts.dat";
    return p;
}
std::wstring GetCoordsPath() {
    wchar_t appdata[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata);
    std::wstring p = appdata;
    p += L"\\hollow_l2_overlay";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\coords.dat";
    return p;
}

}  // namespace

std::vector<BYTE> DpapiProtect(const std::wstring& plaintext) {
    DATA_BLOB in = { (DWORD)(plaintext.size() * sizeof(wchar_t)),
                     (BYTE*)plaintext.data() };
    DATA_BLOB out = {};
    std::vector<BYTE> result;
    if (CryptProtectData(&in, L"hollow_l2 overlay account",
                         nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        result.assign(out.pbData, out.pbData + out.cbData);
        LocalFree(out.pbData);
    }
    return result;
}

std::wstring DpapiUnprotect(const std::vector<BYTE>& blob) {
    if (blob.empty()) return L"";
    DATA_BLOB in = { (DWORD)blob.size(), (BYTE*)blob.data() };
    DATA_BLOB out = {};
    std::wstring result;
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        result.assign((wchar_t*)out.pbData,
                      (wchar_t*)out.pbData + out.cbData / sizeof(wchar_t));
        LocalFree(out.pbData);
    }
    return result;
}

void AccountsLoad() {
    std::wstring path = GetStoragePath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Logf("AccountsLoad: no file at %ls", path.c_str());
        return;
    }
    DWORD sz = GetFileSize(h, nullptr);
    std::vector<BYTE> buf(sz);
    DWORD rd = 0;
    ReadFile(h, buf.data(), sz, &rd, nullptr);
    CloseHandle(h);
    if (rd < 4 || memcmp(buf.data(), "HL2A", 4) != 0) { Logf("AccountsLoad: bad magic"); return; }
    size_t off = 4;
    for (int i = 0; i < kNumSlots; i++) {
        if (off + 8 > buf.size()) break;
        uint32_t userBytes = *(uint32_t*)(buf.data() + off);
        uint32_t blobBytes = *(uint32_t*)(buf.data() + off + 4);
        off += 8;
        if (off + userBytes + blobBytes > buf.size()) break;
        g_slots[i].user.assign((wchar_t*)(buf.data() + off),
                               (wchar_t*)(buf.data() + off + userBytes));
        off += userBytes;
        g_slots[i].passBlob.assign(buf.data() + off, buf.data() + off + blobBytes);
        off += blobBytes;
    }
    // Sanitize loaded slots — purge any with embedded NULs, control chars,
    // or trivially short users (likely leftover test cruft / corruption).
    // This keeps malformed creds from being sent to the auth server, which
    // crashes the L2 client with a "Critical Error" dialog.
    int populated = 0, purged = 0;
    for (int i = 0; i < kNumSlots; i++) {
        if (g_slots[i].empty()) continue;
        const std::wstring& u = g_slots[i].user;
        bool ok = (u.size() >= 2 && u.size() <= 32);
        if (ok) {
            for (wchar_t c : u) {
                if (c == 0 || c < 32 || c == 127) { ok = false; break; }
            }
        }
        if (!ok) {
            Logf("AccountsLoad: slot %d corrupted (user.size=%zu), purged",
                 i + 1, u.size());
            g_slots[i] = {};
            ++purged;
        } else {
            ++populated;
        }
    }
    if (purged > 0) AccountsSave();  // persist the cleanup
    Logf("AccountsLoad: loaded %d slots (%d purged)", populated, purged);
}

void AccountsSave() {
    std::wstring path = GetStoragePath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Logf("AccountsSave: CreateFile err=%lu", GetLastError());
        return;
    }
    auto wr = [&](const void* p, DWORD n) { DWORD w; WriteFile(h, p, n, &w, nullptr); };
    wr("HL2A", 4);
    for (int i = 0; i < kNumSlots; i++) {
        uint32_t ub = (uint32_t)(g_slots[i].user.size() * sizeof(wchar_t));
        uint32_t bb = (uint32_t)g_slots[i].passBlob.size();
        wr(&ub, 4); wr(&bb, 4);
        if (ub) wr(g_slots[i].user.data(), ub);
        if (bb) wr(g_slots[i].passBlob.data(), bb);
    }
    CloseHandle(h);
    Logf("AccountsSave: wrote %ls", path.c_str());
}

Account& AccountsGet(int slot) {
    static Account dummy;
    if (slot < 0 || slot >= kNumSlots) return dummy;
    return g_slots[slot];
}

LoginCoords& CoordsGet() { return g_coords; }

void CoordsLoad() {
    HANDLE h = CreateFileW(GetCoordsPath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Logf("CoordsLoad: no file (not calibrated yet)"); return; }
    struct { uint32_t magic; LONG idX, idY, passX, passY; } rec = {};
    DWORD rd = 0;
    ReadFile(h, &rec, sizeof(rec), &rd, nullptr);
    CloseHandle(h);
    if (rd == sizeof(rec) && rec.magic == 0x4C43324C /* 'L2CL' little-endian */) {
        g_coords.idField   = { rec.idX, rec.idY };
        g_coords.passField = { rec.passX, rec.passY };
        g_coords.calibrated = true;
        Logf("CoordsLoad: ID=(%ld,%ld) PASS=(%ld,%ld)",
             rec.idX, rec.idY, rec.passX, rec.passY);
    } else {
        Logf("CoordsLoad: bad magic or short read (%lu bytes)", rd);
    }
}

void CoordsSave() {
    HANDLE h = CreateFileW(GetCoordsPath().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Logf("CoordsSave: CreateFile err=%lu", GetLastError()); return; }
    struct { uint32_t magic; LONG idX, idY, passX, passY; } rec;
    rec.magic = 0x4C43324C;  // 'L2CL'
    rec.idX = g_coords.idField.x;  rec.idY = g_coords.idField.y;
    rec.passX = g_coords.passField.x; rec.passY = g_coords.passField.y;
    DWORD w = 0;
    WriteFile(h, &rec, sizeof(rec), &w, nullptr);
    CloseHandle(h);
    Logf("CoordsSave: ID=(%ld,%ld) PASS=(%ld,%ld)",
         rec.idX, rec.idY, rec.passX, rec.passY);
}

// Heavy init runs on a worker thread to avoid loader-lock issues. Direct3D
// device creation in particular pulls in COM init + driver DLLs and was
// causing L2.exe to fail startup with STATUS_DLL_INIT_FAILED (0xC0000142)
// when run inside DllMain.
static DWORD WINAPI InstallWorker(LPVOID /*lpv*/) {
    Logf("InstallWorker: started, sleeping 2s for L2 init to settle...");
    Sleep(2000);
    AccountsLoad();
    CoordsLoad();
    D3D9HookInstall();
    Logf("InstallWorker: install complete");
    return 0;
}

// Public install entry called by DllMain. Singleton-guards itself and
// spawns InstallWorker to do the actual install AFTER DllMain returns
// (loader lock released by then).
void OverlayInstall() {
    char mutexName[64];
    snprintf(mutexName, 64, "Local\\hollow_l2_overlay_%lu", GetCurrentProcessId());
    HANDLE hMutex = CreateMutexA(nullptr, TRUE, mutexName);
    if (!hMutex) {
        Logf("OverlayInstall: CreateMutex err=%lu", GetLastError());
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Logf("OverlayInstall: already installed in this process — skip");
        CloseHandle(hMutex);
        return;
    }
    // hMutex deliberately leaked — keeps the singleton claim for the
    // lifetime of the process.
    HANDLE h = CreateThread(nullptr, 0, InstallWorker, nullptr, 0, nullptr);
    if (h) {
        Logf("OverlayInstall: spawned install worker");
        CloseHandle(h);
    } else {
        Logf("OverlayInstall: CreateThread failed err=%lu", GetLastError());
    }
}
