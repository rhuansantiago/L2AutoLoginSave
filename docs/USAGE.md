# Using L2AutoLoginSave

Install + operate guide for `l2ui.dll`.

> 🇧🇷 Versão em português: [USAGE.pt-BR.md](USAGE.pt-BR.md)

---

## Concept

`l2ui.dll` is **side-loaded** into the L2 process via an IAT entry
added to **Engine.dll** (NOT L2.exe). When Windows resolves
Engine.dll's imports, our DLL gets loaded; its `L2UI_Init` export
runs, installs D3D9 + NWindow hooks, and the ImGui panel appears
inside the client's frame.

---

## 1. Install (one-time per L2 install)

### 1.1. Place the DLL

Drop `l2ui.dll` into the L2 client's `System_en` folder (next to
`L2.exe`, `Engine.dll`, `NWindow.dll`, etc.):

```bat
copy build\Release\l2ui.dll  "C:\Games\L2\System_en\"
```

### 1.2. Back up Engine.dll

```bat
copy "C:\Games\L2\System_en\Engine.dll" ^
     "C:\Games\L2\System_en\Engine.dll.original"
```

> ⚠️ Keep this backup. The IAT patch is destructive; if anything
> goes wrong, restore it.

### 1.3. Patch Engine.dll's IAT

Two ways — pick one.

#### Option A: Python script (recommended)

```bat
pip install pefile
python add_import.py "C:\Games\L2\System_en\Engine.dll"
```

The script adds an import for `l2ui.dll!L2UI_Init` to Engine.dll's
import table and saves it in place. Read-only verification:

```bat
python -c "import pefile; p=pefile.PE(r'C:\Games\L2\System_en\Engine.dll'); print([d.dll for d in p.DIRECTORY_ENTRY_IMPORT])"
```

You should see `b'l2ui.dll'` in the list.

#### Option B: CFF Explorer (manual)

1. Download CFF Explorer (free, by NTCore):
   <https://ntcore.com/?page_id=388>
2. Open `Engine.dll` in CFF Explorer.
3. Left tree → **Import Adder**.
4. Right pane → **Add** (bottom-left) → choose `l2ui.dll`.
5. With `l2ui.dll` selected, in the function input box type
   `L2UI_Init` and click **+ Add**.
6. Click **Rebuild Import Table**.
7. **File → Save** (overwrites Engine.dll — that's why we backed it
   up).

### 1.4. Launch

Run `L2.exe` normally. On a clean install, the in-game overlay
appears on the login screen.

---

## 2. Why Engine.dll, not L2.exe

L2.exe is **Themida-protected**. Modifying its IAT triggers
Themida's anti-tamper, and on Interlude clients this corrupts the
D3D9 device-recreation path — Alt+Enter (or any in-game resolution
change) fails with `D3DERR_DEVICELOST`, freezing the client.

`Engine.dll` is **not** Themida-protected. L2.exe imports it
naturally, so the load order
L2.exe → Engine.dll → l2ui.dll happens for free.

Don't try to modify L2.exe even if it "seems to work" — the failure
mode only triggers on resolution change.

---

## 3. Verify the DLL loaded

Log file at:

```
%LOCALAPPDATA%\hollow_l2_overlay\overlay.log
```

On Windows 10/11 that resolves to e.g.
`C:\Users\<you>\AppData\Local\hollow_l2_overlay\overlay.log`.

Expected on a clean attach:

```
[ATTACH] pid=12340
host exe : C:\Games\L2\System_en\L2.exe
self dll : C:\Games\L2\System_en\l2ui.dll
EnsureNWindowHooks: matched profile 'Essence 541 SamuraiCrow' (ts=0x692828e1)
MH hook installed (lazy): NWindow!execGotoLogin @ 0x...
MH hook installed (lazy): NWindow!ExecuteEvent @ 0x...
D3D9 EndScene hook installed
```

If you see `UNKNOWN NWindow build (TimeDateStamp=0x...)`, your
client isn't in the profile table yet — see [§troubleshooting](#troubleshooting).

---

## 4. Using the overlay

### Login screen

The panel appears automatically when the L2 login screen opens.

- **Add account** → enter username + password → **Save**. Credentials
  are encrypted via DPAPI (`CryptProtectData`) under your Windows
  user, then written to a per-install `accounts.dat` next to the
  DLL. Plaintext never touches disk.
- **Click an account row** → calls `AuthLogin` immediately. The
  client receives the auth response on the next network tick.
- **EULA** appearing right after login? It's auto-accepted on
  Essence (the `execEulaAgree` RVA in the profile fires
  programmatically).

### In-world

Once you're past character select, the panel hides. The DLL is
still loaded; it just keeps the window-title patch active.

#### Window title

On Essence clients, the L2 window title is rewritten to:

```
<charname> [<classname>] Lv.<level>
```

Updates on:
- Char-select (entering world)
- Class change (e.g. subclass swap)
- Level change

On Lucera Interlude, name capture is **disabled** by design — see
the [project's known limitation](https://github.com/luannbr/L2AutoLoginSave#supported-builds).
The title stays at "Lineage II".

---

## 5. Removing it

To uninstall: just restore the Engine.dll backup.

```bat
del "C:\Games\L2\System_en\Engine.dll"
copy "C:\Games\L2\System_en\Engine.dll.original" "C:\Games\L2\System_en\Engine.dll"
```

You can also leave `l2ui.dll` in place — without the IAT entry,
Windows never loads it. The DPAPI-encrypted account store
(`accounts.dat`) stays where it is until you delete it explicitly.

---

## 6. Troubleshooting

### Unknown build (TimeDateStamp not in profile table)

The log line `UNKNOWN NWindow build (TimeDateStamp=0xABCDEF01)`
means the DLL loaded but couldn't pick an RVA set.

To add support:

1. Identify the client family — Essence (modern, GFx Scaleform UI)
   or Interlude (legacy native UI).
2. Open the probe DLL in IDA / Ghidra — `NWindow.dll` for Essence,
   `engine.dll` for Interlude.
3. Look up the RVA list in `src/client_profiles.h`. The comments
   describe each symbol; comparable RVAs in your client are usually
   close to the ones in nearby builds.
4. Add a new row to `kClientProfiles[]` with the TimeDateStamp and
   the filled-in RVAs.
5. Rebuild and test.

PRs welcome — see the README's contributing section.

### Overlay never appears

- Check the log file. If it's empty, the DLL never loaded — your
  IAT patch didn't take.
- Re-run `add_import.py` and verify with the python one-liner in §1.3.
- Make sure you're not using the original (backed-up) Engine.dll by
  mistake.

### Game crashes on launch

- A bad RVA in `client_profiles.h` for your build will dispatch to
  garbage and crash. Revert your profile changes and re-test.
- Themida is **not** the cause for this crash — Themida tampering
  manifests as `D3DERR_DEVICELOST` on resolution change, not
  startup crash.

### Account password autofills but login button does nothing

- The `rvaAuthLoginInternal` for your build is wrong. Find the
  internal `AuthLogin(wchar*, wchar*, int)` function in
  `NWindow.dll`, get its RVA, and update the profile row.

### Alt+Enter freezes / `D3DERR_DEVICELOST`

This means L2.exe (not Engine.dll) was patched. Restore the L2.exe
backup and patch **Engine.dll** instead. Read §2 again.

---

## 7. Privacy & data

The DLL stores account credentials in:

```
<install_dir>\System_en\accounts.dat
```

- Cipher: Win32 DPAPI (`CryptProtectData`) — only your Windows user
  on this PC can decrypt it.
- Not synchronized anywhere. Lives only on disk where you put it.
- The log file (`overlay.log`) **never** contains passwords. It
  logs structural events (hook installation, profile detection,
  scene counters) — no credentials.

If you wipe `accounts.dat`, all saved accounts are gone (no
recovery).

---

## Where to go next

- 📖 [BUILDING.md](BUILDING.md) — build from source
- 📖 [README.md](../README.md) — project overview and supported builds
