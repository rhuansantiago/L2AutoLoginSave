// Phase 2.5: account model + DPAPI storage shared between dllmain.cpp,
// overlay_ui.cpp and d3d9_hook.cpp.

#pragma once
#include <vector>
#include <string>
#include <windows.h>

constexpr int kNumSlots = 10;

struct Account {
    std::wstring user;             // plaintext, displayed in the list
    std::vector<BYTE> passBlob;    // DPAPI-encrypted UTF-16 password
    bool empty() const { return user.empty() && passBlob.empty(); }
};

// Persistent storage at %APPDATA%\hollow_l2_overlay\accounts.dat
void  AccountsLoad();
void  AccountsSave();
Account& AccountsGet(int slot);  // slot in [0, kNumSlots)

// DPAPI helpers (per-user-per-machine encryption — no key, no prompt).
std::vector<BYTE> DpapiProtect(const std::wstring& plaintext);
std::wstring      DpapiUnprotect(const std::vector<BYTE>& blob);

// Calibrated screen coordinates of L2's login form fields. Used by the
// SendInput typer to click each field before typing, so the user/pass don't
// get crossed if TAB doesn't behave (it often doesn't on UE2 textboxes).
struct LoginCoords {
    POINT idField{};
    POINT passField{};
    bool  calibrated = false;
};
void          CoordsLoad();
void          CoordsSave();
LoginCoords&  CoordsGet();

// Install the D3D9 EndScene/Reset hooks (called from DllMain ATTACH).
void OverlayInstall();
