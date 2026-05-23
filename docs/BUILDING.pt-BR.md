# Compilando o L2AutoLoginSave

Build passo-a-passo do `l2ui.dll`.

> 🇺🇸 English version: [BUILDING.md](BUILDING.md)

---

## Pré-requisitos

| Ferramenta | Necessário | Testado |
|------------|------------|---------|
| Visual Studio 2022 | Workload C++ Desktop | 17.10 |
| CMake | ≥ 3.20 | 3.29 |
| Git | qualquer versão recente | 2.40+ |
| Python (opcional) | 3.10+ com `pefile` — pro `add_import.py` | 3.11 |

Onde baixar:

- Visual Studio 2022 Community → <https://visualstudio.microsoft.com/downloads/> (marque **Desenvolvimento para desktop com C++**)
- CMake → <https://cmake.org/download/>
- Python → <https://www.python.org/downloads/>

Internet é necessária no primeiro `cmake configure` — ele baixa
**ImGui v1.91.0** e **MinHook** via FetchContent. ~30 segundos em
uma checkout limpa; fica em cache depois em `build/_deps/`.

---

## Build

O cliente L2 é **32 bits**, então o `l2ui.dll` precisa ser buildado
pra **Win32**.

```bat
git clone https://github.com/luannbr/L2AutoLoginSave.git
cd L2AutoLoginSave
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Saída: `build\Release\l2ui.dll` (~150 KB) mais um `l2ui.pdb` pra
debug.

### Opcional: Build Debug

Um build Release ainda ship PDB completo (`/Zi /DEBUG`) e o runtime
CRT é estático (`/MT`), então a DLL é self-contained — sem
dependência de `vcruntime140.dll`.

Se você quiser binário Debug de verdade mesmo assim:

```bat
cmake --build build --config Debug
```

Saída: `build\Debug\l2ui.dll`.

---

## Erros comuns

| Sintoma | Solução |
|---------|---------|
| `unresolved external symbol __imp__` | Você configurou pra x64. Re-rode com `-A Win32`. |
| `Cannot find Visual Studio 17 2022` | Instale o workload **Desenvolvimento para desktop com C++**. |
| FetchContent travado na primeira config | Espere ~1 min na primeira; reconfigs ficam em cache. |
| `vcruntime140.dll missing` em runtime | Você mudou o `CMAKE_MSVC_RUNTIME_LIBRARY` — mantenha o default `MultiThreaded` (CRT estático). |
| `D3DERR_DEVICELOST` depois do Alt+Enter | Você modificou o L2.exe em vez da Engine.dll. Reverta o L2.exe e injete pelo IAT da Engine.dll. Veja [USAGE.pt-BR.md](USAGE.pt-BR.md). |

---

## Próximos passos

- 📖 [USAGE.pt-BR.md](USAGE.pt-BR.md) — instalar e usar a DLL
- O lado do cliente L2 (patch IAT na Engine.dll) está no
  [USAGE.pt-BR.md](USAGE.pt-BR.md) — leia esse a seguir.
