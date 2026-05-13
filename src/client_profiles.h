// Per-client RVA tables. Selected at runtime by hashing NWindow.dll's PE
// header (TimeDateStamp is unique per build and stable across copies).
//
// Each profile maps to a known L2 Essence (or Lucera Classic) build of
// NWindow.dll. RVAs come from disassembling the corresponding NWindow.dll;
// see notes/analyze_nwindow.ps1 for the extraction script.

#pragma once
#include <cstdint>

struct ClientProfile {
    uint32_t    nwindowTimestamp;            // IMAGE_NT_HEADERS.TimeDateStamp
    const char* label;                       // human-readable build label
    uintptr_t   rvaAuthLoginInternal;        // bool __stdcall(wchar_t*, wchar_t*, int)
    uintptr_t   rvaExecGotoLogin;            // UUIScript::execGotoLogin
    uintptr_t   rvaExecRequestLoginServer;   // UUIScript::execRequestLoginServer
    uintptr_t   rvaExecShowWindowGFx;        // UGFxUIScript::execShowWindow
    uintptr_t   rvaAutoclassNCLobbyWnd;      // *UClass for NCLobbyWnd (server-list popup base)
};

// Builds confirmed by analysis 2026-05-13.
static const ClientProfile kClientProfiles[] = {
    { 0x6708d7c8, "Essence 474",            0x856630, 0x9af0b0, 0x9c19d0, 0x985c30, 0x110efb8 },
    { 0x678f9987, "Essence 509",            0x84fca0, 0x9a6ee0, 0x9b8920, 0x97f620, 0x10fbf80 },
    { 0x6422e278, "Essence Assassins",      0x883610, 0x9d39b0, 0x9e59f0, 0x9ad800, 0x114e248 },
    { 0x68394700, "Essence 520 RoseVein",   0x857fd0, 0x9b20f0, 0x9c3ec0, 0x98a310, 0x111bfe8 },
    { 0x692828e1, "Essence 541 SamuraiCrow",0x8770e0, 0x9da350, 0x9eded0, 0x9adfb0, 0x1166e78 },
    { 0x69b8ec54, "Essence 557",            0x879e50, 0x9ddca0, 0x9f1910, 0x9b10d0, 0x116de90 },
    { 0x5cb5b1d2, "Lucera2 Classic",        0x575670, 0x697ca0, 0x6a8750, 0x67c060, 0x0cbc05c },
};

// Read NWindow.dll's IMAGE_NT_HEADERS.TimeDateStamp from its loaded image.
inline uint32_t GetModulePeTimestamp(void* moduleBase) {
    if (!moduleBase) return 0;
    auto* base = (const unsigned char*)moduleBase;
    uint32_t peOff = *(const uint32_t*)(base + 60);   // e_lfanew
    if (*(const uint32_t*)(base + peOff) != 0x00004550) return 0;  // 'PE\0\0'
    return *(const uint32_t*)(base + peOff + 8);      // IMAGE_FILE_HEADER.TimeDateStamp
}

inline const ClientProfile* FindClientProfile(uint32_t timestamp) {
    for (const auto& p : kClientProfiles) {
        if (p.nwindowTimestamp == timestamp) return &p;
    }
    return nullptr;
}
