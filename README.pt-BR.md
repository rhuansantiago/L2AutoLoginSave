# L2AutoLoginSave

UI de login com conta salva, in-game, para clientes Lineage II. DLL
drop-in que renderiza um painel ImGui dentro do frame D3D9 do
cliente, permite armazenar credenciais por instalação (cifradas via
DPAPI) e dispara o caminho nativo `AuthLogin` diretamente — sem
cliques programados no formulário de login do cliente.

**🇺🇸 English version:** [README.md](README.md)

---

## ⚠️ Aviso legal

Este projeto **não é afiliado, endossado nem patrocinado pela
NCSoft**. "Lineage II" é marca registrada da NCSoft Corporation.

Este software se destina **exclusivamente a uso em servidores
privados** que você opere ou esteja autorizado a participar. Usar em
servidores oficiais da NCSoft pode violar os Termos de Serviço
deles. Os autores não se responsabilizam por contas, personagens ou
ações de terceiros usando este software.

O repositório **não contém assets do jogo protegidos por
copyright** — nenhuma textura, áudio ou dado de jogo do L2 é
redistribuído. A DLL faz hook nas DLLs do cliente em runtime via
RVA; profiles de novos builds são contribuídos como TimeDateStamps
PE + tabelas de offsets.

Use por sua conta e risco.

---

## Features

- **Login com conta salva** — credenciais cifradas por usuário do
  Windows via DPAPI (`CryptProtectData`). Nenhum arquivo em texto
  puro em disco.
- **Dispatch nativo de login** — chama a rotina interna `AuthLogin`
  do cliente diretamente (Essence) ou invoca
  `UNetworkHandler::RequestAuthLogin` via __thiscall (Interlude).
  Sem cliques falsos de mouse, sem injeção de input.
- **Auto-aceitar EULA** — reconhece a abertura do diálogo de EULA
  (sinal por build) e fecha programaticamente.
- **Título da janela** — patcha o título da janela do L2 pra mostrar
  nome do personagem / classe / level em clientes Essence. (Captura
  de nome no Interlude é limitada — veja [docs/USAGE.pt-BR.md](docs/USAGE.pt-BR.md).)
- **Multi-cliente** — escolhe a tabela RVA certa em runtime pelo
  TimeDateStamp PE da NWindow.dll (Essence) ou engine.dll
  (Interlude). Um binário só funciona em todos os builds suportados.
- **Themida-safe** — injeta via IAT da Engine.dll, nunca modifica o
  L2.exe (que tem proteção Themida e quebra ao tentar tamper).

## Builds suportados

| Família | Build | DLL probe | TimeDateStamp |
|---------|-------|-----------|---------------|
| Essence | 474 | NWindow.dll | `0x6708d7c8` |
| Essence | 509 | NWindow.dll | `0x678f9987` |
| Essence | Assassins | NWindow.dll | `0x6422e278` |
| Essence | 520 RoseVein | NWindow.dll | `0x68394700` |
| Essence | 541 SamuraiCrow | NWindow.dll | `0x692828e1` |
| Essence | 557 | NWindow.dll | `0x69b8ec54` |
| Essence (Lucera) | Classic | NWindow.dll | `0x5cb5b1d2` |
| Interlude (Lucera) | TestPatch | engine.dll | `0x46dbe989` |

Se seu build não estiver listado, a DLL ainda carrega mas loga
`UNKNOWN NWindow build` — adicione uma linha em
`src/client_profiles.h` com os RVAs correspondentes. Veja
[docs/USAGE.pt-BR.md §troubleshooting](docs/USAGE.pt-BR.md).

## Quick start

Se você só quer rodar:

```bat
:: Buildar (precisa VS2022 + CMake 3.20+)
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

:: Saída: build/Release/l2ui.dll  (~150 KB)

:: Instalar (uma vez por instalação L2)
copy "L2\System_en\Engine.dll" "L2\System_en\Engine.dll.original"
copy build\Release\l2ui.dll  "L2\System_en\"
python add_import.py "L2\System_en\Engine.dll"

:: Inicie o L2 normalmente — o painel do overlay aparece na tela de login
```

Guias passo-a-passo completos:

- 🇧🇷 [**docs/BUILDING.pt-BR.md**](docs/BUILDING.pt-BR.md) — compilar do zero
- 🇧🇷 [**docs/USAGE.pt-BR.md**](docs/USAGE.pt-BR.md) — instalar, configurar, usar
- 🇺🇸 [**docs/BUILDING.md**](docs/BUILDING.md) — build guide (EN)
- 🇺🇸 [**docs/USAGE.md**](docs/USAGE.md) — usage guide (EN)

## Como funciona

```
┌─────────────┐  redir IAT    ┌───────────────┐   D3D9 EndScene   ┌──────────┐
│  L2.exe     │──────────────►│  Engine.dll   │───────────────────►│ l2ui.dll │
│ (Themida)   │  (intocado)   │  (intocado)   │   trampoline      │  overlay │
└─────────────┘               └───────────────┘    MinHook        └──────────┘
                                                                       │
                                                                       │ chamada AuthLogin
                                                                       │ (FunctionPtr por RVA)
                                                                       ▼
                                                                  ┌──────────┐
                                                                  │ NWindow  │
                                                                  │   .dll   │
                                                                  └──────────┘
```

- Um único export `L2UI_Init` é adicionado à tabela de imports da
  Engine.dll via CFF Explorer ou o script Python incluído. O Windows
  resolve o import quando a Engine.dll carrega, o que dispara o
  DllMain → nossos hooks instalam.
- D3D9 `EndScene` / `Present` / `Reset` são hooked com MinHook pra
  dirigir o frame ImGui dentro do back buffer existente do cliente.
- O botão de login chama a rotina interna `AuthLogin` do cliente
  através de um ponteiro de função resolvido por RVA na entrada do
  profile do build identificado. Sem canal lateral via script.

## Estrutura do projeto

```
.
├── LICENSE                  MIT
├── README.md                este arquivo em inglês
├── README.pt-BR.md          versão em português
├── docs/
│   ├── BUILDING.md          guia de build (EN)
│   ├── BUILDING.pt-BR.md    guia de compilação
│   ├── USAGE.md             guia de uso (EN)
│   └── USAGE.pt-BR.md       guia de uso
├── CMakeLists.txt           build top-level
├── add_import.py            alternativa ao CFF Explorer (baseado em pefile)
└── src/
    ├── dllmain.cpp          DllMain, Logf, export L2UI_Init
    ├── d3d9_hook.cpp        hooks D3D9 + NWindow, frame ImGui
    ├── overlay_ui.cpp       armazenamento de conta (DPAPI por instalação)
    ├── overlay_ui.h
    ├── client_profiles.h    tabela RVA por build
    └── exports.def          lista de exports do módulo
```

## Stack tecnológica

| Componente | Biblioteca | Licença |
|------------|------------|---------|
| Overlay | [Dear ImGui](https://github.com/ocornut/imgui) v1.91.0 | MIT |
| Hooking | [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause |
| Render | Win32 D3D9 (sistema) | — |
| Credenciais | Win32 DPAPI (sistema) | — |

Todas as deps bundle têm licenças permissivas. Sem GPL.

## Status

| Feature | Status |
|---------|--------|
| Roteamento RVA multi-cliente | ✅ |
| Credenciais cifradas via DPAPI | ✅ |
| Dispatch nativo AuthLogin no Essence | ✅ |
| Dispatch UNetworkHandler no Interlude | ✅ |
| Auto-aceitar EULA | ✅ |
| Título da janela (Essence) | ✅ |
| Título da janela (Lucera Interlude) | ❌ limitação aceita (ver USAGE) |
| Sobreviver a device-recreate D3D9 | ✅ |

## Contribuindo

PRs bem-vindos. Pra adicionar um novo build:

1. Rode `notes/analyze_nwindow.ps1` na NWindow.dll alvo (ou
   engine.dll pro Interlude) — vai imprimir o TimeDateStamp e um
   conjunto inicial de RVAs.
2. Adicione uma linha em `src/client_profiles.h`.
3. Teste, daí abra um PR com a identificação do build + nota
   breve sobre quais RVAs você teve que ajustar.

Pra mudanças maiores, abra uma issue primeiro pra discutir o
escopo. O codebase mistura comentários em inglês e português —
inglês preferido pra código novo.

## Licença

MIT — veja [LICENSE](LICENSE).

Dependências bundle (ImGui, MinHook) mantêm suas respectivas
licenças permissivas.
