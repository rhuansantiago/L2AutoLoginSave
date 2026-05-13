# hollow L2 overlay — Phase 1 install

Two ways to load `l2ui.dll` into `L2.exe`. Use whichever fits your
deployment.

---

## Method A — CFF Explorer (permanent, no extra launcher)

Adds `l2ui.dll` to L2.exe's import table. After this one-time edit,
launching L2.exe normally (Steam / NCSoft launcher / direct double-click)
auto-loads our DLL.

### Steps

1. **Download CFF Explorer** if you don't have it.
   https://ntcore.com/?page_id=388 (free, by NTCore / Daniel Pistelli).
2. **Backup L2.exe**:
   ```
   copy "System_en\L2.exe" "System_en\L2.exe.original"
   ```
3. **Open** `System_en\L2.exe` in CFF Explorer.
4. In the left tree, click **Import Adder**.
5. In the right pane, hit **Add** in the bottom-left and choose `l2ui.dll`
   (browse to System_en if needed; the DLL should already be there since
   our build script copies it).
6. With `l2ui.dll` row selected, in the function box on the right type
   `L2UI_Init` and click **+ Add**.
7. Click **Rebuild Import Table**. CFF Explorer shows a confirmation.
8. **File → Save** (overwrites L2.exe — that's why we backed it up).
9. Make sure `System_en\l2ui.dll` exists alongside L2.exe (our build
   already copies it; verify via the build log).
10. Launch L2.exe normally. You should see the
    "hollow L2 overlay - Phase 1" MessageBox before the game boots.

### Caveats

- **L2.exe is Themida-protected**. Themida MAY perform IAT integrity
  checks and refuse to run a modified L2.exe (anti-tamper). If you see a
  Themida error or the game silently exits, revert:
  ```
  copy "System_en\L2.exe.original" "System_en\L2.exe"
  ```
  Then use Method B instead.
- Some versions of Themida tolerate IAT additions; others don't. The
  only way to know is to test. Method B is the safe fallback.

---

## Method B — `hollow_l2_inject.exe` (no L2.exe modification)

Spawns L2.exe paused, injects `l2ui.dll` via `CreateRemoteThread +
LoadLibraryW`, resumes. Zero modification to any L2 file.

### Steps

1. Build the project (already done if you can read this).
2. Run:
   ```
   hollow_l2_inject.exe
   ```
   With no args it uses the hard-coded path to L2.exe and l2ui.dll. To
   override:
   ```
   hollow_l2_inject.exe --exe "<path>\L2.exe" --dll "<path>\l2ui.dll"
   ```
   Pass-through args for L2.exe itself: `--args "..."`.

3. Confirmation: the MessageBox appears as soon as the loader executes
   our DllMain (BEFORE Themida unpacks Core/Engine). Click OK, L2 boots.

### Pros / cons of Method B

| | Pro | Con |
|---|---|---|
| No file modification | yes | uses a separate launcher exe |
| Themida-safe | yes (IAT untouched) | requires user to run our exe |
| Survives game updates | yes | each game update doesn't break us |

---

## Verifying injection worked

Either method should produce a log file at:
```
H:\L2_Essence_542_SamuraiCrow\hollow_l2\overlay.log
```

Sample contents (proven working in Phase 1 test with Method B):
```
[ATTACH] pid=28396 tid=19436
host exe : ...System_en\L2.exe
self dll : ...build\overlay\Release\l2ui.dll
modules visible at attach time:
  kernel32.dll   -> 0x758F0000
  user32.dll     -> 0x759F0000
  Core.dll       -> 0x15000000   <- already mapped via IAT
  Engine.dll     -> 0x20000000   <- already mapped via IAT
  Window.dll     -> 0x11000000
  NWindow.dll    -> 0x00000000   <- loaded later by engine
  D3DDrv.dll     -> 0x00000000   <- loaded later
  steam_api.dll  -> 0x65770000
```

The above confirms our DllMain runs **before** Themida's TLS callbacks
fire — main thread is still suspended (Method B) or just starting
(Method A).

## Files in this folder

```
overlay/
├── CMakeLists.txt
├── README.md                  ← this file
└── src/
    ├── dllmain.cpp            ← l2ui.dll source (DllMain + L2UI_Init export)
    └── inject.cpp             ← hollow_l2_inject.exe source

build/overlay/Release/         ← built artifacts
├── l2ui.dll (~118 KB)         ← the DLL — copy to System_en
└── hollow_l2_inject.exe (~360 KB)  ← optional injector
```
