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
    // The engine's GetClassNameW(int)→FName route returns the MESH asset
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
    if (g_currentPlayerName[0] == 0) return;   // not in-world yet
    if (g_extrasDelayFrames > 0) return;       // initial fetch still pending
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
            }
        }
    }

    if (changed) SetWindowTitleWithPlayer();
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
    g_authLoginSeen = true;
    if (!g_inOurAuthLogin) {
        AutoCaptureLogin(user, pass);
        Logf("HookAuthLogin (game-initiated) — login submitted (state controlled by ExecuteEvent hook)");
        g_loginInProgress = true;
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
    // Don't touch g_overlayShow here — UUIEventManager::ExecuteEvent
    // (hooked separately) is the authoritative state source. It will
    // fire EV_LoginOK / EV_LoginFail / EV_ShowEula / EV_ServerList in
    // response to the server packet and toggle the overlay correctly.
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
    if (g_eulaShown) return;  // never paint over the EULA dialog

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
    CaptureOriginalWindowTitle();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't persist imgui.ini next to L2.exe
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Don't fight the game over cursor shape. L2 paints its own cursor in
    // the game world; if ImGui's Win32 backend also calls SetCursor each
    // frame, the two flicker against each other. We give up cursor hints
    // (no I-beam in InputText, no resize arrow) for a stable game cursor.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

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
// Forward decl — defined later in the file.
static unsigned int SafeReadU32(const void* p);

// Dump the first 32 DWORDs of a UScript object so we can fingerprint its
// class across sessions (Class pointer is somewhere in there, but its
// offset varies per build).
static void DumpThisHeader(const char* tag, void* This) {
    unsigned int dw[32] = {};
    for (int i = 0; i < 32; ++i) dw[i] = SafeReadU32((char*)This + i * 4);
    HMODULE hNW = GetModuleHandleW(L"NWindow.dll");
    uintptr_t nwBase = (uintptr_t)hNW;
    Logf("%s this=%p dw00-07=%08x %08x %08x %08x %08x %08x %08x %08x",
         tag, This, dw[0],dw[1],dw[2],dw[3],dw[4],dw[5],dw[6],dw[7]);
    Logf("                                dw08-15=%08x %08x %08x %08x %08x %08x %08x %08x",
         dw[8],dw[9],dw[10],dw[11],dw[12],dw[13],dw[14],dw[15]);
    Logf("                                dw16-23=%08x %08x %08x %08x %08x %08x %08x %08x",
         dw[16],dw[17],dw[18],dw[19],dw[20],dw[21],dw[22],dw[23]);
    Logf("                                dw24-31=%08x %08x %08x %08x %08x %08x %08x %08x",
         dw[24],dw[25],dw[26],dw[27],dw[28],dw[29],dw[30],dw[31]);
    Logf("  NWindow base=%p — slots inside NWindow image (likely class/vtable):", (void*)nwBase);
    for (int i = 0; i < 32; ++i) {
        uintptr_t v = dw[i];
        if (v >= nwBase && v < nwBase + 0x2000000) {
            Logf("    dw[%d]=0x%08x  rva=0x%lx", i, dw[i], (unsigned long)(v - nwBase));
        }
    }
}

void __fastcall HookExecRequestLoginServer(void* This, void* /*edx*/, void* FFrame, void* result) {
    if (!g_serverListWndThis) {
        g_serverListWndThis = This;
        DumpThisHeader("Learned server-list popup", This);
    }
    Logf("HookRequestLoginServer fired — user picked a server → hide overlay");
    g_overlayShow = false;
    g_loginInProgress = false;
    g_origExecRequestLoginServer(This, FFrame, result);
}

// UGFxUIScript::SetEulaText fires while the EULA dialog is being prepared
// (the C++ side populating the text right before/during ShowWindow). Treat
// it as "EULA is on screen" — suppress our overlay until EulaAgree.
void __fastcall HookExecSetEulaText(void* This, void* /*edx*/, void* FFrame, void* result) {
    if (!g_eulaShown) Logf("HookSetEulaText fired — EULA on screen → hide overlay");
    g_eulaShown = true;
    g_origExecSetEulaText(This, FFrame, result);
}
// UUIEventManager::ExecuteEvent — central UScript event dispatcher. Every
// EV_* fires through here. We filter by event ID to drive overlay state.
//
//   EV_LoginBegin     (5630) → show overlay (login screen entered)
//   EV_LoginFail      (5640) → show overlay (auth rejected)
//   EV_LoginFailFlash (5641) → show overlay
//   EV_LoginOK        (5650) → hide overlay (auth confirmed by server)
//   EV_ShowEula       (5680) → hide overlay (EULA dialog opening)
//   EV_ShowChinaEula  (5681) → hide overlay
//   EV_ServerList     (5691) → hide overlay (server-list packet received)
//   EV_ServerListEnd  (5692) → hide overlay (server-list popup shown)
void __fastcall HookExecuteEvent(int eventID, void* param) {
    switch (eventID) {
        case 5630:                   // EV_LoginBegin
        case 5640: case 5641:        // EV_LoginFail*
            Logf("ExecuteEvent: %d (Login%s) → show overlay",
                 eventID, eventID == 5630 ? "Begin" : "Fail");
            g_overlayShow = true;
            g_eulaShown = false;
            RestoreOriginalWindowTitle();
            break;
        case 5650:                   // EV_LoginOK
            Logf("ExecuteEvent: %d (LoginOK) → hide overlay", eventID);
            g_overlayShow = false;
            break;
        case 5680: case 5681:        // EV_ShowEula / EV_ShowChinaEula
            Logf("ExecuteEvent: %d (ShowEula) → hide overlay", eventID);
            g_overlayShow = false;
            g_eulaShown = true;
            break;
        case 5691: case 5692:        // EV_ServerList / EV_ServerListEnd
            Logf("ExecuteEvent: %d (ServerList) → hide overlay", eventID);
            g_overlayShow = false;
            break;
        default:
            break;
    }
    g_origExecuteEvent(eventID, param);
}

// UUIScript::EulaAgree fires when the user clicks accept or decline. In
// either case the EULA window is closing, so it's safe to re-show overlay.
// We also learn `this` (= LogInEula instance) so future ShowWindow calls
// with the same pointer can hide the overlay proactively.
void __fastcall HookExecEulaAgree(void* This, void* /*edx*/, void* FFrame, void* result) {
    if (!g_eulaWndThis) {
        g_eulaWndThis = This;
        DumpThisHeader("Learned EULA dialog", This);
    }
    Logf("HookEulaAgree fired — EULA dismissed → show overlay");
    g_eulaShown = false;
    g_origExecEulaAgree(This, FFrame, result);
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
        // Phase 0: known popups we want to hide for (learned within-session
        // via HookExecEulaAgree / HookExecRequestLoginServer). Skip the
        // pre-auth re-show logic in those cases — we don't want to confuse
        // a popup re-show with a return-to-login.
        if (This == g_eulaWndThis) {
            if (!g_eulaShown) Logf("ShowWindow: known EULA this=%p → hide overlay", This);
            g_eulaShown = true;
            g_origExecShowWindowGFx(This, FFrame, result);
            return;
        }
        if (This == g_serverListWndThis) {
            if (g_overlayShow) Logf("ShowWindow: known server-list this=%p → hide overlay", This);
            g_overlayShow = false;
            g_loginInProgress = false;
            g_origExecShowWindowGFx(This, FFrame, result);
            return;
        }

        HMODULE hNW = GetModuleHandleW(L"NWindow.dll");
        void* ncLobby = nullptr;
        void* ncEula  = nullptr;
        if (hNW) {
            ncLobby = (void*)SafeReadU32((char*)hNW + g_profile->rvaAutoclassNCLobbyWnd);
            if (g_profile->rvaAutoclassNCEulaWnd) {
                ncEula = (void*)SafeReadU32((char*)hNW + g_profile->rvaAutoclassNCEulaWnd);
            }
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

        // Phase 2: check class match against known popup classes. The object's
        // Class pointer is somewhere in the first 64 bytes; scan and compare
        // against autoclassNCLobbyWnd / autoclassNCEulaWnd.
        unsigned int dw[16] = {};
        for (int i = 0; i < 16; ++i) dw[i] = SafeReadU32((char*)This + i * 4);
        for (int i = 0; i < 16; ++i) {
            if (ncLobby && dw[i] == (unsigned int)(uintptr_t)ncLobby) {
                Logf("ShowWindow: NCLobbyWnd class match @+0x%x (this=%p) → hide overlay",
                     i * 4, This);
                g_overlayShow = false;
                g_loginInProgress = false;
                break;
            }
            if (ncEula && dw[i] == (unsigned int)(uintptr_t)ncEula) {
                if (!g_eulaShown) Logf("ShowWindow: NCEulaWnd class match @+0x%x (this=%p) → hide overlay",
                                       i * 4, This);
                g_eulaWndThis = This;  // cache so future shows can skip the scan
                g_eulaShown = true;
                break;
            }
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
        Logf("HookUNHAuthLogin (game-initiated) — submitted; state via UGE::OnAuth* hooks");
    }
    return g_origUNHAuthLogin(This, nullptr, user, pass, otp);
}
int __fastcall HookUNHServerLogin(void* This, void* /*edx*/, void* paramStack) {
    Logf("HookUNHServerLogin — user picked server → hide overlay");
    g_overlayShow = false;
    return g_origUNHServerLogin(This, nullptr, paramStack);
}
// UNetworkHandler::RequestServerList — client calls this right after the
// auth server confirms AC_LOGIN_OK to ask for the server list. The
// server-list popup opens immediately after, so this is THE "login
// confirmed" signal in the Interlude family.
int __fastcall HookUNHRequestServerList(void* This, void* /*edx*/) {
    Logf("HookUNHRequestServerList — auth OK, server-list incoming → hide overlay");
    g_overlayShow = false;
    return g_origUNHRequestServerList(This, nullptr);
}
// FL2GameData::EulaLoad — fires when the EULA dialog opens on Interlude.
// Note: also fires during engine init (loads localized text from disk).
// Time-gate by g_authLoginSeen so we only act on the post-login firing.
int __fastcall HookEulaLoad(void* This, void* /*edx*/, int flag) {
    if (g_authLoginSeen) {
        Logf("HookEulaLoad (flag=%d, post-auth) — EULA dialog opening → hide overlay", flag);
        g_overlayShow = false;
        g_eulaShown = true;
    } else {
        Logf("HookEulaLoad (flag=%d, pre-auth init) — ignored", flag);
    }
    return g_origEulaLoad(This, nullptr, flag);
}

// FL2GameData::EulaSave(int) — fires on user clicking Agree(1) or
// Disagree(0). On Disagree, AuthReconnect will fire right after and
// re-show the overlay; on Agree, RequestServerList confirms hide.
void __fastcall HookEulaSave(void* This, void* /*edx*/, int param) {
    Logf("HookEulaSave (param=%d) — EULA %s clicked",
         param, param ? "AGREE" : "DISAGREE");
    g_origEulaSave(This, nullptr, param);
}

// FL2GameData::NoticeLoad — fires when the Notice/MOTD dialog opens.
// On some builds the EULA is actually a Notice. Time-gate same as EulaLoad.
void __fastcall HookNoticeLoad(void* This, void* /*edx*/) {
    if (g_authLoginSeen) {
        Logf("HookNoticeLoad (post-auth) — Notice dialog opening → hide overlay");
        g_overlayShow = false;
        g_eulaShown = true;
    } else {
        Logf("HookNoticeLoad (pre-auth init) — ignored");
    }
    g_origNoticeLoad(This, nullptr);
}
// User::SetName — capture local char name (gated by g_enteringWorld).
// Two stubs since the two families use different DLL images and different
// trampolines, but body is identical.
void __fastcall HookUserSetName_Essence(void* This, void* /*edx*/, const wchar_t* name) {
    HandlePlayerSetName(This, name);
    g_origUserSetName_Essence(This, nullptr, name);
}
void __fastcall HookUserSetName_Interlude(void* This, void* /*edx*/, const wchar_t* name) {
    HandlePlayerSetName(This, name);
    g_origUserSetName_Interlude(This, nullptr, name);
}

// UGameEngine::OnRestartResponse — fires when the server confirms a
// /restart (user clicked "Restart" to return to char-select). This is the
// clean "leaving game world" signal. Clears the title.
using PFN_OnRestartResponse = void (__fastcall*)(void* This, void* /*edx*/, void* paramStack);
PFN_OnRestartResponse g_origOnRestartResponse_Essence = nullptr;
PFN_OnRestartResponse g_origOnRestartResponse_Interlude = nullptr;
void __fastcall HookOnRestartResponse_Essence(void* This, void* /*edx*/, void* paramStack) {
    Logf("HookOnRestartResponse (Essence) — back to char-select → clear title");
    RestoreOriginalWindowTitle();
    g_origOnRestartResponse_Essence(This, nullptr, paramStack);
}

// Essence User::SetCurrentClassType(int) — fires on class change.
using PFN_SetCurrentClassType = void (__fastcall*)(void* This, void* /*edx*/, int newType);
PFN_SetCurrentClassType g_origSetCurrentClassType = nullptr;
void __fastcall HookSetCurrentClassType(void* This, void* /*edx*/, int newType) {
    if (This == g_localUser && newType != g_currentClassId && g_currentPlayerName[0] != 0) {
        Logf("Class change event (Essence): %d -> %d", g_currentClassId, newType);
        g_currentClassId = newType;
        const wchar_t* nm = GetClassNameById(newType);
        if (nm) {
            int cn = SafeMeasureWideLen(nm, 63);
            memcpy(g_currentPlayerClass, nm, cn * sizeof(wchar_t));
            g_currentPlayerClass[cn] = 0;
        } else {
            _snwprintf_s(g_currentPlayerClass, _countof(g_currentPlayerClass),
                         _TRUNCATE, L"Cls.%d", newType);
        }
        SetWindowTitleWithPlayer();
    }
    g_origSetCurrentClassType(This, nullptr, newType);
}

// Interlude UGameEngine::OnLevelUpdate(User*, int newLevel) — fires for the
// LOCAL player when the engine processes a level packet. We use the very
// first invocation after world entry as the canonical "this is the local
// player's User*" signal — it overrides any wrong SetName capture (e.g.
// a nearby NPC/player whose SetName fired before our local pawn's).
using PFN_OnLevelUpdate = int (__fastcall*)(void* This, void* /*edx*/, void* user, int newLevel);
PFN_OnLevelUpdate g_origOnLevelUpdate = nullptr;
int __fastcall HookOnLevelUpdate(void* This, void* /*edx*/, void* user, int newLevel) {
    // First firing after entering world → trust user as the local player.
    if (!g_localUserVerified && user && g_profile) {
        g_localUserVerified = true;
        if (user != g_localUser) {
            Logf("OnLevelUpdate (Interlude): re-binding local user %p -> %p",
                 g_localUser, user);
            g_localUser = user;
            g_currentClassId = -1;
            g_guessedLevelOffset = 0;
            // Re-fetch the real char name via User::GetName().
            HMODULE hEng = GetModuleHandleW(L"engine.dll");
            if (hEng && g_profile->rvaUserGetName) {
                using PFN_GetName = wchar_t* (__fastcall*)(void* This, void* /*edx*/);
                auto getName = (PFN_GetName)((uintptr_t)hEng + g_profile->rvaUserGetName);
                wchar_t* realName = nullptr;
                __try { realName = getName(user, nullptr); }
                __except(EXCEPTION_EXECUTE_HANDLER) { realName = nullptr; }
                if (realName) {
                    int n = SafeMeasureWideLen(realName, 63);
                    if (n >= 1) {
                        memcpy(g_currentPlayerName, realName, n * sizeof(wchar_t));
                        g_currentPlayerName[n] = 0;
                        Logf("Player name corrected via GetName: '%ls'", g_currentPlayerName);
                    }
                }
            }
            g_extrasDelayFrames = 60;  // re-scan offsets / class on the new User
        }
    }
    if (user == g_localUser && newLevel >= 1 && newLevel <= 120 &&
        (unsigned)newLevel != g_lastPolledLevel) {
        Logf("Level change event (Interlude): %u -> %d", g_lastPolledLevel, newLevel);
        g_lastPolledLevel = (unsigned)newLevel;
        SetWindowTitleWithPlayer();
    }
    return g_origOnLevelUpdate(This, nullptr, user, newLevel);
}
void __fastcall HookOnRestartResponse_Interlude(void* This, void* /*edx*/, void* paramStack) {
    Logf("HookOnRestartResponse (Interlude) — back to char-select → clear title");
    RestoreOriginalWindowTitle();
    g_origOnRestartResponse_Interlude(This, nullptr, paramStack);
}

// "Entering world" signal — flip the flag so the next User::SetName
// captures the local player's name.
void __fastcall HookRequestEnterWorld_Essence(void* This, void* /*edx*/) {
    Logf("HookRequestEnterWorld (Essence) — flagging entering world");
    g_enteringWorld = true;
    g_origRequestEnterWorld_Essence(This, nullptr);
}
int __fastcall HookRequestEnterWorld_Interlude(void* This, void* /*edx*/,
    int a, int* b, unsigned long c, unsigned long d, unsigned long e, unsigned long f) {
    Logf("HookRequestEnterWorldPacket (Interlude) — flagging entering world (pickedSlot=%d, csCount=%d)",
         g_pickedSlot, g_charSelectCount);
    g_enumDumpDelayFrames = 300;  // ~5s @ 60fps — diagnostic dump of UNH users
    // Char-select window closes here. If we captured (User*, name) for the
    // picked slot during char-select, bind it now — the local pawn often
    // re-uses the char-select User instance, so we get the right name+ptr.
    g_charSelectActive = false;
    if (g_pickedSlot >= 0 && g_pickedSlot < g_charSelectCount) {
        const CharSelectEntry& e0 = g_charSelectBuf[g_pickedSlot];
        if (e0.userPtr && e0.name[0]) {
            wcsncpy_s(g_currentPlayerName, _countof(g_currentPlayerName), e0.name, _TRUNCATE);
            g_localUser = e0.userPtr;
            g_extrasDelayFrames = 60;
            g_currentPlayerClass[0] = 0;
            g_currentClassId = -1;
            g_guessedLevelOffset = 0;
            g_lastPolledLevel = 0;
            Logf("Bound local user from char-select slot %d: User*=%p '%ls'",
                 g_pickedSlot, e0.userPtr, e0.name);
            SetWindowTitleWithPlayer();
        }
    }
    g_enteringWorld = true;        // also enable world-side SetName fallback
    g_localUserVerified = false;
    g_viewTargetBound = false;     // allow SetViewTarget to re-bind on world entry
    return g_origRequestEnterWorld_Interlude(This, nullptr, a, b, c, d, e, f);
}

// UL2ConsoleWnd::ResetSelectCharacterInfo — char-select list begins
// loading. Reset the buffer and start capturing SetNames for the chars.
using PFN_ResetSelectCharInfo = void (__fastcall*)(void* This, void* /*edx*/);
PFN_ResetSelectCharInfo g_origResetSelectCharInfo = nullptr;
void __fastcall HookResetSelectCharInfo(void* This, void* /*edx*/) {
    Logf("ResetSelectCharacterInfo — char-select active, capture window open");
    g_charSelectCount = 0;
    g_charSelectActive = true;
    g_pickedSlot = -1;
    memset(g_charSelectBuf, 0, sizeof(g_charSelectBuf));
    g_origResetSelectCharInfo(This, nullptr);
}

// UL2ConsoleWnd::SelectedCharacterNum(int slot) — user clicked a char.
using PFN_SelectedCharacterNum = void (__fastcall*)(void* This, void* /*edx*/, int slot);
PFN_SelectedCharacterNum g_origSelectedCharacterNum = nullptr;
void __fastcall HookSelectedCharacterNum(void* This, void* /*edx*/, int slot) {
    Logf("SelectedCharacterNum: slot=%d", slot);
    g_pickedSlot = slot;
    g_origSelectedCharacterNum(This, nullptr, slot);
}

// ALineagePlayerController::SetViewTarget(AActor*) — fires when the engine
// sets the camera target on the local player controller. Right after
// world entry, the engine sets the view target to OUR local pawn (an
// AUser instance). That AActor* IS the local user we want.
using PFN_SetViewTarget = void (__fastcall*)(void* This, void* /*edx*/, void* target);
PFN_SetViewTarget g_origSetViewTarget = nullptr;
void __fastcall HookSetViewTarget(void* This, void* /*edx*/, void* target) {
    // Disabled — Interlude title customization is dormant (see HandlePlayerSetName).
    g_origSetViewTarget(This, nullptr, target);
    return;
    static int s_count = 0;
    if (++s_count <= 20) {
        Logf("SetViewTarget#%d: PC=%p target=%p", s_count, This, target);
    }
    // Bind the FIRST non-null target as the local pawn. SetViewTarget is
    // AUTHORITATIVE — overrides any earlier wrong SetName capture (NPC).
    // Read the Pawn pointer at PC+0x150 (verified offset for Lucera
    // Interlude). The earlier PC-scan diagnostic found the local pawn
    // there returning a placeholder name "----" (engine hasn't received
    // the name packet yet). We DON'T call GetName here — re-entrant calls
    // on partially-initialized objects crashed the engine. Instead we
    // save the pointer and let FetchPlayerExtras read the name after the
    // 5-second delay, when the engine is stable.
    if (This && !g_viewTargetBound) {
        g_viewTargetBound = true;
        void* pawn = nullptr;
        __try { pawn = *(void**)((char*)This + 0x150); }
        __except(EXCEPTION_EXECUTE_HANDLER) { pawn = nullptr; }
        if (pawn) {
            Logf("Local Pawn from PC+0x150: %p (was g_localUser=%p '%ls')",
                 pawn, g_localUser, g_currentPlayerName);
            g_localUser = pawn;
            g_currentPlayerName[0] = 0;
            g_currentPlayerClass[0] = 0;
            g_currentClassId = -1;
            g_guessedLevelOffset = 0;
            g_lastPolledLevel = 0;
            g_extrasDelayFrames = 300;  // 5s — let engine receive name packet
        }
    }
    if (false && target && !g_viewTargetBound) {
        g_viewTargetBound = true;
        g_localUser = target;
        g_currentPlayerName[0] = 0;     // clear stale NPC name
        g_currentPlayerClass[0] = 0;
        g_currentClassId = -1;
        g_guessedLevelOffset = 0;
        g_lastPolledLevel = 0;
        g_extrasDelayFrames = 60;
        // Read the name via User::GetName
        if (g_profile && g_profile->rvaUserGetName) {
            HMODULE hEng = GetModuleHandleW(L"engine.dll");
            if (hEng) {
                using PFN_GetName = wchar_t* (__fastcall*)(void* This, void* /*edx*/);
                auto getName = (PFN_GetName)((uintptr_t)hEng + g_profile->rvaUserGetName);
                wchar_t* nm = nullptr;
                __try { nm = getName(target, nullptr); }
                __except(EXCEPTION_EXECUTE_HANDLER) { nm = nullptr; }
                if (nm) {
                    int n = SafeMeasureWideLen(nm, 63);
                    if (n >= 1) {
                        memcpy(g_currentPlayerName, nm, n * sizeof(wchar_t));
                        g_currentPlayerName[n] = 0;
                        Logf("Local pawn name from GetName: '%ls'", g_currentPlayerName);
                        SetWindowTitleWithPlayer();
                    } else {
                        Logf("Local pawn GetName returned empty");
                    }
                } else {
                    Logf("Local pawn GetName returned null");
                }
            }
        }
    }
    g_origSetViewTarget(This, nullptr, target);
}

// UGameEngine::OnReceiveCharacterSelectedPacket — server's response to
// char-pick. Likely contains the char's name + spawn info. We dump all
// args + the bytes pointed by the void* arg so we can identify which
// offset holds the wchar_t* name.
using PFN_OnReceiveCharSelected = void (__fastcall*)(
    void* This, void* /*edx*/,
    int a, int b, int c, int d, void* e, int f, int* g);
PFN_OnReceiveCharSelected g_origOnReceiveCharSelected = nullptr;

void __fastcall HookOnReceiveCharSelected(void* This, void* /*edx*/,
    int a, int b, int c, int d, void* ePtr, int f, int* gPtr) {
    Logf("OnReceiveCharSelectedPacket: a=%d b=%d c=%d d=%d e=%p f=%d g=%p",
         a, b, c, d, ePtr, f, gPtr);
    if (ePtr) {
        __try {
            // Hex dump first 96 bytes of *ePtr
            unsigned char buf[96] = {};
            memcpy(buf, ePtr, 96);
            char hex[400] = {};
            int hp = 0;
            for (int i = 0; i < 96 && hp < (int)sizeof(hex) - 6; i++) {
                hp += _snprintf_s(hex + hp, sizeof(hex) - hp, _TRUNCATE,
                                  "%02x%s", buf[i], (i % 4 == 3) ? " " : "");
            }
            Logf("  e[0..96]: %s", hex);
            // Scan for wide strings at every 2-byte aligned offset
            for (int off = 0; off < 80; off += 2) {
                wchar_t* p = (wchar_t*)(buf + off);
                if (p[0] >= 32 && p[0] < 0xFFFE && p[1] >= 32 && p[1] < 0xFFFE) {
                    int n = 0;
                    while (n < 32 && (buf + off + n*2) < (buf + 96) &&
                           p[n] >= 32 && p[n] < 0xFFFE) n++;
                    if (n >= 2) {
                        wchar_t copy[40] = {};
                        memcpy(copy, p, n * sizeof(wchar_t));
                        copy[n] = 0;
                        Logf("  e+0x%02x as wstring: '%ls'", off, copy);
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) { Logf("  e dump SEH'd"); }
    }
    g_origOnReceiveCharSelected(This, nullptr, a, b, c, d, ePtr, f, gPtr);
}

// UGameEngine::OnAcceptLogOut — user accepted the logout confirmation,
// engine is dropping the game session and going back to the login screen.
void __fastcall HookOnAcceptLogOut(void* This, void* /*edx*/) {
    Logf("HookOnAcceptLogOut — logout accepted → show overlay");
    g_overlayShow = true;
    g_eulaShown = false;
    RestoreOriginalWindowTitle();
    g_origOnAcceptLogOut(This, nullptr);
}
// UNetworkHandler::AuthReconnect — auth reconnect (back to login). Fires
// after disconnect / logout when the client re-establishes the auth
// connection.
void __fastcall HookAuthReconnect(void* This, void* /*edx*/) {
    Logf("HookAuthReconnect — back to login phase → show overlay");
    g_overlayShow = true;
    g_eulaShown = false;
    RestoreOriginalWindowTitle();
    g_origAuthReconnect(This, nullptr);
}
// UL2ConsoleWnd::ExecLobbyEvent — UScript event dispatcher with string
// event names. Log every event so we can identify what fires when EULA
// is shown (since EULA doesn't trigger SetState).
void __fastcall HookExecLobbyEvent(void* This, void* /*edx*/, wchar_t* eventName, int param) {
    static const wchar_t* s_lastName = nullptr;
    static int s_lastParam = -999;
    bool changed = (eventName != s_lastName) || (param != s_lastParam);
    if (changed) {
        Logf("HookExecLobbyEvent: '%ls' param=%d", eventName ? eventName : L"(null)", param);
        s_lastName = eventName;
        s_lastParam = param;
    }
    g_origExecLobbyEvent(This, nullptr, eventName, param);
}

// UL2ConsoleWnd::SetState — Interlude UI state machine. Every transition
// in the login pipeline (Login → Eula → ServerSelect → CharSelect → InGame)
// flows through here. State 2 is the login screen (confirmed empirically
// from earlier logs); anything else means we've moved past login, so we
// hide the overlay. Transitions back to 2 re-show it.
constexpr int kInterludeLoginState = 2;
void __fastcall HookSetConsoleState(void* This, void* /*edx*/, int newState) {
    static int s_last = -1;
    if (newState != s_last) {
        Logf("HookSetConsoleState: L2ConsoleWnd state %d -> %d %s",
             s_last, newState,
             newState == kInterludeLoginState ? "(Login → show overlay)"
                                              : "(non-login → hide overlay)");
        if (newState == kInterludeLoginState) {
            g_overlayShow = true;
            g_eulaShown   = false;
        } else {
            g_overlayShow = false;
        }
        s_last = newState;
    }
    g_origSetConsoleState(This, nullptr, newState);
}
int __fastcall HookUGEAuthSrvOK(void* This, void* /*edx*/) {
    Logf("HookUGEAuthSrvOK — login confirmed → hide overlay");
    g_overlayShow = false;
    return g_origUGEAuthSrvOK(This, nullptr);
}
int __fastcall HookUGEAuthSrvFail(void* This, void* /*edx*/, int code) {
    Logf("HookUGEAuthSrvFail (code=%d) → show overlay", code);
    g_overlayShow = true;
    RestoreOriginalWindowTitle();
    return g_origUGEAuthSrvFail(This, nullptr, code);
}

// Legacy NWindow.dll UWindowHandle::execShowWindow — fires for every native
// (non-GFx) UI window in Interlude clients. We scan the `this` object's
// first 64 bytes for its UClass pointer and compare against
// autoclassNCEulaWnd. Match = EULA dialog is opening → hide overlay.
//
// Same approach as the Essence ShowWindow hook (HookExecShowWindowGFx) but
// against the legacy native-UI module instead of NWindow's GFx side.
void __fastcall HookLegacyShowWindow(void* This, void* /*edx*/, void* FFrame, void* result) {
    static LONG s_callCount = 0;
    LONG n = InterlockedIncrement(&s_callCount);
    if (This && g_profile && g_profile->rvaLegacyAutoclassNCEulaWnd) {
        HMODULE hNW = GetModuleHandleW(L"NWindow.dll");
        if (hNW) {
            void* ncEula = (void*)SafeReadU32(
                (char*)hNW + g_profile->rvaLegacyAutoclassNCEulaWnd);
            unsigned int dw[16] = {};
            for (int i = 0; i < 16; ++i) dw[i] = SafeReadU32((char*)This + i * 4);
            bool eulaMatch = false;
            if (ncEula) {
                for (int i = 0; i < 16; ++i) {
                    if (dw[i] == (unsigned int)(uintptr_t)ncEula) {
                        eulaMatch = true;
                        if (!g_eulaShown) {
                            Logf("Legacy ShowWindow: NCEulaWnd class match @+0x%x "
                                 "(this=%p, call#%ld) → hide overlay", i * 4, This, n);
                        }
                        g_overlayShow = false;
                        g_eulaShown   = true;
                        break;
                    }
                }
            }
            // Diagnostic: log first 40 invocations so we can see WHICH classes
            // come through. Each line dumps the class-pointer candidates (any
            // dword in first 64 bytes that points into NWindow.dll).
            if (!eulaMatch && n <= 40) {
                // NWindow.dll is ~3.85MB; treat any dword that points into
                // [base, base+5MB] as a potential UClass pointer and dump
                // it as an offset (so we can grep autoclass dump for it).
                uintptr_t nwBase = (uintptr_t)hNW;
                uintptr_t nwEnd  = nwBase + 0x500000;
                char buf[256] = {};
                int len = 0;
                for (int i = 0; i < 16 && len < (int)sizeof(buf) - 24; ++i) {
                    uintptr_t v = dw[i];
                    if (v >= nwBase && v < nwEnd) {
                        len += _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE,
                                           " +0x%x=NW+0x%x", i * 4,
                                           (unsigned)(v - nwBase));
                    }
                }
                Logf("Legacy ShowWindow call#%ld this=%p ncEula=NW+0x%x classes:%s",
                     n, This, (unsigned)g_profile->rvaLegacyAutoclassNCEulaWnd, buf);
            }
        }
    }
    g_origLegacyShowWindow(This, FFrame, result);
}

// UUIAPI_WINDOW::execShowWindow — alternate dispatcher. Same shape, just
// log each invocation so we see if THIS is what fires for EULA.
void __fastcall HookLegacyUUIAPIShowWindow(void* This, void* /*edx*/, void* FFrame, void* result) {
    static LONG s_n = 0;
    LONG n = InterlockedIncrement(&s_n);
    if (n <= 30) Logf("UUIAPI_WINDOW::ShowWindow call#%ld this=%p", n, This);
    g_origLegacyUUIAPIShowWindow(This, FFrame, result);
}

// UUIScript::eventOnEvent(int eventID, FString const& param) — engine
// dispatches every UI event through this. Log first 60 distinct event IDs
// so we can grep for the EULA-show ID.
void __fastcall HookLegacyEventOnEvent(void* This, void* /*edx*/, int eventID, void* fstr) {
    static LONG s_n = 0;
    LONG n = InterlockedIncrement(&s_n);
    if (n <= 200) Logf("UUIScript::OnEvent call#%ld this=%p eventID=%d", n, This, eventID);
    g_origLegacyEventOnEvent(This, nullptr, eventID, fstr);
}

// ============================================================================
// Hook installation — dispatches by profile family
// ============================================================================
// Exported via the public `EnsureClientHooksPublic()` wrapper at the
// bottom of this file. Idempotent latch via local static.
void EnsureClientHooks() {
    static bool s_done = false;
    if (s_done) return;
    if (!ResolveProfileAndModule()) return;

    HMODULE hMod = GetModuleHandleW(g_profile->probeModule);
    if (!hMod) return;
    uintptr_t base = (uintptr_t)hMod;

    // MinHook init is idempotent — D3D9HookInstall also calls it, in either
    // order. We need it here because EnsureClientHooks may run BEFORE
    // D3D9HookInstall (Interlude needs the engine.dll hook in early so the
    // UNH::Init capture isn't missed).
    MH_STATUS mhs = MH_Initialize();
    if (mhs != MH_OK && mhs != MH_ERROR_ALREADY_INITIALIZED) {
        Logf("EnsureClientHooks: MH_Initialize failed %d", (int)mhs);
        return;
    }

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
        installOne(g_profile->rvaExecSetEulaText,        (void*)&HookExecSetEulaText,
                   (void**)&g_origExecSetEulaText,        "NWindow!UGFxUIScript::SetEulaText");
        installOne(g_profile->rvaExecEulaAgree,          (void*)&HookExecEulaAgree,
                   (void**)&g_origExecEulaAgree,          "NWindow!UUIScript::EulaAgree");
        installOne(g_profile->rvaUUIEventManagerExecuteEvent, (void*)&HookExecuteEvent,
                   (void**)&g_origExecuteEvent,           "NWindow!UUIEventManager::ExecuteEvent");
        // Char-name capture lives in engine.dll, NOT NWindow.dll.
        if (g_profile->rvaEngineUserSetName || g_profile->rvaEngineRequestEnterWorld) {
            HMODULE hEng = GetModuleHandleW(L"engine.dll");
            if (!hEng) {
                Logf("Essence: engine.dll not loaded, skipping char-name hooks");
            } else {
                uintptr_t eBase = (uintptr_t)hEng;
                auto installEng = [eBase](uintptr_t rva, void* detour, void** ppOrig, const char* name) {
                    if (!rva) return;
                    void* tgt = (void*)(eBase + rva);
                    MH_STATUS cs = MH_CreateHook(tgt, detour, ppOrig);
                    if (cs != MH_OK) { Logf("MH_CreateHook(%s) FAILED %d", name, (int)cs); return; }
                    MH_STATUS es = MH_EnableHook(tgt);
                    if (es != MH_OK) { Logf("MH_EnableHook(%s) FAILED %d", name, (int)es); return; }
                    Logf("MH hook installed: %s @ %p", name, tgt);
                };
                installEng(g_profile->rvaEngineUserSetName,
                           (void*)&HookUserSetName_Essence,
                           (void**)&g_origUserSetName_Essence,
                           "engine!User::SetName");
                installEng(g_profile->rvaEngineRequestEnterWorld,
                           (void*)&HookRequestEnterWorld_Essence,
                           (void**)&g_origRequestEnterWorld_Essence,
                           "engine!UGameEngine::RequestEnterWorld");
                installEng(g_profile->rvaEngineOnRestartResponse,
                           (void*)&HookOnRestartResponse_Essence,
                           (void**)&g_origOnRestartResponse_Essence,
                           "engine!UGameEngine::OnRestartResponse");
                installEng(g_profile->rvaEngineSetCurrentClassType,
                           (void*)&HookSetCurrentClassType,
                           (void**)&g_origSetCurrentClassType,
                           "engine!User::SetCurrentClassType");
            }
        }
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
        installOne(g_profile->rvaUNHRequestServerList,  (void*)&HookUNHRequestServerList,
                   (void**)&g_origUNHRequestServerList,  "engine!UNH::RequestServerList");
        // FL2GameData::EulaLoad fires at engine init AND (maybe) at EULA
        // dialog show. We hook it but time-gate by g_authLoginSeen to skip
        // the init firing. NoticeLoad is a parallel candidate — Lucera
        // might show EULA as a Notice dialog.
        installOne(g_profile->rvaFL2GameDataEulaLoad,    (void*)&HookEulaLoad,
                   (void**)&g_origEulaLoad,              "engine!FL2GameData::EulaLoad");
        installOne(g_profile->rvaFL2GameDataNoticeLoad,  (void*)&HookNoticeLoad,
                   (void**)&g_origNoticeLoad,            "engine!FL2GameData::NoticeLoad");
        installOne(g_profile->rvaFL2GameDataEulaSave,    (void*)&HookEulaSave,
                   (void**)&g_origEulaSave,              "engine!FL2GameData::EulaSave");
        installOne(g_profile->rvaUGEOnAcceptLogOut,     (void*)&HookOnAcceptLogOut,
                   (void**)&g_origOnAcceptLogOut,        "engine!UGE::OnAcceptLogOut");
        installOne(g_profile->rvaUNHAuthReconnect,      (void*)&HookAuthReconnect,
                   (void**)&g_origAuthReconnect,         "engine!UNH::AuthReconnect");
        installOne(g_profile->rvaUL2ConsoleWndSetState, (void*)&HookSetConsoleState,
                   (void**)&g_origSetConsoleState,       "engine!UL2ConsoleWnd::SetState");
        installOne(g_profile->rvaUL2ConsoleWndExecLobbyEvent, (void*)&HookExecLobbyEvent,
                   (void**)&g_origExecLobbyEvent,        "engine!UL2ConsoleWnd::ExecLobbyEvent");
        // Char-name capture for Interlude — same engine.dll as the rest of
        // this branch's hooks.
        installOne(g_profile->rvaUserSetName,           (void*)&HookUserSetName_Interlude,
                   (void**)&g_origUserSetName_Interlude, "engine!User::SetName");
        installOne(g_profile->rvaUNHRequestEnterWorld,  (void*)&HookRequestEnterWorld_Interlude,
                   (void**)&g_origRequestEnterWorld_Interlude,
                   "engine!UNH::RequestEnterWorldPacket");
        installOne(g_profile->rvaUGEOnRestartResponse,  (void*)&HookOnRestartResponse_Interlude,
                   (void**)&g_origOnRestartResponse_Interlude,
                   "engine!UGameEngine::OnRestartResponse");
        installOne(g_profile->rvaUGEOnLevelUpdate,      (void*)&HookOnLevelUpdate,
                   (void**)&g_origOnLevelUpdate,
                   "engine!UGameEngine::OnLevelUpdate");
        installOne(g_profile->rvaUL2ResetSelectCharInfo, (void*)&HookResetSelectCharInfo,
                   (void**)&g_origResetSelectCharInfo,
                   "engine!UL2ConsoleWnd::ResetSelectCharacterInfo");
        installOne(g_profile->rvaUL2SelectedCharacterNum, (void*)&HookSelectedCharacterNum,
                   (void**)&g_origSelectedCharacterNum,
                   "engine!UL2ConsoleWnd::SelectedCharacterNum");
        installOne(g_profile->rvaUGEOnReceiveCharSelected, (void*)&HookOnReceiveCharSelected,
                   (void**)&g_origOnReceiveCharSelected,
                   "engine!UGameEngine::OnReceiveCharacterSelectedPacket");
        installOne(g_profile->rvaLineagePCSetViewTarget, (void*)&HookSetViewTarget,
                   (void**)&g_origSetViewTarget,
                   "engine!ALineagePlayerController::SetViewTarget");
        // Legacy NWindow.dll is loaded dynamically by Unreal's package
        // loader (it's not a static import of L2.exe in Interlude), so it
        // may not be in the process yet. Install lazily — see
        // EnsureLegacyNWindowHook() called from RenderFrame each frame.
    }
    s_done = true;
}

// Lazy install for the legacy NWindow.dll ShowWindow hook (Interlude only).
// NWindow.dll loads dynamically after engine init, so we retry every frame
// until it appears, then latch.
void EnsureLegacyNWindowHook() {
    static bool s_done = false;
    if (s_done) return;
    if (!g_profile || g_profile->family != kFamilyInterlude ||
        !g_profile->rvaLegacyShowWindow) {
        s_done = true; return;
    }
    HMODULE hLegacyNW = GetModuleHandleW(L"NWindow.dll");
    if (!hLegacyNW) return;  // not loaded yet — try next frame
    void* tgt = (void*)((uintptr_t)hLegacyNW + g_profile->rvaLegacyShowWindow);
    MH_STATUS cs = MH_CreateHook(tgt, (void*)&HookLegacyShowWindow,
                                 (void**)&g_origLegacyShowWindow);
    if (cs != MH_OK) {
        Logf("MH_CreateHook(legacyNW!ShowWindow) FAILED %d", (int)cs);
        s_done = true; return;
    }
    MH_STATUS es = MH_EnableHook(tgt);
    if (es != MH_OK) {
        Logf("MH_EnableHook(legacyNW!ShowWindow) FAILED %d", (int)es);
        s_done = true; return;
    }
    Logf("MH hook installed (lazy): legacyNW!UWindowHandle::ShowWindow @ %p "
         "(NWindow.dll base=%p)", tgt, hLegacyNW);

    // Diagnostic hooks — alt ShowWindow dispatcher + UScript event receiver.
    auto installAt = [&](uintptr_t rva, void* detour, void** ppOrig, const char* name) {
        if (!rva) return;
        void* t = (void*)((uintptr_t)hLegacyNW + rva);
        if (MH_CreateHook(t, detour, ppOrig) != MH_OK) {
            Logf("MH_CreateHook(%s) FAILED", name); return;
        }
        if (MH_EnableHook(t) != MH_OK) {
            Logf("MH_EnableHook(%s) FAILED", name); return;
        }
        Logf("MH hook installed (lazy): %s @ %p", name, t);
    };
    installAt(g_profile->rvaLegacyUUIAPIShowWindow, (void*)&HookLegacyUUIAPIShowWindow,
              (void**)&g_origLegacyUUIAPIShowWindow, "legacyNW!UUIAPI_WINDOW::ShowWindow");
    installAt(g_profile->rvaLegacyEventOnEvent, (void*)&HookLegacyEventOnEvent,
              (void**)&g_origLegacyEventOnEvent, "legacyNW!UUIScript::eventOnEvent");

    s_done = true;
}

void RenderFrame(IDirect3DDevice9* dev) {
    InitImGuiIfNeeded(dev);
    if (!g_imguiReady) return;
    EnsureClientHooks();
    EnsureLegacyNWindowHook();
    if (g_extrasDelayFrames > 0) {
        if (--g_extrasDelayFrames == 0) FetchPlayerExtras();
    }
    if (g_enumDumpDelayFrames > 0) {
        if (--g_enumDumpDelayFrames == 0) EnumerateInterludeUsers();
    }
    // Periodic poll as safety net (~every 10s @ 60 FPS). Real-time updates
    // come from SetCurrentClassType / OnLevelUpdate hooks. This catches
    // edge cases where the event hook doesn't fire for some reason.
    static int s_pollCounter = 0;
    if (++s_pollCounter >= 600) {
        s_pollCounter = 0;
        PollPlayerExtras();
    }

    // Skip ImGui frame processing entirely when the overlay isn't visible
    // (manual hide, EULA on screen, post-login, etc.). The Win32 backend
    // calls SetCursor() during NewFrame regardless of whether anything is
    // drawn, which fights the game's own cursor management — the visible
    // symptom is the in-game cursor flickering between the OS arrow and
    // the game's painted cursor. Skipping the frame stops the fight.
    if (!g_overlayShow || g_eulaShown) return;

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

// Public wrapper for overlay_ui.cpp's InstallWorker. Forwards to the
// anonymous-namespace EnsureClientHooks (same TU, direct call works).
namespace { void EnsureClientHooks(); }
void EnsureClientHooksPublic() { EnsureClientHooks(); }

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
