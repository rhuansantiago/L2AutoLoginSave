# hollow L2 overlay

In-game ImGui overlay rendered inside L2's D3D9 framebuffer. Provides a
saved-account login UI that calls the native `AuthLogin` directly
(bypassing the L2 client's own login form), with per-install
credential storage encrypted via DPAPI.

Supports multiple L2 builds — runtime detection by NWindow.dll's PE
timestamp picks the right RVA table (see `src/client_profiles.h`).

---

## Install

`l2ui.dll` is loaded into `L2.exe` via an IAT entry pointing at the
named export `L2UI_Init`. One-time setup per install.

### Steps

1. **Download CFF Explorer** if you don't have it.
   https://ntcore.com/?page_id=388 (free, by NTCore / Daniel Pistelli).
2. **Backup L2.exe**:
   ```
   copy "System_en\L2.exe" "System_en\L2.exe.original"
   ```
3. **Open** `System_en\L2.exe` in CFF Explorer.
4. In the left tree, click **Import Adder**.
5. In the right pane, hit **Add** in the bottom-left and choose
   `l2ui.dll` (drop it in `System_en` first if it isn't there yet — the
   build script puts it there by default).
6. With `l2ui.dll` row selected, in the function box on the right type
   `L2UI_Init` and click **+ Add**.
7. Click **Rebuild Import Table**. CFF Explorer shows a confirmation.
8. **File → Save** (overwrites L2.exe — that's why we backed it up).
9. Make sure `System_en\l2ui.dll` exists alongside L2.exe.
10. Launch L2.exe normally.

Python alternative for step 3–8: `python add_import.py L2.exe` (uses
`pefile`).

### Caveats

- **L2.exe is Themida-protected**. Some Themida configurations refuse
  to run a modified L2.exe. If the game silently exits, revert:
  ```
  copy "System_en\L2.exe.original" "System_en\L2.exe"
  ```
  All Essence / Lucera Classic builds tested so far accept the IAT
  addition.

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
