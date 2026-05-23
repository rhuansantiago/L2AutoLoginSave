# Building L2AutoLoginSave

Step-by-step build for `l2ui.dll`.

> 🇧🇷 Versão em português: [BUILDING.pt-BR.md](BUILDING.pt-BR.md)

---

## Prerequisites

| Tool | Required | Tested |
|------|----------|--------|
| Visual Studio 2022 | C++ Desktop workload | 17.10 |
| CMake | ≥ 3.20 | 3.29 |
| Git | any recent | 2.40+ |
| Python (optional) | 3.10+ with `pefile` — for `add_import.py` | 3.11 |

Where to get them:

- Visual Studio 2022 Community → <https://visualstudio.microsoft.com/downloads/> (select **Desktop development with C++**)
- CMake → <https://cmake.org/download/>
- Python → <https://www.python.org/downloads/>

Internet is required for the first `cmake` configure — it fetches
**ImGui v1.91.0** and **MinHook** via FetchContent. ~30 seconds on a
clean checkout; cached afterwards in `build/_deps/`.

---

## Build

The L2 client is **32-bit**, so `l2ui.dll` must be built for
**Win32**.

```bat
git clone https://github.com/luannbr/L2AutoLoginSave.git
cd L2AutoLoginSave
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Output: `build\Release\l2ui.dll` (~150 KB) plus a matching
`l2ui.pdb` for debugging.

### Optional: Debug build

A Release build still ships a full PDB (`/Zi /DEBUG`) and CRT
runtime is static (`/MT`), so the DLL is self-contained — no
`vcruntime140.dll` dependency.

If you want a true Debug binary anyway:

```bat
cmake --build build --config Debug
```

Output: `build\Debug\l2ui.dll`.

---

## Common build errors

| Symptom | Fix |
|---------|-----|
| `unresolved external symbol __imp__` | You configured for x64. Re-run with `-A Win32`. |
| `Cannot find Visual Studio 17 2022` | Install the **Desktop development with C++** workload. |
| FetchContent hang on first configure | Allow up to 1 min on first run; subsequent configures are cached. |
| `vcruntime140.dll missing` at runtime | You changed `CMAKE_MSVC_RUNTIME_LIBRARY` — keep the default `MultiThreaded` (static CRT). |
| `D3DERR_DEVICELOST` after Alt+Enter | You modified L2.exe instead of Engine.dll. Revert L2.exe and inject via Engine.dll's IAT. See [USAGE.md](USAGE.md). |

---

## Where to go next

- 📖 [USAGE.md](USAGE.md) — install and use the DLL
- The L2 client side of the install (IAT patch on Engine.dll) is in
  [USAGE.md](USAGE.md) — read that next.
