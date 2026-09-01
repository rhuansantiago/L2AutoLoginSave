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
// IDirect3D9::CreateDevice — the factory's device-creation method. Hooked
// so we can release ImGui's DX9 refs on the OLD device before a new one is
// created (some L2 Interlude paths fail with D3DERR_DEVICELOST when the
// old device's refcount is still elevated by our resources).
using CreateDevice_t = HRESULT (__stdcall*)(
    IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDev);
CreateDevice_t g_origCreateDevice = nullptr;
Reset_t     g_origReset      = nullptr;
Reset_t     g_origResetEx    = nullptr;
Present_t   g_origPresent    = nullptr;
Present_t   g_origPresentEx  = nullptr;
void**      g_vtable     = nullptr;
void**      g_vtableEx   = nullptr;
WNDPROC     g_origWndProc  = nullptr;
HWND        g_hostHwnd     = nullptr;
// Window-title personalization: captures L2's original title once, then
// rewrites it to "<original> - <char>" when the local player enters world.
// The local char name is captured via a User::SetName hook gated by the
// RequestEnterWorld signal — engine creates the local pawn FIRST, then
// loads NPCs/other players, so the very next SetName call after the
// "entering world" signal is our character. Helps multi-boxers tell
// their windows apart in alt-tab / taskbar.
wchar_t        g_currentPlayerName[64]    = {};
wchar_t        g_currentPlayerClass[64]   = {};
int            g_currentClassId           = -1;
unsigned int   g_lastPolledLevel          = 0;
wchar_t        g_originalWindowTitle[256] = {};
bool           g_originalTitleCaptured    = false;
volatile bool  g_enteringWorld            = false;
volatile bool  g_localUserVerified        = false;
volatile bool  g_viewTargetBound          = false;
// Char-select buffer (Interlude): captured during the char-select screen so
// we can bind g_localUser BEFORE world entry. In some builds the local
// pawn's SetName fires only during char-select, not in-world.
struct CharSelectEntry { void* userPtr; wchar_t name[64]; };
CharSelectEntry g_charSelectBuf[10] = {};
int            g_charSelectCount     = 0;
volatile bool  g_charSelectActive    = false;
int            g_pickedSlot          = -1;
// Pointer to the local player's User instance — captured from the first
// User::SetName call after RequestEnterWorld. Used to call GetClassNamePointer
// and to scan offsets for the level field (no clean getter exported).
void*          g_localUser                = nullptr;
int            g_extrasDelayFrames        = 0;
int            g_guessedLevelOffset       = 0;  // 0 = unknown
int            g_enumDumpDelayFrames      = 0;  // when >0, decremented per frame; on hit 0, dump UNH users
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
PFN_ExecThiscall g_origExecSetEulaText        = nullptr;
PFN_ExecThiscall g_origExecEulaAgree          = nullptr;
// Set while the EULA dialog is on screen. DrawOverlay suppresses output
// while this is true so our window doesn't paint over the legal notice.
volatile bool g_eulaShown = false;
// __fastcall on x86 — ECX=arg0, EDX=arg1, rest on stack. The internal
// UUIEventManager::ExecuteEvent dispatcher takes (int eventID, FString*
// param) in (ECX, EDX). Hooking it gives us THE proper signal for the
// auth flow: EV_LoginOK / EV_LoginFail / EV_ShowEula / EV_ServerList all
// pass through this one function.
using PFN_ExecuteEvent = void (__fastcall*)(int eventID, void* param);
PFN_ExecuteEvent g_origExecuteEvent = nullptr;
// Within-session learning: captured `this` pointers for the EULA dialog
// and the server-list popup (both GFxUIScript-derived UScript classes).
// First time the user interacts with each, we capture; subsequent
// ShowWindow calls with the same `this` know to hide our overlay.
// Heap-allocated so they change per session — first encounter in any
// given session still shows the overlay (acceptable trade-off without
// deeper UClass/FName reflection).
void* g_eulaWndThis       = nullptr;
void* g_serverListWndThis = nullptr;
// --------------------------- Interlude family --------------------------------
// All Interlude hooks are __thiscall on member functions (this in ECX).
// We use the __fastcall trick (ECX=this, EDX=junk) for both detours and the
// outgoing call from LoginNative. The trampoline gets stored back into a
// typedef'd pointer so we can re-enter the original with the right ABI.
using PFN_UNH_AuthLogin       = int  (__fastcall*)(void* This, void* /*edx*/, wchar_t* user, wchar_t* pass, int otp);
using PFN_UNH_Init            = void (__fastcall*)(void* This, void* /*edx*/, int n, void* gameEngine);
using PFN_UNH_ServerLogin     = int  (__fastcall*)(void* This, void* /*edx*/, void* l2ParamStack);
using PFN_UGE_SrvSelOK        = int  (__fastcall*)(void* This, void* /*edx*/);
using PFN_UGE_SrvSelFail      = int  (__fastcall*)(void* This, void* /*edx*/, int code);
using PFN_UNH_IsNotYetLogin   = bool (__fastcall*)(void* This, void* /*edx*/);
using PFN_UNH_RequestServerList = int  (__fastcall*)(void* This, void* /*edx*/);
using PFN_EulaLoad              = int  (__fastcall*)(void* This, void* /*edx*/, int flag);
using PFN_OnAcceptLogOut        = void (__fastcall*)(void* This, void* /*edx*/);
using PFN_AuthReconnect         = void (__fastcall*)(void* This, void* /*edx*/);
using PFN_SetConsoleState       = void (__fastcall*)(void* This, void* /*edx*/, int newState);
using PFN_ExecLobbyEvent        = void (__fastcall*)(void* This, void* /*edx*/, wchar_t* eventName, int param);
PFN_UNH_AuthLogin         g_origUNHAuthLogin         = nullptr;
PFN_UNH_Init              g_origUNHInit              = nullptr;
PFN_UNH_ServerLogin       g_origUNHServerLogin       = nullptr;
PFN_UGE_SrvSelOK          g_origUGEAuthSrvOK         = nullptr;
PFN_UGE_SrvSelFail        g_origUGEAuthSrvFail       = nullptr;
PFN_UNH_IsNotYetLogin     g_pUNHIsNotYetLogin        = nullptr;
PFN_UNH_RequestServerList g_origUNHRequestServerList = nullptr;
PFN_EulaLoad              g_origEulaLoad             = nullptr;
PFN_OnAcceptLogOut        g_origOnAcceptLogOut       = nullptr;
PFN_AuthReconnect         g_origAuthReconnect        = nullptr;
PFN_SetConsoleState       g_origSetConsoleState      = nullptr;
PFN_ExecLobbyEvent        g_origExecLobbyEvent       = nullptr;
void*                     g_uNetworkHandler          = nullptr;  // captured singleton this
// Legacy NWindow.dll (Interlude) UWindowHandle::execShowWindow. Same
// signature as the Essence ShowWindow native: __thiscall(FFrame&, void*).
PFN_ExecThiscall g_origLegacyShowWindow = nullptr;
PFN_ExecThiscall g_origLegacyUUIAPIShowWindow = nullptr;
// UUIScript::eventOnEvent(int eventID, FString const& param) — __thiscall.
// We hook this to log every UI event the engine fires at script side. The
// EULA show should be one of them; once we know the eventID we can match.
using PFN_LegacyEventOnEvent = void (__fastcall*)(void* This, void* /*edx*/, int eventID, void* fstr);
PFN_LegacyEventOnEvent g_origLegacyEventOnEvent = nullptr;
// User::SetName(wchar_t const*) — engine sets the local player's char name
// during world entry. We capture the first call after RequestEnterWorld
// fires (the local pawn is created before NPCs/other players).
//
// Signatures across families are equivalent: __thiscall void(wchar_t*).
// Each family has its own RVA in its own engine.dll image.
using PFN_UserSetName = void (__fastcall*)(void* This, void* /*edx*/, const wchar_t* name);
PFN_UserSetName g_origUserSetName_Essence = nullptr;
PFN_UserSetName g_origUserSetName_Interlude = nullptr;
// "Entering world" signal hooks. Different shapes per family but same role:
// set g_enteringWorld = true so the next User::SetName is recognized as ours.
// Essence: UGameEngine::RequestEnterWorld() — no args
using PFN_RequestEnterWorld_Essence = void (__fastcall*)(void* This, void* /*edx*/);
PFN_RequestEnterWorld_Essence g_origRequestEnterWorld_Essence = nullptr;
// Interlude: UNetworkHandler::RequestEnterWorldPacket(int, int*, K, K, K, K)
using PFN_RequestEnterWorld_Interlude = int (__fastcall*)(void* This, void* /*edx*/,
    int a, int* b, unsigned long c, unsigned long d, unsigned long e, unsigned long f);
PFN_RequestEnterWorld_Interlude g_origRequestEnterWorld_Interlude = nullptr;
// FL2GameData::EulaSave(int) — fires when user clicks Agree(1) or Disagree(0)
// on the EULA dialog. Hook this to CONFIRM the EULA flow is actually
// happening (and to log timing) so we can correlate with other signals.
using PFN_EulaSave = void (__fastcall*)(void* This, void* /*edx*/, int param);
PFN_EulaSave g_origEulaSave = nullptr;
// FL2GameData::NoticeLoad — candidate "dialog opening" signal (untested).
using PFN_NoticeLoad = void (__fastcall*)(void* This, void* /*edx*/);
PFN_NoticeLoad g_origNoticeLoad = nullptr;
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
// Window-title helpers. Capture L2's original title on first opportunity,
// then toggle between "<original>" (logged out / pre-world) and
// "<original> - <char>" (in-world).
static void CaptureOriginalWindowTitle() {
    if (g_originalTitleCaptured || !g_hostHwnd) return;
    int got = GetWindowTextW(g_hostHwnd, g_originalWindowTitle,
                             (int)_countof(g_originalWindowTitle));
    if (got <= 0) {
        wcscpy_s(g_originalWindowTitle, L"Lineage II");
    }
    g_originalTitleCaptured = true;
    Logf("WindowTitle: captured original = '%ls'", g_originalWindowTitle);
}
static void SetWindowTitleWithPlayer() {
    if (!g_hostHwnd || g_currentPlayerName[0] == 0) return;
    CaptureOriginalWindowTitle();
    wchar_t title[384];
    bool hasLvl = (g_lastPolledLevel >= 1 && g_lastPolledLevel <= 120);
    bool hasCls = (g_currentPlayerClass[0] != 0);
    if (hasCls && hasLvl) {
        _snwprintf_s(title, _countof(title), _TRUNCATE,
                     L"%s - %s [%s Lv.%u]",
                     g_originalWindowTitle, g_currentPlayerName,
                     g_currentPlayerClass, g_lastPolledLevel);
    } else if (hasCls) {
        _snwprintf_s(title, _countof(title), _TRUNCATE, L"%s - %s [%s]",
                     g_originalWindowTitle, g_currentPlayerName,
                     g_currentPlayerClass);
    } else if (hasLvl) {
        _snwprintf_s(title, _countof(title), _TRUNCATE, L"%s - %s [Lv.%u]",
                     g_originalWindowTitle, g_currentPlayerName,
                     g_lastPolledLevel);
    } else {
        _snwprintf_s(title, _countof(title), _TRUNCATE, L"%s - %s",
                     g_originalWindowTitle, g_currentPlayerName);
    }
    SetWindowTextW(g_hostHwnd, title);
    Logf("WindowTitle: set to '%ls'", title);
}
static void RestoreOriginalWindowTitle() {
    g_currentPlayerName[0] = 0;
    g_currentPlayerClass[0] = 0;
    g_currentClassId = -1;
    g_guessedLevelOffset = 0;
    g_lastPolledLevel = 0;
    g_localUser = nullptr;
    g_extrasDelayFrames = 0;
    g_enteringWorld = false;
    g_localUserVerified = false;
    g_charSelectCount = 0;
    g_charSelectActive = false;
    g_pickedSlot = -1;
    g_viewTargetBound = false;
    if (!g_hostHwnd || !g_originalTitleCaptured) return;
    SetWindowTextW(g_hostHwnd, g_originalWindowTitle);
    Logf("WindowTitle: restored to '%ls'", g_originalWindowTitle);
}
// Capture the local player's User* + name. The challenge is that User::SetName
// fires for EVERY visible character (local player, nearby players, NPCs).
// In some builds the local pawn's SetName is the very first; in others NPCs
// at spawn (e.g. "Adell" the newbie guide) get SetName before our pawn.
//
// Strategy: keep capturing every SetName while g_enteringWorld is true. Each
// capture resets the 60-frame extras-fetch countdown. After 1 second of NO
// new SetName, FetchPlayerExtras runs and locks in the LAST candidate seen,
// which is overwhelmingly likely to be our local player (NPCs/scene actors
// load first; the local pawn loads last because the engine waits for the
// world payload from the server). FetchPlayerExtras clears g_enteringWorld.
//
// Also handles in-game re-creation: when the engine destroys+recreates the
// local pawn (class swap, transform, awakening), SetName fires AGAIN with
// the same name but a new `This` pointer. We re-bind g_localUser there.
static void HandlePlayerSetName(void* userThis, const wchar_t* name) {
    // Interlude (Lucera): local pawn isn't captured via User::SetName in
    // this build (only NPCs / nearby actors fire it). Other approaches
    // (PC+0x150 Pawn read, OnLevelUpdate, char-select buffer) also failed
    // to identify the local player reliably. Title customization disabled
    // for Interlude — kept as just "Lineage II" until a better signal is
    // found. Essence works fine via this path.
    if (g_profile && g_profile->family == kFamilyInterlude) return;
    int n = SafeMeasureWideLen(name, 63);
    if (n < 2) return;
    for (int i = 0; i < n; i++) {
        wchar_t c = name[i];
        if (c < 32 || c == 127) return;
    }
    // Case 0: char-select window — record each (User*, name) by order so
    // SelectedCharacterNum's slot index maps to the right entry.
    if (g_charSelectActive && g_charSelectCount < 10) {
        g_charSelectBuf[g_charSelectCount].userPtr = userThis;
        memcpy(g_charSelectBuf[g_charSelectCount].name, name, n * sizeof(wchar_t));
        g_charSelectBuf[g_charSelectCount].name[n] = 0;
        Logf("CharSelect[%d]: User*=%p name='%ls'",
             g_charSelectCount, userThis, g_charSelectBuf[g_charSelectCount].name);
        g_charSelectCount++;
        return;
    }
    // Case 1: world-entry capture window open → keep updating to LATEST.
    if (g_enteringWorld) {
        memcpy(g_currentPlayerName, name, n * sizeof(wchar_t));
        g_currentPlayerName[n] = 0;
        g_localUser = userThis;
        g_extrasDelayFrames = 60;       // reset countdown — wait for stability
        g_currentPlayerClass[0] = 0;
        g_currentClassId = -1;
        g_guessedLevelOffset = 0;
        g_lastPolledLevel = 0;
        Logf("Player candidate (window open): '%ls' (User*=%p)", name, userThis);
        SetWindowTitleWithPlayer();
        return;
    }
    // Case 2: re-bind — same name, different User*. Engine destroyed our
    // old pawn and made a new one (class change / transform / awakening).
    if (g_currentPlayerName[0] != 0 && userThis != g_localUser) {
        int existingLen = SafeMeasureWideLen(g_currentPlayerName, 63);
        if (existingLen == n &&
            memcmp(name, g_currentPlayerName, n * sizeof(wchar_t)) == 0) {
            Logf("Local User recreated (likely class change): %p -> %p, re-fetching",
                 g_localUser, userThis);
            g_localUser = userThis;
            g_extrasDelayFrames = 60;
            g_currentPlayerClass[0] = 0;
            g_currentClassId = -1;
            g_guessedLevelOffset = 0;
            g_lastPolledLevel = 0;
        }
    }
}
// Class-ID → human-readable name lookup. Built from the L2 Essence "Tale
// of Aden Salvation" server source (ClassId.java). User::GetCurrentClassType
// returns this int. The engine's FName-returning GetClassNameW(int) maps
// class IDs to MESH ASSETS (e.g. "LineageWarrior.MElf"), not to real class
// names — so we maintain our own table.
static const wchar_t* GetClassNameById(int classId) {
    switch (classId) {
        // ---- Awakened (1st class) - Human
        case 0:  return L"Human Fighter";
        case 1:  return L"Warrior";
        case 2:  return L"Gladiator";
        case 3:  return L"Warlord";
        case 4:  return L"Human Knight";
        case 5:  return L"Paladin";
        case 6:  return L"Dark Avenger";
        case 7:  return L"Rogue";
        case 8:  return L"Treasure Hunter";
        case 9:  return L"Hawkeye";
        case 10: return L"Human Mystic";
        case 11: return L"Human Wizard";
        case 12: return L"Sorcerer";
        case 13: return L"Necromancer";
        case 14: return L"Warlock";
        case 15: return L"Cleric";
        case 16: return L"Bishop";
        case 17: return L"Prophet";
        // ---- Elf
        case 18: return L"Elven Fighter";
        case 19: return L"Elven Knight";
        case 20: return L"Temple Knight";
        case 21: return L"Sword Singer";
        case 22: return L"Elven Scout";
        case 23: return L"Plains Walker";
        case 24: return L"Silver Ranger";
        case 25: return L"Elven Mystic";
        case 26: return L"Elven Wizard";
        case 27: return L"Spellsinger";
        case 28: return L"Elemental Summoner";
        case 29: return L"Oracle";
        case 30: return L"Elder";
        // ---- Dark Elf
        case 31: return L"Dark Fighter";
        case 32: return L"Palus Knight";
        case 33: return L"Shillien Knight";
        case 34: return L"Bladedancer";
        case 35: return L"Assassin";
        case 36: return L"Abyss Walker";
        case 37: return L"Phantom Ranger";
        case 38: return L"Dark Mystic";
        case 39: return L"Dark Wizard";
        case 40: return L"Spellhowler";
        case 41: return L"Phantom Summoner";
        case 42: return L"Shillien Oracle";
        case 43: return L"Shillien Elder";
        // ---- Orc
        case 44: return L"Orc Fighter";
        case 45: return L"Orc Raider";
        case 46: return L"Destroyer";
        case 47: return L"Orc Monk";
        case 48: return L"Tyrant";
        case 49: return L"Orc Mystic";
        case 50: return L"Orc Shaman";
        case 51: return L"Overlord";
        case 52: return L"Warcryer";
        // ---- Dwarf
        case 53: return L"Dwarven Fighter";
        case 54: return L"Scavenger";
        case 55: return L"Bounty Hunter";
        case 56: return L"Artisan";
        case 57: return L"Warsmith";
        // ---- 3rd class (Human)
        case 88: return L"Duelist";
        case 89: return L"Dreadnought";
        case 90: return L"Phoenix Knight";
        case 91: return L"Hell Knight";
        case 92: return L"Sagittarius";
        case 93: return L"Adventurer";
        case 94: return L"Archmage";
        case 95: return L"Soultaker";
        case 96: return L"Arcana Lord";
        case 97: return L"Cardinal";
        case 98: return L"Hierophant";
        // ---- 3rd class (Elf)
        case 99:  return L"Eva's Templar";
        case 100: return L"Sword Muse";
        case 101: return L"Wind Rider";
        case 102: return L"Moonlight Sentinel";
        case 103: return L"Mystic Muse";
        case 104: return L"Elemental Master";
        case 105: return L"Eva's Saint";
        // ---- 3rd class (Dark Elf)
        case 106: return L"Shillien Templar";
        case 107: return L"Spectral Dancer";
        case 108: return L"Ghost Hunter";
        case 109: return L"Ghost Sentinel";
        case 110: return L"Storm Screamer";
        case 111: return L"Spectral Master";
        case 112: return L"Shillien Saint";
        // ---- 3rd class (Orc/Dwarf)
        case 113: return L"Titan";
        case 114: return L"Grand Khavatari";
        case 115: return L"Dominator";
        case 116: return L"Doomcryer";
        case 117: return L"Fortune Seeker";
        case 118: return L"Maestro";
        // ---- Kamael
        case 125: return L"Trooper";
        case 126: return L"Warder";
        case 127: return L"Berserker";
        case 130: return L"Soul Ranger";
        case 131: return L"Doombringer";
        case 134: return L"Trickster";
        case 192: return L"Kamael Soldier";
        case 193: return L"Soul Finder";
        case 194: return L"Soul Breaker";
        case 195: return L"Soul Hound";
        // ---- Death Knight (Essence)
        case 196: return L"Death Pilgrim (Human)";
        case 197: return L"Death Blade (Human)";
        case 198: return L"Death Messenger (Human)";
        case 199: return L"Death Knight (Human)";
        case 200: return L"Death Pilgrim (Elf)";
        case 201: return L"Death Blade (Elf)";
        case 202: return L"Death Messenger (Elf)";
        case 203: return L"Death Knight (Elf)";
        case 204: return L"Death Pilgrim (Dark Elf)";
        case 205: return L"Death Blade (Dark Elf)";
        case 206: return L"Death Messenger (Dark Elf)";
        case 207: return L"Death Knight (Dark Elf)";
        // ---- Sylph
        case 208: return L"Sylph Gunner";
        case 209: return L"Sharpshooter";
        case 210: return L"Wind Sniper";
        case 211: return L"Storm Blaster";
        // ---- Orc Rider
        case 217: return L"Orc Rider Lancer";
        case 218: return L"Orc Rider";
        case 219: return L"Dragoon";
        case 220: return L"Vanguard Rider";
        // ---- Assassin (Essence)
        case 221: return L"Assassin (Human)";
        case 222: return L"Hawkeye Assassin";
        case 223: return L"Crow Assassin";
        case 224: return L"Hidden Blade";
        case 225: return L"Assassin (Dark Elf)";
        case 226: return L"Shadow Assassin";
        case 227: return L"Phantom Assassin";
        case 228: return L"Shadow Walker";
        // ---- High Elf
        case 236: return L"Element Weaver (1st)";
        case 237: return L"Element Weaver (2nd)";
        case 238: return L"Element Weaver (3rd)";
        case 239: return L"Element Weaver (4th)";
        case 240: return L"Divine Templar (1st)";
        case 241: return L"Divine Templar (2nd)";
        case 242: return L"Divine Templar (3rd)";
        case 243: return L"Divine Templar (4th)";
        // ---- Varkas / Rose Vain / Samurai
        case 247: return L"Varkas (1st)";
        case 248: return L"Varkas (2nd)";
        case 249: return L"Varkas (3rd)";
        case 250: return L"Varkas (4th)";
        case 251: return L"Rose Vain (1st)";
        case 252: return L"Rose Vain (2nd)";
        case 253: return L"Rose Vain (3rd)";
        case 254: return L"Rose Vain (4th)";
        case 260: return L"Ashigaru";
        case 261: return L"Hatamoto";
        case 262: return L"Ronin";
        case 263: return L"Samurai";
        default:  return nullptr;
    }
}
// Read class name via User::GetClassNameW (Interlude: no args, Essence: takes
// int classType from GetCurrentClassType) — both return FName by value
// which we decode via Core.dll. Also scans first 512 bytes for level
// candidates. Called ~1s after SetName so the engine has time to populate
// the User struct from the EnterWorld response packet.
static void FetchPlayerExtras() {
    if (!g_localUser || !g_profile) return;
    HMODULE hEng = GetModuleHandleW(L"engine.dll");
    // Interlude: read the real char name via GetName on the Pawn pointer
    // we captured from PC+0x150 in HookSetViewTarget. The placeholder
    // "----" should have been replaced by the actual name by now (5s gate).
    if (hEng && g_profile->family == kFamilyInterlude && g_profile->rvaUserGetName) {
        using PFN_GetName = wchar_t* (__fastcall*)(void* This, void* /*edx*/);
        auto getName = (PFN_GetName)((uintptr_t)hEng + g_profile->rvaUserGetName);
        wchar_t* nm = nullptr;
        __try { nm = getName(g_localUser, nullptr); }
        __except(EXCEPTION_EXECUTE_HANDLER) { nm = nullptr; }
        int len = SafeMeasureWideLen(nm, 63);
        if (nm && len >= 1) {
            // Skip placeholder names made of dashes / nulls
            bool placeholder = true;
            for (int i = 0; i < len; i++) {
                if (nm[i] != L'-' && nm[i] != L' ' && nm[i] != 0) { placeholder = false; break; }
            }
            if (!placeholder) {
                memcpy(g_currentPlayerName, nm, len * sizeof(wchar_t));
                g_currentPlayerName[len] = 0;
                Logf("Interlude pawn name (from GetName at PC+0x150 → User): '%ls'",
                     g_currentPlayerName);
                SetWindowTitleWithPlayer();
            } else {
                Logf("Interlude pawn name still placeholder ('%ls') — retrying in 60 frames", nm);
                g_extrasDelayFrames = 60;  // retry once
                return;                     // skip class/level scan this round
            }
        } else {
            Logf("Interlude pawn GetName empty/null — retrying in 60 frames");
            g_extrasDelayFrames = 60;
            return;
        }
    }
    // Class name via GetCurrentClassType (int) + hardcoded ID→name table.
    // The engine's GetClassNameW(int)→FName route returns the MESH ASSET
    // name (useless), so we maintain our own table built from server source.
    if (hEng) {
        uintptr_t rvaGetType = (g_profile->family == kFamilyEssence)
            ? g_profile->rvaEngineUserGetCurrentClassType
            : g_profile->rvaUserGetCurrentClassType;
        if (rvaGetType) {
            using PFN_GetType = int (__fastcall*)(void* This, void* /*edx*/);
            auto getType = (PFN_GetType)((uintptr_t)hEng + rvaGetType);
            int classId = -1;
            __try { classId = getType(g_localUser, nullptr); }
            __except(EXCEPTION_EXECUTE_HANDLER) { classId = -1; }
            if (classId >= 0) {
                g_currentClassId = classId;
                const wchar_t* name = GetClassNameById(classId);
                if (name) {
                    int cn = SafeMeasureWideLen(name, 63);
                    memcpy(g_currentPlayerClass, name, cn * sizeof(wchar_t));
                    g_currentPlayerClass[cn] = 0;
                    Logf("Player class captured: '%ls' (id=%d)",
                         g_currentPlayerClass, classId);
                } else {
                    _snwprintf_s(g_currentPlayerClass, _countof(g_currentPlayerClass),
                                 _TRUNCATE, L"Cls.%d", classId);
                    Logf("Player class (unknown ID): id=%d → fallback '%ls'",
                         classId, g_currentPlayerClass);
                }
            } else {
                Logf("GetCurrentClassType SEH'd, no class");
            }
        }
    }
    // Level offset hunt — log all int dwords in [1,99] within first 512 bytes.
    // User picks the right offset by reading the log + comparing with their
    // in-game level. The first match gets used as a guess for SetWindowTitle.
    Logf("Level offset scan @ User*=%p (looking for plausible level 1..99):",
         g_localUser);
    int firstCandidate = 0;
    int candidatesFound = 0;
    for (int off = 0; off < 512; off += 4) {
        unsigned int v = 0;
        __try { v = *(volatile unsigned int*)((char*)g_localUser + off); }
        __except(EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v >= 1 && v <= 99) {
            Logf("  +0x%03x = %3u", off, v);
            if (firstCandidate == 0) firstCandidate = off;
            if (++candidatesFound >= 30) break;
        }
    }
    if (firstCandidate != 0) {
        g_guessedLevelOffset = firstCandidate;
        // Seed g_lastPolledLevel so the polling loop doesn't fire a redundant
        // "level changed" on the very first poll.
        __try { g_lastPolledLevel = *(volatile unsigned int*)((char*)g_localUser + g_guessedLevelOffset); }
        __except(EXCEPTION_EXECUTE_HANDLER) { g_lastPolledLevel = 0; }
        Logf("Level: guessed offset +0x%x (first 1..99 hit). "
             "Compare with in-game level — adjust offset in code if wrong.",
             g_guessedLevelOffset);
    }
    // Capture window closes here — subsequent SetNames are nearby players /
    // NPCs / etc, not us (unless a re-creation handled in HandlePlayerSetName).
    g_enteringWorld = false;
    Logf("Capture window closed, locked on User*=%p '%ls'", g_localUser, g_currentPlayerName);
    SetWindowTitleWithPlayer();
}
// Diagnostic: enumerate all User*s known to UNetworkHandler by calling
// GetUser(id) for ids 0..200 and logging the names. The local player must
// be in this list; comparing with known NPC names should let us identify
// it. Triggered once 5s after RequestEnterWorld.
static void EnumerateInterludeUsers() {
    if (!g_uNetworkHandler || !g_profile) return;
    if (g_profile->family != kFamilyInterlude) return;
    if (!g_profile->rvaUNHGetUserByID || !g_profile->rvaUserGetName) return;
    HMODULE hEng = GetModuleHandleW(L"engine.dll");
    if (!hEng) return;
    using PFN_GetUserByID = void* (__fastcall*)(void* This, void* /*edx*/, int id);
    using PFN_GetName     = wchar_t* (__fastcall*)(void* This, void* /*edx*/);
    auto getUser = (PFN_GetUserByID)((uintptr_t)hEng + g_profile->rvaUNHGetUserByID);
    auto getName = (PFN_GetName)    ((uintptr_t)hEng + g_profile->rvaUserGetName);
    Logf("=== Enumerating UNetworkHandler known users ===");
    int found = 0;
    for (int id = 0; id < 200; id++) {
        void* u = nullptr;
        __try { u = getUser(g_uNetworkHandler, nullptr, id); }
        __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!u) continue;
        wchar_t* name = nullptr;
        __try { name = getName(u, nullptr); }
        __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!name) { Logf("  ID=%d User*=%p name=(null)", id, u); found++; continue; }
        int n = SafeMeasureWideLen(name, 63);
        if (n <= 0) { Logf("  ID=%d User*=%p name=<invalid>", id, u); found++; continue; }
        wchar_t copy[64] = {};
        memcpy(copy, name, n * sizeof(wchar_t));
        copy[n] = 0;
        Logf("  ID=%d User*=%p name='%ls'", id, u, copy);
        found++;
    }
    Logf("=== Done. Found %d known users ===", found);
}
// Re-poll level + classId once per second while in-world. Updates the
// title whenever either value changes. Cheap: one memory read + one
// virtual function call per second. SEH-wrapped so a stale User* won't
// crash us if the engine recycled the object between polls.
static void PollPlayerExtras() {
    if (!g_localUser || !g_profile) return;
    if (g_currentPlayerName[0] == 0) return;
    if (g_extrasDelayFrames > 0) return;
    bool changed = false;
    // Level re-read (direct memory access at the cached offset).
    if (g_guessedLevelOffset > 0) {
        unsigned int lvl = 0;
        __try { lvl = *(volatile unsigned int*)((char*)g_localUser + g_guessedLevelOffset); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return; }
        if (lvl != g_lastPolledLevel && lvl >= 1 && lvl <= 120) {
            Logf("Level changed: %u -> %u", g_lastPolledLevel, lvl);
            g_lastPolledLevel = lvl;
            changed = true;
        }
    }
    // Class ID re-read (via getter).
    HMODULE hEng = GetModuleHandleW(L"engine.dll");
    if (hEng) {
        uintptr_t rva = (g_profile->family == kFamilyEssence)
            ? g_profile->rvaEngineUserGetCurrentClassType
            : g_profile->rvaUserGetCurrentClassType;
        if (rva) {
            using PFN_GetType = int (__fastcall*)(void* This, void* /*edx*/);
            auto getType = (PFN_GetType)((uintptr_t)hEng + rva);
            int classId = -1;
            __try { classId = getType(g_localUser, nullptr); }
            __except(EXCEPTION_EXECUTE_HANDLER) { classId = -1; }
            if (classId >= 0 && classId != g_currentClassId) {
                Logf("Class changed: %d -> %d", g_currentClassId, classId);
                g_currentClassId = classId;
                const wchar_t* name = GetClassNameById(classId);
                if (name) {
                    int cn = SafeMeasureWideLen(name, 63);
                    memcpy(g_currentPlayerClass, name, cn * sizeof(wchar_t));
                    g_currentPlayerClass[cn] = 0;
                } else {
                    _snwprintf_s(g_currentPlayerClass, _countof(g_currentPlayerClass),
                                 _TRUNCATE, L"Cls.%d", classId);
                }
                changed = true;
            }  // ← ADD THIS CLOSING BRACE
        }
    }
    if (changed) {
        SetWindowTitleWithPlayer();
    }
}
// -----------------------------------------------------------------------------
// Hook install / uninstall
// -----------------------------------------------------------------------------
static LRESULT CALLBACK SubclassWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

static HRESULT __stdcall hkEndScene(IDirect3DDevice9* device) {
    if (!g_imguiReady) {
        return ((EndScene_t)g_origEndScene)(device);
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    BuildOverlayUI();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    return ((EndScene_t)g_origEndScene)(device);
}

static HRESULT __stdcall hkReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    if (g_imguiReady) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
    }
    HRESULT hr = ((Reset_t)g_origReset)(device, pp);
    if (g_imguiReady && SUCCEEDED(hr)) {
        ImGui_ImplDX9_Init(device);
        ImGui_ImplWin32_Init(g_hostHwnd);
    }
    return hr;
}

// ... (restante das funções de hook e instalação)

} // namespace
