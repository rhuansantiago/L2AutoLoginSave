# Usando o L2AutoLoginSave

Guia de instalação + operação do `l2ui.dll`.

> 🇺🇸 English version: [USAGE.md](USAGE.md)

---

## Conceito

`l2ui.dll` é **side-loaded** no processo L2 via uma entrada de IAT
adicionada na **Engine.dll** (NÃO no L2.exe). Quando o Windows
resolve os imports da Engine.dll, nossa DLL é carregada; o export
`L2UI_Init` roda, instala os hooks D3D9 + NWindow, e o painel ImGui
aparece dentro do frame do cliente.

---

## 1. Instalar (uma vez por instalação L2)

### 1.1. Colocar a DLL

Solte o `l2ui.dll` na pasta `System_en` do cliente L2 (ao lado de
`L2.exe`, `Engine.dll`, `NWindow.dll`, etc.):

```bat
copy build\Release\l2ui.dll  "C:\Games\L2\System_en\"
```

### 1.2. Backup da Engine.dll

```bat
copy "C:\Games\L2\System_en\Engine.dll" ^
     "C:\Games\L2\System_en\Engine.dll.original"
```

> ⚠️ Mantenha esse backup. O patch IAT é destrutivo; se algo der
> errado, restaure.

### 1.3. Patchear o IAT da Engine.dll

Duas formas — escolha uma.

#### Opção A: Script Python (recomendado)

```bat
pip install pefile
python add_import.py "C:\Games\L2\System_en\Engine.dll"
```

O script adiciona um import pra `l2ui.dll!L2UI_Init` na tabela de
imports da Engine.dll e salva no lugar. Verificação read-only:

```bat
python -c "import pefile; p=pefile.PE(r'C:\Games\L2\System_en\Engine.dll'); print([d.dll for d in p.DIRECTORY_ENTRY_IMPORT])"
```

Você deve ver `b'l2ui.dll'` na lista.

#### Opção B: CFF Explorer (manual)

1. Baixe o CFF Explorer (grátis, by NTCore):
   <https://ntcore.com/?page_id=388>
2. Abra `Engine.dll` no CFF Explorer.
3. Árvore esquerda → **Import Adder**.
4. Painel direito → **Add** (canto inferior esquerdo) → escolha `l2ui.dll`.
5. Com `l2ui.dll` selecionado, na caixa de função à direita digite
   `L2UI_Init` e clique **+ Add**.
6. Clique **Rebuild Import Table**.
7. **File → Save** (sobrescreve a Engine.dll — por isso o backup).

### 1.4. Lançar

Rode o `L2.exe` normalmente. Numa instalação limpa, o overlay
in-game aparece na tela de login.

---

## 2. Por que Engine.dll e não L2.exe

O L2.exe tem proteção **Themida**. Modificar o IAT dele dispara o
anti-tamper do Themida e, em clientes Interlude, isso corrompe o
caminho de device-recreation do D3D9 — Alt+Enter (ou qualquer
mudança de resolução in-game) falha com `D3DERR_DEVICELOST` e
trava o cliente.

A `Engine.dll` **não** tem Themida. O L2.exe importa ela
naturalmente, então a ordem de load
L2.exe → Engine.dll → l2ui.dll acontece de graça.

Não tente modificar o L2.exe mesmo que "pareça funcionar" — o modo
de falha só dispara ao mudar resolução.

---

## 3. Verificar que a DLL carregou

Arquivo de log em:

```
%LOCALAPPDATA%\hollow_l2_overlay\overlay.log
```

No Windows 10/11 isso resolve pra algo tipo
`C:\Users\<vc>\AppData\Local\hollow_l2_overlay\overlay.log`.

Esperado em um attach limpo:

```
[ATTACH] pid=12340
host exe : C:\Games\L2\System_en\L2.exe
self dll : C:\Games\L2\System_en\l2ui.dll
EnsureNWindowHooks: matched profile 'Essence 541 SamuraiCrow' (ts=0x692828e1)
MH hook installed (lazy): NWindow!execGotoLogin @ 0x...
MH hook installed (lazy): NWindow!ExecuteEvent @ 0x...
D3D9 EndScene hook installed
```

Se aparecer `UNKNOWN NWindow build (TimeDateStamp=0x...)`, seu
cliente ainda não está na tabela de profile — veja
[§troubleshooting](#troubleshooting).

---

## 4. Usando o overlay

### Tela de login

O painel aparece automaticamente quando a tela de login do L2 abre.

- **Add account** → digite usuário + senha → **Save**. As
  credenciais são cifradas via DPAPI (`CryptProtectData`) sob seu
  usuário Windows, daí gravadas num `accounts.dat` por instalação
  ao lado da DLL. Texto puro nunca toca o disco.
- **Clique em uma linha de conta** → chama `AuthLogin`
  imediatamente. O cliente recebe a resposta de auth no próximo
  tick de rede.
- **EULA** aparece logo após o login? É auto-aceito no Essence (o
  RVA `execEulaAgree` do profile dispara programaticamente).

### In-world

Depois do char-select, o painel se esconde. A DLL continua
carregada; só mantém o patch do título da janela ativo.

#### Título da janela

Em clientes Essence, o título da janela L2 é reescrito pra:

```
<charname> [<classname>] Lv.<level>
```

Atualiza em:
- Char-select (entrando no mundo)
- Mudança de classe (ex: subclass swap)
- Mudança de level

No Lucera Interlude, a captura de nome é **desabilitada** por design
— veja a [limitação conhecida do projeto](https://github.com/luannbr/L2AutoLoginSave#builds-suportados).
O título fica em "Lineage II".

---

## 5. Removendo

Pra desinstalar: só restaure o backup da Engine.dll.

```bat
del "C:\Games\L2\System_en\Engine.dll"
copy "C:\Games\L2\System_en\Engine.dll.original" "C:\Games\L2\System_en\Engine.dll"
```

Você pode também deixar o `l2ui.dll` no lugar — sem a entrada IAT,
o Windows nunca carrega ele. O armazenamento de contas cifrado por
DPAPI (`accounts.dat`) fica onde está até você apagar
explicitamente.

---

## 6. Troubleshooting

### Build desconhecido (TimeDateStamp não está na tabela)

A linha de log `UNKNOWN NWindow build (TimeDateStamp=0xABCDEF01)`
quer dizer que a DLL carregou mas não conseguiu escolher um conjunto
de RVAs.

Pra adicionar suporte:

1. Identifique a família do cliente — Essence (moderno, UI GFx
   Scaleform) ou Interlude (legado, UI nativa).
2. Abra a DLL de probe no IDA / Ghidra — `NWindow.dll` pro Essence,
   `engine.dll` pro Interlude.
3. Veja a lista de RVAs em `src/client_profiles.h`. Os comentários
   descrevem cada símbolo; RVAs comparáveis no seu cliente
   geralmente ficam perto das dos builds vizinhos.
4. Adicione uma linha nova em `kClientProfiles[]` com o
   TimeDateStamp e os RVAs preenchidos.
5. Rebuild e teste.

PRs bem-vindos — veja a seção de contribuição do README.

### Overlay nunca aparece

- Verifique o log file. Se estiver vazio, a DLL nunca carregou —
  seu patch IAT não pegou.
- Re-rode o `add_import.py` e verifique com o one-liner Python em §1.3.
- Garanta que não está usando a Engine.dll original (do backup) por
  engano.

### Jogo crasha ao iniciar

- Um RVA errado em `client_profiles.h` pro seu build vai dispatchar
  pra lixo e crashar. Reverta suas mudanças de profile e teste de novo.
- O Themida **não** é a causa desse crash — tamper Themida se
  manifesta como `D3DERR_DEVICELOST` na mudança de resolução, não
  crash no startup.

### Senha auto-preenche mas o botão de login não faz nada

- O `rvaAuthLoginInternal` pro seu build está errado. Ache a função
  interna `AuthLogin(wchar*, wchar*, int)` na `NWindow.dll`, pegue o
  RVA dela e atualize a linha do profile.

### Alt+Enter trava / `D3DERR_DEVICELOST`

Isso quer dizer que o L2.exe (não a Engine.dll) foi patcheado.
Restaure o backup do L2.exe e patcheie a **Engine.dll**. Releia §2.

---

## 7. Privacidade & dados

A DLL armazena credenciais de conta em:

```
<install_dir>\System_en\accounts.dat
```

- Cipher: Win32 DPAPI (`CryptProtectData`) — só seu usuário Windows
  nesse PC consegue decifrar.
- Não é sincronizado em lugar nenhum. Mora só no disco onde você
  colocou.
- O log file (`overlay.log`) **nunca** contém senhas. Loga eventos
  estruturais (instalação de hook, detecção de profile, contadores
  de scene) — sem credenciais.

Se você apagar `accounts.dat`, todas as contas salvas somem (sem
recuperação).

---

## Próximos passos

- 📖 [BUILDING.pt-BR.md](BUILDING.pt-BR.md) — compilar do zero
- 📖 [README.pt-BR.md](../README.pt-BR.md) — visão geral do projeto e builds suportados
