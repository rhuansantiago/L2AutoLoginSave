// Phase 2.5 D3D9 hook + ImGui overlay rendering.
//
// Strategy:
//   1. At install time we create a temp IDirect3DDevice9 (1x1 windowed)
//      just to read its vtable pointer. All real Device9 instances share
//      the same vtable (it lives in d3d9.dll's read-only data), so
//      patching this one slot affects L2's device too.
//   2. We swap the EndScene slot (offset 0xA8 / vtable index 42 on x86)
//      and the Reset slot (vtable index 16) for our hooks.
//   3. First time our EndScene hook fires we lazily init ImGui's DX9 +
//      Win32 backends using the REAL L2 device + window. We also subclass
//      the window's wndproc so ImGui gets mouse/keyboard.
//   4. Each frame: ImGui new frame, build overlay UI, render, then call
//      original EndScene.
//
// The Reset hook is required because device reset (alt-tab fullscreen,
// resize) destroys all default-pool D3D resources — ImGui's vertex/index
// buffers and font texture have to be released/recreated around it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "MinHook.h"
#include "overlay_ui.h"
#include "client_profiles.h"

#pragma comment(lib, "d3d9.lib")

extern "C" void Logf(const char* fmt, ...);

// Forward decl from ImGui's Win32 backend — handles input messages.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using EndScene_t = HRESULT(__stdcall*)(IDirect3DDevice9*);
using Reset_t    = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t  = HRESULT(__stdcall*)(IDirect3DDevice9*,
                                       const RECT*, const RECT*, HWND, const RGNDATA*);

EndScene_t  g_origEndScene   = nullptr;
EndScene_t  g_origEndSceneEx = nullptr;
Reset_t     g_origReset      = nullptr;
Reset_t     g_origResetEx    = nullptr;
Present_t   g_origPresent    = nullptr;
Present_t   g_origPresentEx  = nullptr;

void**      g_vtable     = nullptr;
void**      g_vtableEx   = nullptr;
WNDPROC     g_origWndProc  = nullptr;
HWND        g_hostHwnd     = nullptr;
bool        g_imguiReady   = false;
bool        g_overlayShow  = true;  // toggle with INSERT key

volatile LONG g_callsEndScene = 0;
volatile LONG g_callsPresent  = 0;
volatile LONG g_callsReset    = 0;

// UI state
int   g_selectedSlot  = -1;
// Add/Edit modal state — modal is opened by '+' button or right-click Edit.
bool  g_openAddModal  = false;   // request: open modal next frame
int   g_editingSlot   = -1;      // -1 = adding new, >=0 = editing this slot
char  g_addUserBuf[64] = {0};
char  g_addPassBuf[64] = {0};

// -----------------------------------------------------------------------------
// UI helpers — wstring <-> utf8 conversion for ImGui (it speaks utf8)
// -----------------------------------------------------------------------------
void WideToUtf8(const std::wstring& w, char* out, int outBytes) {
    out[0] = 0;
    if (w.empty()) return;
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, outBytes, nullptr, nullptr);
}
std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    return w;
}

// -----------------------------------------------------------------------------
// Slot helpers
// -----------------------------------------------------------------------------
static int FirstEmptySlot() {
    for (int i = 0; i < kNumSlots; i++) {
        if (AccountsGet(i).empty()) return i;
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Native NWindow.dll fn pointers / hook targets. NWindow.dll is not
// Themida-protected, so these RVAs are stable.
//
//   AuthLogin                 @ NWindow+0x8770e0
//       bool __cdecl(wchar_t* u, wchar_t* p, int otp)
//       internal body of UUIScript::execRequestLogin
//
//   execRequestLoginServer    @ NWindow+0x9eded0
//       void __thiscall(UUIScript*, FFrame&, void* result)
//       fired by UScript when the user clicks a server in the server list
//       popup. Hooking it gives us a reliable "leaving login screen" event,
//       since after this fires the client transitions to char-select.
// -----------------------------------------------------------------------------
// Per-client RVA table lives in client_profiles.h. Detected at hook install
// time by hashing the probe module's PE TimeDateStamp.
const ClientProfile* g_profile = nullptr;

// --------------------------- Essence family ----------------------------------
// AuthLogin is __stdcall (its epilogue ends with `ret 0xC`, callee cleans
// the 12-byte arg block).
using PFN_AuthLoginStdcall = int (__stdcall*)(wchar_t* user, wchar_t* pass, int otp);
PFN_AuthLoginStdcall g_pAuthLogin    = nullptr;  // raw entry pointer
PFN_AuthLoginStdcall g_origAuthLogin = nullptr;  // MinHook trampoline (real impl)
volatile bool g_inOurAuthLogin = false;          // suppress auto-capture when WE call

// __thiscall trampoline shared across all the UScript-native hooks below.
using PFN_ExecThiscall = void (__thiscall*)(void* This, void* FFrame, void* result);
PFN_ExecThiscall g_origExecGotoLogin          = nullptr;
PFN_ExecThiscall g_origExecRequestLoginServer = nullptr;
PFN_ExecThiscall g_origExecShowWindowGFx      = nullptr;

// --------------------------- Interlude family --------------------------------
// All Interlude hooks are __thiscall on member functions (this in ECX).
// We use the __fastcall trick (ECX=this, EDX=junk) for both detours and the
// outgoing call from LoginNative. The trampoline gets stored back into a
// typedef'd pointer so we can re-enter the original with the right ABI.
using PFN_UNH_AuthLogin    = int  (__fastcall*)(void* This, void* /*edx*/, wchar_t* user, wchar_t* pass, int otp);
using PFN_UNH_Init         = void (__fastcall*)(void* This, void* /*edx*/, int n, void* gameEngine);
using PFN_UNH_ServerLogin  = int  (__fastcall*)(void* This, void* /*edx*/, void* l2ParamStack);
using PFN_UGE_SrvSelOK     = int  (__fastcall*)(void* This, void* /*edx*/);
using PFN_UGE_SrvSelFail   = int  (__fastcall*)(void* This, void* /*edx*/, int code);
using PFN_UNH_IsNotYetLogin= bool (__fastcall*)(void* This, void* /*edx*/);

PFN_UNH_AuthLogin     g_origUNHAuthLogin     = nullptr;
PFN_UNH_Init          g_origUNHInit          = nullptr;
PFN_UNH_ServerLogin   g_origUNHServerLogin   = nullptr;
PFN_UGE_SrvSelOK      g_origUGEAuthSrvOK     = nullptr;
PFN_UGE_SrvSelFail    g_origUGEAuthSrvFail   = nullptr;
PFN_UNH_IsNotYetLogin g_pUNHIsNotYetLogin    = nullptr;
void*                 g_uNetworkHandler      = nullptr;  // captured singleton this

// State: between AuthLogin success and the next state transition (login
// completes OR returns to login), every UGFxUIScript::ShowWindow call is
// logged with this-byte-dump so we can identify which UScript object/class
// is the server-list popup.
volatile bool g_loginInProgress = false;

// Disconnect-return detection: every UGFxUIScript::ShowWindow `this`
// pointer observed BEFORE the first AuthLogin attempt is recorded as a
// "pre-auth window" (Login UI, splash, etc.). After AuthLogin has fired,
// any subsequent ShowWindow on a `this` in this set means we're back to
// the login phase — including the disconnect path that bypasses
// execGotoLogin entirely.
constexpr int kMaxPreAuthWnds = 8;
void* g_preAuthWnd[kMaxPreAuthWnds] = {};
int   g_preAuthCount = 0;
volatile bool g_authLoginSeen = false;

bool ResolveProfileAndModule() {
    if (g_profile) return true;
    g_profile = FindClientProfile();
    if (!g_profile) {
        // No probe module loaded with a matching timestamp yet.
        return false;
    }
    Logf("Profile matched: '%s' (family=%s, probe=%ls)",
         g_profile->label,
         g_profile->family == kFamilyEssence ? "Essence" : "Interlude",
         g_profile->probeModule);
    return true;
}

bool ResolveNativeLogin() {
    if (!ResolveProfileAndModule()) return false;
    HMODULE hMod = GetModuleHandleW(g_profile->probeModule);
    if (!hMod) return false;

    if (g_profile->family == kFamilyEssence) {
        if (g_pAuthLogin) return true;
        g_pAuthLogin = (PFN_AuthLoginStdcall)((uintptr_t)hMod + g_profile->rvaAuthLoginInternal);
        Logf("LoginNative: AuthLogin=%p  (NWindow base=%p + rva=0x%lx)",
             g_pAuthLogin, hMod, (unsigned long)g_profile->rvaAuthLoginInternal);
        return true;
    }
    // Interlude — already resolved (function pointer is per-call from profile)
    return true;
}

// SEH wrappers must live in functions with no C++ objects requiring unwind
// (otherwise: error C2712). Sentinel -1 on exception.
static int SafeAuthLoginCall(PFN_AuthLoginStdcall fn, wchar_t* u, wchar_t* p, int otp) {
    __try { return fn(u, p, otp); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int SafeUNHAuthLoginCall(PFN_UNH_AuthLogin fn, void* This, wchar_t* u, wchar_t* p, int otp) {
    __try { return fn(This, nullptr, u, p, otp); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int SafeUNHIsNotYetLogin(PFN_UNH_IsNotYetLogin fn, void* This) {
    __try { return fn(This, nullptr) ? 1 : 0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int SafeMeasureWideLen(const wchar_t* p, int maxLen) {
    if (!p) return -1;
    __try {
        for (int i = 0; i < maxLen; i++) if (p[i] == 0) return i;
        return maxLen;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Called from inside HookAuthLogin when the game's own UI submitted the
// login. Saves the credentials to the first empty slot if not already in
// the list.
static void AutoCaptureLogin(const wchar_t* user, const wchar_t* pass) {
    int userLen = SafeMeasureWideLen(user, 64);
    int passLen = SafeMeasureWideLen(pass, 64);
    if (userLen < 2 || passLen < 1) {
        Logf("AutoCapture: skipped (user.len=%d pass.len=%d)", userLen, passLen);
        return;
    }
    std::wstring u(user, userLen);
    std::wstring p(pass, passLen);
    // Validate user chars (no control/embedded NULs)
    for (wchar_t c : u) {
        if (c == 0 || c < 32 || c == 127) {
            Logf("AutoCapture: skipped, user has bad char 0x%04x", (unsigned)c);
            return;
        }
    }
    // Skip if already saved
    for (int i = 0; i < kNumSlots; i++) {
        if (AccountsGet(i).user == u) {
            Logf("AutoCapture: user='%ls' already in slot %d", u.c_str(), i + 1);
            return;
        }
    }
    int slot = FirstEmptySlot();
    if (slot < 0) {
        Logf("AutoCapture: no empty slot for user='%ls'", u.c_str());
        return;
    }
    AccountsGet(slot).user = u;
    AccountsGet(slot).passBlob = DpapiProtect(p);
    AccountsSave();
    Logf("AutoCapture: saved game login to slot %d (user='%ls')", slot + 1, u.c_str());
}

// MinHook detour for the AuthLogin internal fn. Fires for BOTH our overlay
// (which sets g_inOurAuthLogin) and for the L2 native login UI. When the
// game itself submitted, we capture the credentials AND force-show the
// overlay (game-initiated AuthLogin proves the user is back on the login
// screen — covers the disconnect path that bypasses execGotoLogin).
int __stdcall HookAuthLogin(wchar_t* user, wchar_t* pass, int otp) {
    g_authLoginSeen = true;  // unlock pre-auth re-show detection
    if (!g_inOurAuthLogin) {
        AutoCaptureLogin(user, pass);
        Logf("HookAuthLogin (game-initiated) — forcing overlay visible");
        g_overlayShow = true;
        g_loginInProgress = true;  // re-arm ShowWindow probe for the next transition
    }
    return g_origAuthLogin(user, pass, otp);
}

bool LoginNative(const std::wstring& user, const std::wstring& pass) {
    if (!ResolveNativeLogin()) return false;
    std::wstring u = user, p = pass;
    g_inOurAuthLogin = true;
    g_authLoginSeen  = true;

    int rc = 0;
    if (g_profile->family == kFamilyEssence) {
        // Stdcall internal AuthLogin via trampoline (bypass our own hook).
        PFN_AuthLoginStdcall fn = g_origAuthLogin ? g_origAuthLogin : g_pAuthLogin;
        rc = SafeAuthLoginCall(fn, u.data(), p.data(), /*otp*/ 0);
    } else {
        // Interlude — __thiscall on captured UNetworkHandler singleton.
        if (!g_uNetworkHandler) {
            g_inOurAuthLogin = false;
            Logf("LoginNative: UNetworkHandler singleton not captured yet — "
                 "log in once via the native L2 form first to populate it");
            return false;
        }
        HMODULE hEng = GetModuleHandleW(g_profile->probeModule);
        PFN_UNH_AuthLogin fn = g_origUNHAuthLogin;
        if (!fn && hEng) {
            fn = (PFN_UNH_AuthLogin)((uintptr_t)hEng + g_profile->rvaUNHRequestAuthLogin);
        }
        if (!fn) {
            g_inOurAuthLogin = false;
            Logf("LoginNative: no UNH RequestAuthLogin pointer available");
            return false;
        }
        rc = SafeUNHAuthLoginCall(fn, g_uNetworkHandler, u.data(), p.data(), /*otp*/ 0);
    }

    g_inOurAuthLogin = false;
    if (rc == -1) {
        Logf("LoginNative: AuthLogin threw SEH exception");
        return false;
    }
    Logf("LoginNative: AuthLogin returned %d for user='%ls'", rc, user.c_str());
    if (rc != 0) {
        // Arm ShowWindow probing — every subsequent UGFxUIScript::ShowWindow
        // call gets dumped until we identify the server-list popup or some
        // terminal state transition fires (HookGotoLogin / HookRequestLoginServer).
        g_loginInProgress = true;
    }
    return rc != 0;
}

// True iff user/pass look like sensible login credentials. Catches embedded
// NULs (which print as terminator under %ls and pass naive size() checks),
// control characters, and trivially-short strings. The L2 client pops a
// hard "Critical Error" dialog and dies when the auth server rejects
// malformed packets, so we'd rather refuse the send here.
static bool IsSaneCredString(const std::wstring& s, size_t minLen) {
    if (s.size() < minLen) return false;
    for (wchar_t c : s) {
        if (c == 0) return false;
        if (c < 32 || c == 127) return false;
    }
    return true;
}

// Login the currently selected slot. Heavily logged for diagnostics — we
// keep losing the chain somewhere between validation and the actual call.
void ActionLogin() {
    Logf("ActionLogin v3 ENTRY: g_selectedSlot=%d", g_selectedSlot);
    if (g_selectedSlot < 0 || g_selectedSlot >= kNumSlots) {
        Logf("ActionLogin: BAIL no slot selected");
        return;
    }
    Account& a = AccountsGet(g_selectedSlot);
    Logf("ActionLogin: slot=%d user.size=%zu passBlob.size=%zu",
         g_selectedSlot + 1, a.user.size(), a.passBlob.size());
    if (a.user.size() < 2) {
        Logf("ActionLogin: BAIL user.size=%zu < 2", a.user.size());
        return;
    }
    for (size_t k = 0; k < a.user.size(); ++k) {
        wchar_t c = a.user[k];
        if (c == 0 || c < 32 || c == 127) {
            Logf("ActionLogin: BAIL user[%zu]=0x%04x bad", k, (unsigned)c);
            return;
        }
    }
    std::wstring pass = DpapiUnprotect(a.passBlob);
    Logf("ActionLogin: pass.size after Unprotect=%zu", pass.size());
    if (pass.size() < 1) {
        Logf("ActionLogin: BAIL pass empty");
        return;
    }
    Logf("ActionLogin: -> LoginNative user='%ls'", a.user.c_str());
    LoginNative(a.user, pass);
}

// -----------------------------------------------------------------------------
// ImGui frame
// -----------------------------------------------------------------------------
// Open the Add/Edit modal. slot=-1 → add new; slot>=0 → edit existing.
static void OpenAccountModal(int slot) {
    g_editingSlot = slot;
    if (slot < 0) {
        g_addUserBuf[0] = 0;
        g_addPassBuf[0] = 0;
    } else {
        Account& a = AccountsGet(slot);
        WideToUtf8(a.user, g_addUserBuf, sizeof(g_addUserBuf));
        std::wstring pw = DpapiUnprotect(a.passBlob);
        WideToUtf8(pw, g_addPassBuf, sizeof(g_addPassBuf));
    }
    g_openAddModal = true;
}

// Persist the modal's edits into a slot. If editing an existing slot, write
// back to it; if adding new, drop into the first empty slot. Returns the
// slot index used, or -1 on failure (no empty slot / empty inputs).
static int CommitAccountModal() {
    std::wstring user = Utf8ToWide(g_addUserBuf);
    std::wstring pass = Utf8ToWide(g_addPassBuf);
    if (user.empty() || pass.empty()) return -1;
    int target = g_editingSlot >= 0 ? g_editingSlot : FirstEmptySlot();
    if (target < 0 || target >= kNumSlots) {
        Logf("UI: no empty slot to add account");
        return -1;
    }
    Account& a = AccountsGet(target);
    a.user = user;
    a.passBlob = DpapiProtect(pass);
    AccountsSave();
    Logf("UI: %s slot %d (user='%ls')",
         g_editingSlot >= 0 ? "edited" : "added", target + 1, user.c_str());
    return target;
}

void DrawOverlay() {
    if (!g_overlayShow) return;

    ImGuiIO& io = ImGui::GetIO();
    // Fixed size, left-center of the game window. ImGuiCond_Appearing snaps
    // back to this position/size every time the overlay re-opens (after
    // INSERT-toggle or auto-show on return to login), but lets the user
    // drag/resize during a single visible session.
    constexpr float kWinW = 290.0f;
    constexpr float kWinH = 420.0f;
    ImGui::SetNextWindowSize(ImVec2(kWinW, kWinH), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(20.0f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing,
        ImVec2(0.0f, 0.5f));  // pivot: left edge, vertical center

    if (ImGui::Begin("Saved accounts", &g_overlayShow,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::Separator();

        // List of populated slots only. Click to select, double-click to
        // login, right-click for Login / Edit / Remove. Visible rows
        // alternate background tint (zebra) for readability.
        const ImU32 kZebraColor = IM_COL32(255, 255, 255, 14);
        int populated = 0;
        // Reserve ~95 px at the bottom for [Log In][+] + hint lines. The
        // listbox gets the rest and shows a scrollbar when content overflows.
        const float kReserveBottom = 95.0f;
        float listH = ImGui::GetContentRegionAvail().y - kReserveBottom;
        if (listH < 80.0f) listH = 80.0f;
        if (ImGui::BeginListBox("##slots", ImVec2(-1, listH))) {
            for (int i = 0; i < kNumSlots; i++) {
                Account& a = AccountsGet(i);
                if (a.empty()) continue;
                ++populated;

                char user_utf8[64];
                WideToUtf8(a.user, user_utf8, sizeof(user_utf8));
                char label[160];
                snprintf(label, sizeof(label), " %2d   %s", i + 1, user_utf8);

                // Zebra stripe — draw subtle bg rect every other visible row,
                // before the Selectable so its hover/select layer covers it.
                if ((populated & 1) == 0) {
                    ImVec2 p0 = ImGui::GetCursorScreenPos();
                    float h = ImGui::GetTextLineHeightWithSpacing();
                    ImVec2 p1(p0.x + ImGui::GetContentRegionAvail().x, p0.y + h);
                    ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, kZebraColor);
                }

                ImGui::PushID(i);
                bool selected = (g_selectedSlot == i);

                // Reserve room on the right for the X delete button so the
                // Selectable doesn't span the whole row.
                const float kXBtnSpace = 26.0f;
                float labelW = ImGui::GetContentRegionAvail().x - kXBtnSpace;
                if (ImGui::Selectable(label, selected,
                        ImGuiSelectableFlags_AllowItemOverlap |
                        ImGuiSelectableFlags_AllowDoubleClick,
                        ImVec2(labelW, 0))) {
                    g_selectedSlot = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        ActionLogin();
                    }
                }
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    g_selectedSlot = i;
                    if (ImGui::MenuItem("Login"))  ActionLogin();
                    if (ImGui::MenuItem("Edit"))   OpenAccountModal(i);
                    if (ImGui::MenuItem("Remove")) {
                        a = {};
                        AccountsSave();
                        if (g_selectedSlot == i) g_selectedSlot = -1;
                        Logf("UI: removed slot %d", i + 1);
                    }
                    ImGui::EndPopup();
                }
                // X button — same line, red-tinted, deletes the slot.
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32( 90, 30, 30, 180));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(180, 50, 50, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(220, 70, 70, 255));
                if (ImGui::SmallButton("X")) {
                    a = {};
                    AccountsSave();
                    if (g_selectedSlot == i) g_selectedSlot = -1;
                    Logf("UI: removed slot %d via X button", i + 1);
                }
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }
            if (populated == 0) {
                ImGui::TextDisabled("  (empty — press + to add)");
            }
            ImGui::EndListBox();
        }

        ImGui::Spacing();
        bool canLogin = (g_selectedSlot >= 0 && g_selectedSlot < kNumSlots
                         && !AccountsGet(g_selectedSlot).empty());
        ImGui::BeginDisabled(!canLogin);
        if (ImGui::Button("Log In", ImVec2(215, 28))) ActionLogin();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(FirstEmptySlot() < 0);
        if (ImGui::Button("+", ImVec2(35, 28))) OpenAccountModal(-1);
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextDisabled("Double-click an account to log in.");
        ImGui::TextDisabled("INSERT toggles overlay.");
    }
    ImGui::End();

    // ---- Add/Edit account modal ----
    if (g_openAddModal) {
        ImGui::OpenPopup("Account##addedit");
        g_openAddModal = false;
    }
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Account##addedit", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled(g_editingSlot < 0 ? "New account" : "Edit account");
        ImGui::Spacing();
        ImGui::PushItemWidth(-80);
        ImGui::InputText("User", g_addUserBuf, sizeof(g_addUserBuf));
        ImGui::InputText("Pass", g_addPassBuf, sizeof(g_addPassBuf),
                         ImGuiInputTextFlags_Password);
        ImGui::PopItemWidth();
        ImGui::Spacing();

        // require at least 2 chars in user and 1 char in pass — anything
        // shorter is almost certainly a typo and the L2 client tends to
        // crash when the auth server rejects bogus packets.
        size_t userLen = strlen(g_addUserBuf);
        size_t passLen = strlen(g_addPassBuf);
        bool canSave = (userLen >= 2) && (passLen >= 1);
        ImGui::BeginDisabled(!canSave);
        if (ImGui::Button("Save", ImVec2(75, 26))) {
            int idx = CommitAccountModal();
            if (idx >= 0) g_selectedSlot = idx;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save & Login", ImVec2(110, 26))) {
            int idx = CommitAccountModal();
            if (idx >= 0) {
                g_selectedSlot = idx;
                ActionLogin();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(75, 26))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------------
// WndProc hook — feed mouse/keyboard to ImGui and SUPPRESS messages from
// reaching L2 when ImGui has focus. ImGui_ImplWin32_WndProcHandler returns 0
// almost always (even after consuming a message), so we can't rely on its
// return value — we check io.WantCaptureKeyboard / io.WantCaptureMouse
// AFTER feeding the message in.
// -----------------------------------------------------------------------------
LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Toggle overlay with INSERT key (never falls through to game).
    if (msg == WM_KEYDOWN && wp == VK_INSERT) {
        g_overlayShow = !g_overlayShow;
        return 0;
    }

    if (g_imguiReady) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);

        if (g_overlayShow) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureKeyboard) {
                switch (msg) {
                    case WM_KEYDOWN: case WM_KEYUP:
                    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                    case WM_CHAR: case WM_SYSCHAR:
                    case WM_DEADCHAR: case WM_SYSDEADCHAR:
                    case WM_UNICHAR:
                    case WM_IME_CHAR:
                        return 0;
                }
            }
            if (io.WantCaptureMouse) {
                if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
                if (msg == WM_SETCURSOR) return 1;
            }
        }
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
// Hook bodies
// -----------------------------------------------------------------------------
void InitImGuiIfNeeded(IDirect3DDevice9* dev) {
    if (g_imguiReady) return;

    D3DDEVICE_CREATION_PARAMETERS p = {};
    if (FAILED(dev->GetCreationParameters(&p))) {
        Logf("InitImGui: GetCreationParameters failed");
        return;
    }
    g_hostHwnd = p.hFocusWindow;
    if (!g_hostHwnd) {
        Logf("InitImGui: no hFocusWindow on device");
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't persist imgui.ini next to L2.exe
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Flat theme — zero rounding, neutral grayscale, no gradients.
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 0.0f;
    s.FrameRounding     = 0.0f;
    s.PopupRounding     = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.GrabRounding      = 0.0f;
    s.TabRounding       = 0.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.ItemSpacing       = ImVec2(8, 6);
    s.FramePadding      = ImVec2(8, 5);
    s.WindowPadding     = ImVec2(10, 10);
    auto& c = s.Colors;
    const ImVec4 kBg     (0.09f, 0.10f, 0.12f, 0.97f);
    const ImVec4 kBgAlt  (0.13f, 0.14f, 0.17f, 1.00f);
    const ImVec4 kAccent (0.27f, 0.45f, 0.70f, 1.00f);
    const ImVec4 kBorder (0.20f, 0.22f, 0.25f, 1.00f);
    c[ImGuiCol_WindowBg]        = kBg;
    c[ImGuiCol_PopupBg]         = kBg;
    c[ImGuiCol_ChildBg]         = kBgAlt;
    c[ImGuiCol_FrameBg]         = kBgAlt;
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBg]         = kBg;
    c[ImGuiCol_TitleBgActive]   = kBg;
    c[ImGuiCol_TitleBgCollapsed]= kBg;
    c[ImGuiCol_Border]          = kBorder;
    c[ImGuiCol_Button]          = kBgAlt;
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonActive]    = kAccent;
    c[ImGuiCol_Header]          = kBgAlt;
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderActive]    = kAccent;
    c[ImGuiCol_Separator]       = kBorder;
    c[ImGuiCol_SeparatorHovered]= kBorder;
    c[ImGuiCol_SeparatorActive] = kBorder;
    c[ImGuiCol_CheckMark]       = kAccent;
    c[ImGuiCol_ScrollbarBg]     = kBg;
    c[ImGuiCol_ScrollbarGrab]   = kBgAlt;
    c[ImGuiCol_Text]            = ImVec4(0.86f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.50f, 0.52f, 0.55f, 1.00f);

    ImGui_ImplWin32_Init(g_hostHwnd);
    ImGui_ImplDX9_Init(dev);

    // Subclass the L2 window's wndproc so we get input.
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hostHwnd, GWLP_WNDPROC,
                                                (LONG_PTR)HookWndProc);

    g_imguiReady = true;
    Logf("InitImGui: ImGui live (hwnd=%p, dev=%p)", g_hostHwnd, dev);
}

// MinHook detours for UScript natives. __thiscall on x86 passes `this`
// in ECX, so MSVC __fastcall (ECX, EDX, stack...) gives us access — we
// ignore the EDX slot (register garbage). The trampoline g_orig... is
// typed __thiscall so we re-enter the original with the right convention.
void __fastcall HookExecGotoLogin(void* This, void* /*edx*/, void* FFrame, void* result) {
    Logf("HookGotoLogin fired — back to login → show overlay");
    g_overlayShow = true;
    g_loginInProgress = false;
    g_origExecGotoLogin(This, FFrame, result);
}
void __fastcall HookExecRequestLoginServer(void* This, void* /*edx*/, void* FFrame, void* result) {
    Logf("HookRequestLoginServer fired — user picked a server → hide overlay");
    g_overlayShow = false;
    g_loginInProgress = false;
    g_origExecRequestLoginServer(This, FFrame, result);
}

// SEH-safe DWORD probe.
static unsigned int SafeReadU32(const void* p) {
    __try { return *(const volatile unsigned int*)p; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0xDEADBEEFu; }
}

// UGFxUIScript::ShowWindow — fires for every Flash-driven UI window. Three
// jobs:
//  1. Before any AuthLogin: register `this` as a pre-auth window (Login UI,
//     splash, etc.).
//  2. After AuthLogin, hide overlay if `this->Class == NCLobbyWnd` (works
//     for builds that route the server-list popup through ShowWindow).
//  3. After AuthLogin, if `this` is in the pre-auth set, the login phase
//     is back — show overlay. Covers the disconnect-return path where
//     execGotoLogin is never called.
void __fastcall HookExecShowWindowGFx(void* This, void* /*edx*/, void* FFrame, void* result) {
    if (This && g_profile) {
        HMODULE hNW = GetModuleHandleW(L"NWindow.dll");
        void* ncLobby = nullptr;
        if (hNW) {
            ncLobby = (void*)SafeReadU32((char*)hNW + g_profile->rvaAutoclassNCLobbyWnd);
        }

        // Phase 1: pre-auth recording.
        if (!g_authLoginSeen) {
            bool already = false;
            for (int i = 0; i < g_preAuthCount; ++i) {
                if (g_preAuthWnd[i] == This) { already = true; break; }
            }
            if (!already && g_preAuthCount < kMaxPreAuthWnds) {
                g_preAuthWnd[g_preAuthCount++] = This;
                Logf("PreAuth: registered window #%d this=%p", g_preAuthCount, This);
            }
        } else {
            // Phase 3: re-show of a pre-auth window after we've already
            // started auth at least once → back to login.
            for (int i = 0; i < g_preAuthCount; ++i) {
                if (g_preAuthWnd[i] == This) {
                    if (!g_overlayShow) {
                        Logf("PreAuth wnd #%d re-shown (this=%p) → back to login → show overlay",
                             i + 1, This);
                        g_overlayShow = true;
                        g_loginInProgress = false;
                    }
                    break;
                }
            }
        }

        // Phase 2: NCLobbyWnd class match.
        unsigned int dw[16] = {};
        for (int i = 0; i < 16; ++i) dw[i] = SafeReadU32((char*)This + i * 4);
        int matchOffset = -1;
        for (int i = 0; i < 16; ++i) {
            if (dw[i] == (unsigned int)(uintptr_t)ncLobby && ncLobby != nullptr) {
                matchOffset = i * 4; break;
            }
        }
        if (matchOffset >= 0) {
            Logf("ShowWindow: NCLobbyWnd class match @+0x%x (this=%p) → hide overlay",
                 matchOffset, This);
            g_overlayShow = false;
            g_loginInProgress = false;
        }
    }
    g_origExecShowWindowGFx(This, FFrame, result);
}

// ============================================================================
// Interlude hook detours
// ============================================================================
void __fastcall HookUNHInit(void* This, void* /*edx*/, int n, void* gameEngine) {
    g_uNetworkHandler = This;
    Logf("HookUNHInit fired — UNetworkHandler singleton @ %p", This);
    g_origUNHInit(This, nullptr, n, gameEngine);
}
int __fastcall HookUNHAuthLogin(void* This, void* /*edx*/, wchar_t* user, wchar_t* pass, int otp) {
    g_authLoginSeen = true;
    if (!g_uNetworkHandler) {
        g_uNetworkHandler = This;
        Logf("HookUNHAuthLogin: captured singleton late @ %p", This);
    }
    if (!g_inOurAuthLogin) {
        AutoCaptureLogin(user, pass);
        Logf("HookUNHAuthLogin (game-initiated) — forcing overlay visible");
        g_overlayShow = true;
    }
    return g_origUNHAuthLogin(This, nullptr, user, pass, otp);
}
int __fastcall HookUNHServerLogin(void* This, void* /*edx*/, void* paramStack) {
    Logf("HookUNHServerLogin — user picked server → hide overlay");
    g_overlayShow = false;
    return g_origUNHServerLogin(This, nullptr, paramStack);
}
int __fastcall HookUGEAuthSrvOK(void* This, void* /*edx*/) {
    Logf("HookUGEAuthSrvOK — login confirmed → hide overlay");
    g_overlayShow = false;
    return g_origUGEAuthSrvOK(This, nullptr);
}
int __fastcall HookUGEAuthSrvFail(void* This, void* /*edx*/, int code) {
    Logf("HookUGEAuthSrvFail (code=%d) → show overlay", code);
    g_overlayShow = true;
    return g_origUGEAuthSrvFail(This, nullptr, code);
}

// ============================================================================
// Hook installation — dispatches by profile family
// ============================================================================
static void EnsureClientHooks() {
    static bool s_done = false;
    if (s_done) return;
    if (!ResolveProfileAndModule()) return;

    HMODULE hMod = GetModuleHandleW(g_profile->probeModule);
    if (!hMod) return;
    uintptr_t base = (uintptr_t)hMod;

    auto installOne = [base](uintptr_t rva, void* detour, void** ppOrig, const char* name) {
        if (!rva) return;
        void* tgt = (void*)(base + rva);
        MH_STATUS cs = MH_CreateHook(tgt, detour, ppOrig);
        if (cs != MH_OK) { Logf("MH_CreateHook(%s) FAILED %d", name, (int)cs); return; }
        MH_STATUS es = MH_EnableHook(tgt);
        if (es != MH_OK) { Logf("MH_EnableHook(%s) FAILED %d", name, (int)es); return; }
        Logf("MH hook installed: %s @ %p (trampoline=%p)", name, tgt, *ppOrig);
    };

    if (g_profile->family == kFamilyEssence) {
        installOne(g_profile->rvaExecGotoLogin,          (void*)&HookExecGotoLogin,
                   (void**)&g_origExecGotoLogin,          "NWindow!execGotoLogin");
        installOne(g_profile->rvaExecRequestLoginServer, (void*)&HookExecRequestLoginServer,
                   (void**)&g_origExecRequestLoginServer, "NWindow!execRequestLoginServer");
        installOne(g_profile->rvaExecShowWindowGFx,      (void*)&HookExecShowWindowGFx,
                   (void**)&g_origExecShowWindowGFx,      "NWindow!UGFxUIScript::ShowWindow");
        installOne(g_profile->rvaAuthLoginInternal,      (void*)&HookAuthLogin,
                   (void**)&g_origAuthLogin,              "NWindow!AuthLogin");
    } else {  // Interlude
        // Stash IsNotYetLogin entry for the polling loop.
        if (g_profile->rvaUNHIsNotYetLogin) {
            g_pUNHIsNotYetLogin = (PFN_UNH_IsNotYetLogin)(base + g_profile->rvaUNHIsNotYetLogin);
            Logf("Interlude IsNotYetLogin entry @ %p", g_pUNHIsNotYetLogin);
        }
        installOne(g_profile->rvaUNHInit,               (void*)&HookUNHInit,
                   (void**)&g_origUNHInit,               "engine!UNH::Init");
        installOne(g_profile->rvaUNHRequestAuthLogin,   (void*)&HookUNHAuthLogin,
                   (void**)&g_origUNHAuthLogin,          "engine!UNH::RequestAuthLogin");
        installOne(g_profile->rvaUNHRequestServerLogin, (void*)&HookUNHServerLogin,
                   (void**)&g_origUNHServerLogin,        "engine!UNH::RequestServerLogin");
        installOne(g_profile->rvaUGEAuthSrvSelectOK,    (void*)&HookUGEAuthSrvOK,
                   (void**)&g_origUGEAuthSrvOK,          "engine!UGE::OnAuthSrvSelectOK");
        installOne(g_profile->rvaUGEAuthSrvSelectFail,  (void*)&HookUGEAuthSrvFail,
                   (void**)&g_origUGEAuthSrvFail,        "engine!UGE::OnAuthSrvSelectFail");
    }
    s_done = true;
}

void RenderFrame(IDirect3DDevice9* dev) {
    InitImGuiIfNeeded(dev);
    if (!g_imguiReady) return;
    EnsureClientHooks();
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawOverlay();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

HRESULT __stdcall HookEndScene(IDirect3DDevice9* dev) {
    LONG n = InterlockedIncrement(&g_callsEndScene);
    if (n <= 3) Logf("HookEndScene call #%ld (dev=%p)", n, dev);
    RenderFrame(dev);
    return g_origEndScene(dev);
}

HRESULT __stdcall HookEndSceneEx(IDirect3DDevice9* dev) {
    LONG n = InterlockedIncrement(&g_callsEndScene);
    if (n <= 3) Logf("HookEndSceneEx call #%ld (dev=%p)", n, dev);
    RenderFrame(dev);
    return g_origEndSceneEx(dev);
}

HRESULT __stdcall HookPresent(IDirect3DDevice9* dev, const RECT* src,
                              const RECT* dst, HWND hwnd, const RGNDATA* dirty) {
    LONG n = InterlockedIncrement(&g_callsPresent);
    if (n <= 3) Logf("HookPresent call #%ld (dev=%p)", n, dev);
    // Some apps skip BeginScene/EndScene entirely (mostly D3D9Ex). Render
    // here as a fallback so we don't depend on EndScene firing.
    if (g_callsEndScene == 0) RenderFrame(dev);
    return g_origPresent(dev, src, dst, hwnd, dirty);
}

HRESULT __stdcall HookPresentEx(IDirect3DDevice9* dev, const RECT* src,
                                const RECT* dst, HWND hwnd, const RGNDATA* dirty) {
    LONG n = InterlockedIncrement(&g_callsPresent);
    if (n <= 3) Logf("HookPresentEx call #%ld (dev=%p)", n, dev);
    if (g_callsEndScene == 0) RenderFrame(dev);
    return g_origPresentEx(dev, src, dst, hwnd, dirty);
}

HRESULT __stdcall HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    InterlockedIncrement(&g_callsReset);
    Logf("HookReset called");
    if (g_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_origReset(dev, pp);
    if (g_imguiReady) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

HRESULT __stdcall HookResetEx(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    InterlockedIncrement(&g_callsReset);
    Logf("HookResetEx called");
    if (g_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_origResetEx(dev, pp);
    if (g_imguiReady) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// Periodic watchdog: log call counts. With MinHook the patches are on the
// FUNCTION CODE in d3d9.dll (.text), shared across all devices. If counts
// stay at zero after several seconds, L2 isn't using d3d9.dll's standard
// device implementation at all.
DWORD WINAPI WatchdogThread(LPVOID /*lpv*/) {
    for (int i = 0; i < 30; i++) {
        Sleep(1000);
        Logf("[watchdog t=%ds] EndScene=%ld Present=%ld Reset=%ld%s",
             i + 1,
             g_callsEndScene, g_callsPresent, g_callsReset,
             g_imguiReady ? "  imgui=ready" : "");
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Install — temp device to read vtable + atomic vtable patch
// -----------------------------------------------------------------------------
// Read the vtable function pointers from a freshly-created temp device.
// The vtable is per-instance in modern d3d9.dll (heap-allocated, freed on
// Release), but the FUNCTIONS the slots point at live in d3d9.dll's .text
// — those are stable. We save the function addresses and patch the code
// itself via MinHook.
bool ReadDeviceVtableSlots(bool useEx, IDirect3D9* d3dBase,
                           void** outReset, void** outPresent, void** outEndScene) {
    *outReset = *outPresent = *outEndScene = nullptr;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetDesktopWindow();
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    if (useEx) {
        IDirect3D9Ex* d3dEx = (IDirect3D9Ex*)d3dBase;
        IDirect3DDevice9Ex* devEx = nullptr;
        HRESULT hr = d3dEx->CreateDeviceEx(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &devEx);
        if (FAILED(hr) || !devEx) {
            Logf("ReadVtable(Ex): CreateDeviceEx failed hr=0x%lx", hr);
            return false;
        }
        void** vt = *(void***)devEx;
        *outReset    = vt[16];
        *outPresent  = vt[17];
        *outEndScene = vt[42];
        devEx->Release();
        return true;
    }
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3dBase->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                       GetDesktopWindow(),
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                       &pp, &dev);
    if (FAILED(hr) || !dev) {
        Logf("ReadVtable: CreateDevice failed hr=0x%lx", hr);
        return false;
    }
    void** vt = *(void***)dev;
    *outReset    = vt[16];
    *outPresent  = vt[17];
    *outEndScene = vt[42];
    dev->Release();
    return true;
}

}  // namespace

void D3D9HookInstall() {
    HMODULE hD3D9 = LoadLibraryW(L"d3d9.dll");
    if (!hD3D9) { Logf("D3D9HookInstall: LoadLibrary(d3d9.dll) failed"); return; }

    auto pDirect3DCreate9 = (decltype(&Direct3DCreate9))
        GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pDirect3DCreate9) { Logf("D3D9HookInstall: no Direct3DCreate9"); return; }

    IDirect3D9* d3d = pDirect3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { Logf("D3D9HookInstall: Direct3DCreate9 failed"); return; }

    void *reset = nullptr, *present = nullptr, *endScene = nullptr;
    if (!ReadDeviceVtableSlots(false, d3d, &reset, &present, &endScene)) {
        d3d->Release();
        return;
    }
    d3d->Release();
    Logf("D3D9HookInstall: Device9 targets - Reset=%p Present=%p EndScene=%p",
         reset, present, endScene);

    // Device9Ex path — may share OR differ
    auto pDirect3DCreate9Ex = (HRESULT (WINAPI*)(UINT, IDirect3D9Ex**))
        GetProcAddress(hD3D9, "Direct3DCreate9Ex");
    void *resetEx = nullptr, *presentEx = nullptr, *endSceneEx = nullptr;
    if (pDirect3DCreate9Ex) {
        IDirect3D9Ex* d3dEx = nullptr;
        if (SUCCEEDED(pDirect3DCreate9Ex(D3D_SDK_VERSION, &d3dEx)) && d3dEx) {
            ReadDeviceVtableSlots(true, d3dEx, &resetEx, &presentEx, &endSceneEx);
            d3dEx->Release();
            Logf("D3D9HookInstall: Device9Ex targets - Reset=%p Present=%p EndScene=%p",
                 resetEx, presentEx, endSceneEx);
        }
    }

    // ---- MinHook installation ----
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Logf("MH_Initialize failed: %d", (int)s);
        return;
    }

    auto hookOne = [](void* target, void* detour, void** ppOrig, const char* name) {
        if (!target) return;
        MH_STATUS cs = MH_CreateHook(target, detour, ppOrig);
        if (cs != MH_OK) { Logf("MH_CreateHook(%s) FAILED status=%d", name, (int)cs); return; }
        MH_STATUS es = MH_EnableHook(target);
        if (es != MH_OK) { Logf("MH_EnableHook(%s) FAILED status=%d", name, (int)es); return; }
        Logf("MH hook installed: %s @ %p (trampoline=%p)", name, target, *ppOrig);
    };

    hookOne(endScene, (void*)&HookEndScene, (void**)&g_origEndScene, "Device9::EndScene");
    hookOne(present,  (void*)&HookPresent,  (void**)&g_origPresent,  "Device9::Present");
    hookOne(reset,    (void*)&HookReset,    (void**)&g_origReset,    "Device9::Reset");

    // Device9Ex: only hook if different from Device9. If the same function
    // implements both (some d3d9.dll versions share), the Device9 hook
    // already covers it.
    if (endSceneEx && endSceneEx != endScene)
        hookOne(endSceneEx, (void*)&HookEndSceneEx, (void**)&g_origEndSceneEx, "Device9Ex::EndScene");
    if (presentEx && presentEx != present)
        hookOne(presentEx,  (void*)&HookPresentEx,  (void**)&g_origPresentEx,  "Device9Ex::Present");
    if (resetEx && resetEx != reset)
        hookOne(resetEx,    (void*)&HookResetEx,    (void**)&g_origResetEx,    "Device9Ex::Reset");

    // For watchdog logging — save the FUNCTION addresses (not vtable
    // pointers, which are per-device and would be invalid post-Release).
    g_vtable = (void**)endScene;   // reused as "the EndScene fn addr" marker

    // NWindow!execRequestLoginServer hook is installed lazily from
    // RenderFrame (via EnsureNWindowHooks) — NWindow.dll isn't loaded yet
    // at this point in normal L2 startup.

    HANDLE hWd = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    if (hWd) CloseHandle(hWd);
}
