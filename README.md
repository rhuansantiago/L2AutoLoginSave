# L2AutoLoginSave

In-game saved-account login UI for Lineage II clients. Drop-in DLL
that renders an ImGui panel inside the L2 client's D3D9 frame, lets
you store credentials per-install (DPAPI-encrypted) and dispatches
the native `AuthLogin` path directly — no scripted clicks on the
client's own login form.

**🇧🇷 Versão em português:** [README.pt-BR.md](README.pt-BR.md)

---

## ⚠️ Disclaimer

This project is **not affiliated with, endorsed by, or sponsored by
NCSoft**. "Lineage II" is a trademark of NCSoft Corporation.

This software is intended **exclusively for use on private servers**
that you operate or are authorized to participate in. Using it on
official NCSoft servers may violate their Terms of Service. The
authors take no responsibility for accounts, characters, or actions
taken by third parties using this software.

The repository contains **no copyrighted game assets** — no L2
textures, audio, or game data is redistributed. The DLL hooks into
client DLLs at runtime by RVA; build profiles for additional clients
are contributed as PE TimeDateStamps + offset tables.

Use at your own risk.

---

## Features

- **Saved-account login** — credentials encrypted per Windows user
  via DPAPI (`CryptProtectData`). No plaintext file on disk.
- **Native login dispatch** — calls the client's internal
  `AuthLogin` directly (Essence) or invokes `UNetworkHandler::RequestAuthLogin`
  via __thiscall (Interlude). No fake mouse clicks, no input
  injection.
- **EULA auto-accept** — recognizes the EULA dialog opening (per-build
  signal) and closes it programmatically.
- **Window title** — patches the L2 window title to show character
  name / class / level on Essence clients. (Interlude name capture
  is limited — see [docs/USAGE.md](docs/USAGE.md).)
- **Multi-client** — picks the right RVA table at runtime from the
  PE TimeDateStamp of NWindow.dll (Essence) or engine.dll
  (Interlude). One DLL binary works across all supported builds.
- **Themida-safe** — injects via Engine.dll's IAT, never modifies
  L2.exe (which is Themida-protected and breaks on tamper).

## Supported builds

| Family | Build | Probe DLL | TimeDateStamp |
|--------|-------|-----------|---------------|
| Essence | 474 | NWindow.dll | `0x6708d7c8` |
| Essence | 509 | NWindow.dll | `0x678f9987` |
| Essence | Assassins | NWindow.dll | `0x6422e278` |
| Essence | 520 RoseVein | NWindow.dll | `0x68394700` |
| Essence | 541 SamuraiCrow | NWindow.dll | `0x692828e1` |
| Essence | 557 | NWindow.dll | `0x69b8ec54` |
| Essence (Lucera) | Classic | NWindow.dll | `0x5cb5b1d2` |
| Interlude (Lucera) | TestPatch | engine.dll | `0x46dbe989` |

If your build isn't listed, the DLL still loads but logs
`UNKNOWN NWindow build` — add a row to `src/client_profiles.h` with
the matching RVAs. See [docs/USAGE.md §troubleshooting](docs/USAGE.md).

## Quick start

If you just want to run it:

```bat
:: Build (needs VS2022 + CMake 3.20+)
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

:: Output: build/Release/l2ui.dll  (~150 KB)

:: Install (one-time per L2 install)
copy "L2\System_en\Engine.dll" "L2\System_en\Engine.dll.original"
copy build\Release\l2ui.dll  "L2\System_en\"
python add_import.py "L2\System_en\Engine.dll"

:: Launch L2 normally — the overlay panel appears on the login screen
```

Full step-by-step guides:

- 🇺🇸 [**docs/BUILDING.md**](docs/BUILDING.md) — compile from source
- 🇺🇸 [**docs/USAGE.md**](docs/USAGE.md) — install, configure, use
- 🇧🇷 [**docs/BUILDING.pt-BR.md**](docs/BUILDING.pt-BR.md) — guia de compilação
- 🇧🇷 [**docs/USAGE.pt-BR.md**](docs/USAGE.pt-BR.md) — guia de uso

## How it works

```
┌─────────────┐   IAT redir   ┌───────────────┐   D3D9 EndScene   ┌──────────┐
│  L2.exe     │──────────────►│  Engine.dll   │───────────────────►│ l2ui.dll │
│ (Themida)   │  (untouched)  │  (untouched)  │   MinHook trampo  │  overlay │
└─────────────┘               └───────────────┘                   └──────────┘
                                                                       │
                                                                       │ AuthLogin call
                                                                       │ (FunctionPtr by RVA)
                                                                       ▼
                                                                  ┌──────────┐
                                                                  │ NWindow  │
                                                                  │   .dll   │
                                                                  └──────────┘
```

- A single `L2UI_Init` export is added to Engine.dll's import table
  via CFF Explorer or the bundled Python script. Windows resolves
  the import when Engine.dll loads, which triggers DllMain → our
  hooks install.
- D3D9 `EndScene` / `Present` / `Reset` are hooked with MinHook to
  drive the ImGui frame inside the client's existing back buffer.
- The login button calls the client's internal `AuthLogin` routine
  through a function pointer resolved by RVA at the matched build's
  profile entry. No script side-channel.

## Project layout

```
.
├── LICENSE                  MIT
├── README.md                this file (English)
├── README.pt-BR.md          Portuguese version
├── docs/
│   ├── BUILDING.md          build guide (EN)
│   ├── BUILDING.pt-BR.md    guia de compilação
│   ├── USAGE.md             usage guide (EN)
│   └── USAGE.pt-BR.md       guia de uso
├── CMakeLists.txt           top-level build
├── add_import.py            CFF Explorer alternative (pefile-based)
└── src/
    ├── dllmain.cpp          DllMain, Logf, L2UI_Init export
    ├── d3d9_hook.cpp        D3D9 + NWindow hooks, ImGui frame
    ├── overlay_ui.cpp       account storage (DPAPI per-install)
    ├── overlay_ui.h
    ├── client_profiles.h    per-build RVA table
    └── exports.def          module exports list
```

## Tech stack

| Component | Library | License |
|-----------|---------|---------|
| Overlay | [Dear ImGui](https://github.com/ocornut/imgui) v1.91.0 | MIT |
| Hooking | [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause |
| Render | Win32 D3D9 (system) | — |
| Credentials | Win32 DPAPI (system) | — |

All bundled deps are permissively licensed. No GPL.

## Status

| Feature | Status |
|---------|--------|
| Multi-client RVA routing | ✅ |
| DPAPI-encrypted credentials | ✅ |
| Essence native AuthLogin dispatch | ✅ |
| Interlude UNetworkHandler dispatch | ✅ |
| EULA auto-accept | ✅ |
| Window title (Essence) | ✅ |
| Window title (Lucera Interlude) | ❌ accepted limitation (see USAGE) |
| D3D9 device-recreate survival | ✅ |

## Contributing

PRs welcome. To add a new build:

1. Run `notes/analyze_nwindow.ps1` on the target NWindow.dll (or
   engine.dll for Interlude) — it will print the TimeDateStamp and
   a starting set of RVAs.
2. Add a row to `src/client_profiles.h`.
3. Test, then open a PR with the build identification + a brief
   note on which RVAs you had to fix up.

For larger changes, please open an issue first to discuss scope.
The codebase mixes English and Portuguese comments — English is
preferred for new code.

## License

MIT — see [LICENSE](LICENSE).

Bundled dependencies (ImGui, MinHook) retain their respective
permissive licenses.
