// Per-client RVA tables. Selected at runtime by reading the PE TimeDateStamp
// of a probe module (NWindow.dll for the Essence family, engine.dll for
// Interlude-era clients).
//
// Two families:
//   Essence  — modern L2 (GFx Scaleform UI). Login goes through NWindow.dll's
//              UUIScript natives + internal AuthLogin.
//   Interlude — legacy L2 (native xdat UI). Login is a virtual method on
//              UNetworkHandler living in engine.dll, called via __thiscall.

#pragma once
#include <cstdint>

enum ClientFamily {
    kFamilyEssence  = 0,
    kFamilyInterlude = 1,
};

struct ClientProfile {
    const wchar_t* probeModule;        // L"NWindow.dll" or L"engine.dll"
    uint32_t       probeTimestamp;     // IMAGE_NT_HEADERS.TimeDateStamp
    const char*    label;
    ClientFamily   family;

    // -------- Essence-only (zero for Interlude) ----------------------------
    // All RVAs are inside NWindow.dll.
    uintptr_t rvaAuthLoginInternal;        // bool __stdcall(wchar*, wchar*, int)
    uintptr_t rvaExecGotoLogin;            // UUIScript::execGotoLogin
    uintptr_t rvaExecRequestLoginServer;   // UUIScript::execRequestLoginServer
    uintptr_t rvaExecShowWindowGFx;        // UGFxUIScript::execShowWindow
    uintptr_t rvaAutoclassNCLobbyWnd;      // *UClass for NCLobbyWnd (post-login lobby)
    uintptr_t rvaExecSetEulaText;          // UGFxUIScript::SetEulaText — EULA shown
    uintptr_t rvaExecEulaAgree;            // UUIScript::EulaAgree — EULA closed
    uintptr_t rvaAutoclassNCEulaWnd;       // *UClass for NCEulaWnd (EULA dialog base)
    uintptr_t rvaUUIEventManagerExecuteEvent;  // internal ExecuteEvent(int eventID, FString*)
    // Char name capture (Essence family). RVAs are in engine.dll, NOT
    // NWindow.dll. Looked up via GetModuleHandleW(L"engine.dll").
    uintptr_t rvaEngineUserSetName;        // User::SetName(wchar_t const*) — fires on char-name set
    uintptr_t rvaEngineRequestEnterWorld;  // UGameEngine::RequestEnterWorld() — "entering world" signal
    uintptr_t rvaEngineUserGetClassNamePointer; // User::GetClassNamePointer() → wchar_t* MESH name (not real class)
    // Proper class name lookup (Essence): GetCurrentClassType + GetClassNameW + FName::operator* (Core.dll)
    uintptr_t rvaEngineUserGetCurrentClassType; // User::GetCurrentClassType() → int
    uintptr_t rvaEngineUserGetClassNameW;       // User::GetClassNameW(int classType) → FName (returned in EAX:EDX)
    uintptr_t rvaCoreFNameDeref;                // FName::operator*() → wchar_t* — RVA in Core.dll
    // Char-select return signal: fires when server confirms /restart (back to char-select)
    uintptr_t rvaEngineOnRestartResponse;       // UGameEngine::OnRestartResponse
    uintptr_t rvaEngineSetCurrentClassType;     // User::SetCurrentClassType(int) — fires on class change

    // -------- Interlude-only (zero for Essence) ----------------------------
    // All RVAs are inside engine.dll.
    uintptr_t rvaUNHInit;                  // UNetworkHandler::Init — captures singleton this
    uintptr_t rvaUNHRequestAuthLogin;      // UNetworkHandler::RequestAuthLogin (__thiscall)
    uintptr_t rvaUNHRequestServerLogin;    // UNetworkHandler::RequestServerLogin (user picked server)
    uintptr_t rvaUNHIsNotYetLogin;         // UNetworkHandler::IsNotYetLogin (state poll)
    uintptr_t rvaUGEAuthSrvSelectOK;       // UGameEngine::OnAuthServerSelectSuccess
    uintptr_t rvaUGEAuthSrvSelectFail;     // UGameEngine::OnAuthServerSelectFail
    uintptr_t rvaUNHRequestServerList;     // UNetworkHandler::RequestServerList — fires right after auth OK,
                                           // before the server-list popup opens. This is the "login confirmed" signal.
    uintptr_t rvaFL2GameDataEulaLoad;      // FL2GameData::EulaLoad — EULA opening
    uintptr_t rvaUGEOnAcceptLogOut;        // UGameEngine::OnAcceptLogOut — user logged out, back to login
    uintptr_t rvaUNHAuthReconnect;         // UNetworkHandler::AuthReconnect — auth reconnect (back to login)
    uintptr_t rvaUL2ConsoleWndSetState;    // UL2ConsoleWnd::SetState(L2ConsoleState) — Interlude UI state machine
    uintptr_t rvaUL2ConsoleWndExecLobbyEvent; // UL2ConsoleWnd::ExecLobbyEvent(wchar_t*,int) — UScript event dispatcher
    uintptr_t rvaLegacyShowWindow;         // UWindowHandle::execShowWindow in legacy NWindow.dll (Interlude)
    uintptr_t rvaLegacyAutoclassNCEulaWnd; // *UClass for NCEulaWnd in legacy NWindow.dll (Interlude)
    uintptr_t rvaLegacyEventOnEvent;       // UUIScript::eventOnEvent(int, FString const&) in legacy NWindow.dll
    uintptr_t rvaLegacyUUIAPIShowWindow;   // UUIAPI_WINDOW::execShowWindow in legacy NWindow.dll (alt UI dispatcher)
    uintptr_t rvaFL2GameDataEulaSave;      // FL2GameData::EulaSave(int) — user clicked Agree(1)/Disagree(0)
    uintptr_t rvaFL2GameDataNoticeLoad;    // FL2GameData::NoticeLoad — Notice dialog open (candidate for EULA)
    // Char name capture (Interlude). RVAs in engine.dll, same module as the
    // existing Interlude hooks.
    uintptr_t rvaUserSetName;              // User::SetName(wchar_t*) — fires on char-name set
    uintptr_t rvaUNHRequestEnterWorld;     // UNetworkHandler::RequestEnterWorldPacket — "entering world" signal
    uintptr_t rvaUserGetClassNamePointer;  // User::GetClassNamePointer() → wchar_t* MESH name (not real class)
    uintptr_t rvaUserGetCurrentClassType;  // User::GetCurrentClassType() → int (Interlude)
    uintptr_t rvaUserGetClassNameW;        // User::GetClassNameW(int) → FName  (Interlude)
    uintptr_t rvaCoreFNameDerefInterlude;  // FName::operator*() → wchar_t* in Interlude's Core.dll
    uintptr_t rvaUGEOnRestartResponse;     // UGameEngine::OnRestartResponse — back to char-select
    uintptr_t rvaUGEOnLevelUpdate;         // UGameEngine::OnLevelUpdate(User*, int) — fires on level change (Interlude)
    uintptr_t rvaUserGetName;              // User::GetName() → wchar_t* — used to fix name after OnLevelUpdate re-bind
    uintptr_t rvaUL2ResetSelectCharInfo;   // UL2ConsoleWnd::ResetSelectCharacterInfo() — char-select list cleared
    uintptr_t rvaUL2SelectedCharacterNum;  // UL2ConsoleWnd::SelectedCharacterNum(int slot) — user picked a char
    uintptr_t rvaUGEOnReceiveCharSelected; // UGameEngine::OnReceiveCharacterSelectedPacket — server confirms char-pick with full info
    uintptr_t rvaUNHGetUserByID;           // UNetworkHandler::GetUser(int id) → User* — used to enumerate known users
    uintptr_t rvaLineagePCSetViewTarget;   // ALineagePlayerController::SetViewTarget(AActor*) — fires on local pawn setup
};

// Builds confirmed by analysis 2026-05-13.
static const ClientProfile kClientProfiles[] = {
    // -------- Essence family (probe NWindow.dll) ---------------------------
    { L"NWindow.dll", 0x6708d7c8, "Essence 474",             kFamilyEssence,
      0x856630, 0x9af0b0, 0x9c19d0, 0x985c30, 0x110efb8, 0x983b80, 0x98eee0, 0x110efd8, 0x939050,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x678f9987, "Essence 509",             kFamilyEssence,
      0x84fca0, 0x9a6ee0, 0x9b8920, 0x97f620, 0x10fbf80, 0x97d570, 0x988970, 0x10fbfa0, 0x933050,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x6422e278, "Essence Assassins",       kFamilyEssence,
      0x883610, 0x9d39b0, 0x9e59f0, 0x9ad800, 0x114e248, 0x9ab7a0, 0x9b6560, 0x114e268, 0x961360,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x68394700, "Essence 520 RoseVein",    kFamilyEssence,
      0x857fd0, 0x9b20f0, 0x9c3ec0, 0x98a310, 0x111bfe8, 0x988260, 0x9938d0, 0x111c008, 0x93e4a0,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x692828e1, "Essence 541 SamuraiCrow", kFamilyEssence,
      0x8770e0, 0x9da350, 0x9eded0, 0x9adfb0, 0x1166e78, 0x9abf00, 0x9b7980, 0x1166e98, 0x961080,
      0x7a52e0,   // rvaEngineUserSetName              (engine.dll 0x6928282b)
      0x896c40,   // rvaEngineRequestEnterWorld
      0x796aa0,   // rvaEngineUserGetClassNamePointer  (mesh — kept for reference, not used)
      0x0c62f0,   // rvaEngineUserGetCurrentClassType
      0x7a1cd0,   // rvaEngineUserGetClassNameW
      0x0114b0,   // rvaCoreFNameDeref                 (Core.dll 0x692827a9)
      0x71f670,   // rvaEngineOnRestartResponse
      0x0c6300,   // rvaEngineSetCurrentClassType
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x69b8ec54, "Essence 557",             kFamilyEssence,
      0x879e50, 0x9ddca0, 0x9f1910, 0x9b10d0, 0x116de90, 0x9af020, 0x9baaa0, 0x116deb0, 0x9641d0,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x5cb5b1d2, "Lucera2 Classic",         kFamilyEssence,
      0x575670, 0x697ca0, 0x6a8750, 0x67c060, 0x0cbc05c, 0x67a0f0, 0x6841b0, 0x0cbc084, 0x633a50,
      0,0,0,0,0,0 },

    // -------- Interlude family (probe engine.dll) --------------------------
    { L"engine.dll", 0x46dbe989, "Lucera TestPatch (Interlude)", kFamilyInterlude,
      0,0,0,0,0,0,0,0,0,                    // no NWindow / EULA / ExecuteEvent RVAs
      0,0,0,0,0,0,0,0,                      // no Essence engine.dll RVAs (8 fields)
      /*UNHInit*/                0x3ab2,
      /*UNHRequestAuthLogin*/    0x5060,
      /*UNHRequestServerLogin*/  0xbb04,
      /*UNHIsNotYetLogin*/       0x113b0,
      /*UGEAuthSrvSelectOK*/     0x4ec1,
      /*UGEAuthSrvSelectFail*/   0xba41,
      /*UNHRequestServerList*/   0x409d,
      /*FL2GameDataEulaLoad*/    0x6c76,
      /*UGEOnAcceptLogOut*/      0x11e5,
      /*UNHAuthReconnect*/       0x905c,
      /*UL2ConsoleWndSetState*/  0xe6a6,
      /*UL2ConsoleWndExecLobbyEvent*/ 0x2bdf,
      /*LegacyShowWindow*/       0x132580,   // in legacy NWindow.dll (timestamp 0x46775bd3)
      /*LegacyAutoclassNCEulaWnd*/ 0x352610,
      /*LegacyEventOnEvent*/     0x07f210,
      /*LegacyUUIAPIShowWindow*/ 0x1120e0,
      /*FL2GameDataEulaSave*/    0x00ed09,
      /*FL2GameDataNoticeLoad*/  0x0098bd,
      /*UserSetName*/            0x000072f7,
      /*UNHRequestEnterWorld*/   0x0000da17,
      /*UserGetClassNamePointer*/ 0x00014f74,
      /*UserGetCurrentClassType*/ 0,           // not separately needed — GetClassNameW takes no args
      /*UserGetClassNameW*/      0x000102df,
      /*CoreFNameDerefInterlude*/ 0x000012c1,  // Core.dll
      /*UGEOnRestartResponse*/   0x000141f5,
      /*UGEOnLevelUpdate*/       0x00002176,
      /*UserGetName*/            0x000106d6,
      /*UL2ResetSelectCharInfo*/ 0x00014fce,
      /*UL2SelectedCharacterNum*/ 0x0001082a,
      /*UGEOnReceiveCharSelected*/ 0x00007e14,
      /*UNHGetUserByID*/         0x00011554,
      /*LineagePCSetViewTarget*/ 0x0000a4c0 },
};

// Read a loaded module's IMAGE_NT_HEADERS.TimeDateStamp.
inline uint32_t GetModulePeTimestamp(void* moduleBase) {
    if (!moduleBase) return 0;
    auto* base = (const unsigned char*)moduleBase;
    uint32_t peOff = *(const uint32_t*)(base + 60);   // e_lfanew
    if (*(const uint32_t*)(base + peOff) != 0x00004550) return 0;  // 'PE\0\0'
    return *(const uint32_t*)(base + peOff + 8);
}

inline const ClientProfile* FindClientProfile() {
    for (const auto& p : kClientProfiles) {
        HMODULE hMod = GetModuleHandleW(p.probeModule);
        if (!hMod) continue;
        if (GetModulePeTimestamp(hMod) == p.probeTimestamp) return &p;
    }
    return nullptr;
}
