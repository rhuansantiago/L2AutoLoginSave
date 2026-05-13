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
    uintptr_t rvaAutoclassNCLobbyWnd;      // *UClass for NCLobbyWnd

    // -------- Interlude-only (zero for Essence) ----------------------------
    // All RVAs are inside engine.dll.
    uintptr_t rvaUNHInit;                  // UNetworkHandler::Init — captures singleton this
    uintptr_t rvaUNHRequestAuthLogin;      // UNetworkHandler::RequestAuthLogin (__thiscall)
    uintptr_t rvaUNHRequestServerLogin;    // UNetworkHandler::RequestServerLogin
    uintptr_t rvaUNHIsNotYetLogin;         // UNetworkHandler::IsNotYetLogin (state poll)
    uintptr_t rvaUGEAuthSrvSelectOK;       // UGameEngine::OnAuthServerSelectSuccess
    uintptr_t rvaUGEAuthSrvSelectFail;     // UGameEngine::OnAuthServerSelectFail
};

// Builds confirmed by analysis 2026-05-13.
static const ClientProfile kClientProfiles[] = {
    // -------- Essence family (probe NWindow.dll) ---------------------------
    { L"NWindow.dll", 0x6708d7c8, "Essence 474",             kFamilyEssence,
      0x856630, 0x9af0b0, 0x9c19d0, 0x985c30, 0x110efb8,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x678f9987, "Essence 509",             kFamilyEssence,
      0x84fca0, 0x9a6ee0, 0x9b8920, 0x97f620, 0x10fbf80,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x6422e278, "Essence Assassins",       kFamilyEssence,
      0x883610, 0x9d39b0, 0x9e59f0, 0x9ad800, 0x114e248,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x68394700, "Essence 520 RoseVein",    kFamilyEssence,
      0x857fd0, 0x9b20f0, 0x9c3ec0, 0x98a310, 0x111bfe8,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x692828e1, "Essence 541 SamuraiCrow", kFamilyEssence,
      0x8770e0, 0x9da350, 0x9eded0, 0x9adfb0, 0x1166e78,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x69b8ec54, "Essence 557",             kFamilyEssence,
      0x879e50, 0x9ddca0, 0x9f1910, 0x9b10d0, 0x116de90,
      0,0,0,0,0,0 },
    { L"NWindow.dll", 0x5cb5b1d2, "Lucera2 Classic",         kFamilyEssence,
      0x575670, 0x697ca0, 0x6a8750, 0x67c060, 0x0cbc05c,
      0,0,0,0,0,0 },

    // -------- Interlude family (probe engine.dll) --------------------------
    { L"engine.dll", 0x46dbe989, "Lucera TestPatch (Interlude)", kFamilyInterlude,
      0,0,0,0,0,                            // no NWindow RVAs
      /*UNHInit*/                0x3ab2,
      /*UNHRequestAuthLogin*/    0x5060,
      /*UNHRequestServerLogin*/  0xbb04,
      /*UNHIsNotYetLogin*/       0x113b0,
      /*UGEAuthSrvSelectOK*/     0x4ec1,
      /*UGEAuthSrvSelectFail*/   0xba41 },
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
