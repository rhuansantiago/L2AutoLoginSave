# hollow L2 overlay

In-game ImGui overlay rendered inside L2's D3D9 framebuffer. Provides a
saved-account login UI that calls the native `AuthLogin` directly
(bypassing the L2 client's own login form), with per-install
credential storage encrypted via DPAPI.

Supports multiple L2 builds — runtime detection by NWindow.dll's PE
timestamp picks the right RVA table (see `src/client_profiles.h`).

---

## Install

`l2ui.dll` is loaded into the L2 process via an IAT entry on
**`Engine.dll`** (NOT `L2.exe`). One-time setup per install.

### Why Engine.dll instead of L2.exe

L2.exe is Themida-protected. Modifying its IAT triggers Themida's
anti-tamper checks, which on Interlude clients corrupt the D3D9
device-recreation path — Alt+Enter (or in-game resolution changes)
fails with `D3DERR_DEVICELOST`, leaving the game frozen or with a
zombie window.

`Engine.dll` is **not** Themida-protected. L2.exe imports Engine.dll
naturally, so when Windows loads L2.exe → Engine.dll → Engine.dll's
IAT triggers → our DLL loads. Same effect, no Themida detection.

### Steps

1. **Download CFF Explorer** if you don't have it.
   https://ntcore.com/?page_id=388 (free, by NTCore / Daniel Pistelli).
2. **Backup Engine.dll**:
   ```
   copy "System_en\Engine.dll" "System_en\Engine.dll.original"
   ```
3. Drop `l2ui.dll` into the L2 `System_en` folder (the build script
   puts it there by default).
4. **Open** `System_en\Engine.dll` in CFF Explorer.
5. In the left tree, click **Import Adder**.
6. In the right pane, hit **Add** in the bottom-left and choose
   `l2ui.dll`.
7. With `l2ui.dll` row selected, in the function box on the right type
   `L2UI_Init` and click **+ Add**.
8. Click **Rebuild Import Table**. CFF Explorer shows a confirmation.
9. **File → Save** (overwrites Engine.dll — that's why we backed it up).
10. Launch L2.exe normally.

Python alternative for steps 4–9:
`python add_import.py Engine.dll` (uses `pefile`).

### Caveats

- If something goes wrong, revert: `copy Engine.dll.original Engine.dll`.
- Don't modify L2.exe — Themida will break Alt+Enter exit-fullscreen
  on Interlude builds and may degrade other behavior even on Essence.
- All Essence / Classic / Interlude builds we tested accept the
  Engine.dll IAT addition.

---

## Verifying it loaded

Log file at `%LOCALAPPDATA%\hollow_l2_overlay\overlay.log`:
```
[ATTACH] pid=...
host exe : ...System_en\L2.exe
self dll : ...System_en\l2ui.dll
EnsureNWindowHooks: matched profile '<build name>' (ts=0x...)
MH hook installed (lazy): NWindow!execGotoLogin @ ...
...
```

If you see `UNKNOWN NWindow build (TimeDateStamp=0x...)`, the build
isn't in the profile table yet — run `notes/analyze_nwindow.ps1` on
that NWindow.dll and add the row to `client_profiles.h`.

---

## Files

```
overlay/
├── CMakeLists.txt
├── README.md
├── add_import.py             ← Python alternative to CFF Explorer's
│                               Import Adder
└── src/
    ├── dllmain.cpp           ← DllMain, Logf, L2UI_Init export
    ├── d3d9_hook.cpp         ← D3D9 + NWindow hooks, ImGui frame
    ├── overlay_ui.cpp        ← account storage (DPAPI + per-install)
    ├── overlay_ui.h
    ├── client_profiles.h     ← per-build RVA table
    └── exports.def           ← module exports list
```

Build artifact: `build/overlay/Release/l2ui.dll` (~150 KB).
