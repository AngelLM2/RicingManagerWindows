# 03 — Arquitetura

## 1. Ideia central em uma frase

> **O usuário descreve o visual desejado (um "rice"); o motor tira um snapshot do estado atual, calcula a diferença, aplica de forma transacional e sabe desfazer.** Tudo o que é pesado, arriscado ou que pode travar roda num processo separado, supervisionado, que pode cair sem levar o resto junto.

## 2. Modelo de processos

```
                       ┌──────────────────────────┐
      usuário ───────► │  rmw-settings.exe  (UI)  │  abre sob demanda; ENCERRA ao fechar
                       └────────────┬─────────────┘
                                    │  IPC (named pipe, só o usuário/sessão atual)
      rmw.exe (CLI) ────────────────┤
                                    ▼
                       ┌──────────────────────────┐
                       │  rmw-core.exe            │  ÚNICO que altera o sistema e os dados
                       │  • RiceEngine + diário   │  (snapshot / plano / aplicar / desfazer)
                       │  • eventos do sistema    │  (monitor, DPI, Explorer, energia, sessão)
                       │  • agendador             │
                       │  • supervisor (Jobs)     │
                       │  • bandeja (se residente)│
                       └──┬──────────┬─────────┬──┘
            cria + IPC    │          │         │
                          ▼          ▼         ▼
            ┌──────────────────┐ ┌────────────────┐ ┌──────────────────┐
            │ rmw-core.exe     │ │ rmw-core.exe   │ │ rmw-wallpaper.exe│
            │ --apply-helper   │ │ --import-worker│ │ D3D11 + Media    │
            │ fala com Explorer│ │ abre .rice/    │ │ Foundation       │
            │ (pode travar →   │ │ .rmwp (entrada │ │ (vídeo atrás dos │
            │  é morto)        │ │ hostil)        │ │  ícones)         │
            └──────────────────┘ └────────────────┘ └──────────────────┘
            ┌──────────────────┐        IPC        ┌──────────────────┐
            │ rmw-bar.exe      │ ◄──────────────── │ rmw-plugin-host  │
            │ barra própria:   │  árvore de widgets│ Luau em sandbox  │
            │ desenha, NUNCA   │  e valores        │ (código da       │
            │ roda plugin      │                   │  comunidade)     │
            └──────────────────┘                   └──────────────────┘
                     (Fase 3+) rmw-pluginhost-native (plugins C++), rmw-elevated (Modo avançado)
```

### 2.1 Regra de ouro: o core é o único escritor

- **UI e CLI são sempre clientes.** Nunca alteram o sistema nem os bancos do core diretamente.
- Cliente não encontrou o pipe → **inicia o `rmw-core`** (com `CREATE_BREAKAWAY_FROM_JOB`, para o core não morrer junto com o terminal/IDE que rodou a CLI) e tenta conectar de novo. Se dois clientes fizerem isso ao mesmo tempo, quem perder a corrida só reconecta.
- **Instância única** do core: mutex nomeado `Local\RicingManagerWindows-<SID>` (o prefixo `Local\` já isola por sessão) + pipe criado com `FILE_FLAG_FIRST_PIPE_INSTANCE`.
- **Aplicações são serializadas**: uma transação por vez; uma segunda recebe `busy` (a UI mostra "aguarde", a CLI pode esperar com `--wait`).
- Isso existe **desde o M1**: o core mínimo (sem bandeja nem supervisor) + IPC + CLI cliente. Não há "trocar de arquitetura" no meio do caminho.

### 2.2 Modo "zero residente"

- Se o rice ativo **não** precisa de nada contínuo (vídeo, agendamento, slideshow por monitor, regras), o core aplica e **encerra após ~30 s ocioso** (sem clientes conectados).
- Entrada de inicialização `HKCU\…\Run` sempre presente: `rmw-core --startup` confere o diário (transação interrompida?) e o contador de modo seguro, inicia o que for residente e **sai em < 1 s** se nada precisar dele. Respeita `StartupApproved` (se o usuário desativou no Gerenciador de Tarefas, não reativamos).
- **Bandeja só enquanto residente** (configuração "manter na bandeja" para quem quiser). Atalhos no menu Iniciar sempre existem: o app e **"Restaurar Windows original"**.

### 2.3 Por que vários processos?

| Motivo | Efeito |
|---|---|
| **Isolamento de crash** | Decodificar vídeo e falar com driver de GPU são as partes mais propensas a crash. Se `rmw-wallpaper` cair, o core reinicia; se cair de novo, aplica um wallpaper estático de fallback. |
| **Isolamento de travamento** | Chamadas ao Explorer (`IDesktopWallpaper`, `IThemeManager2`, broadcasts) podem **travar indefinidamente** se o Explorer travar. Uma thread travada não pode ser cancelada; um processo pode ser encerrado. Por isso essas chamadas rodam no `--apply-helper`, com timeout. |
| **Teto de memória** | Cada worker roda num **Job Object** (detalhes em §2.4). Vazamento → worker encerrado e reiniciado, o PC não sofre. Se o core morrer, os workers morrem junto (`KILL_ON_JOB_CLOSE`). |
| **Leveza real** | A UI só existe aberta; o renderizador só existe com wallpaper animado; o core só existe se necessário. |
| **Segurança** | Entrada não confiável (`.rice`, vídeos) é processada fora do core, com teto de memória. Permite sandbox mais restrito por processo (doc 06). |
| **UI trocável** | A UI é só um cliente IPC: dá para trocar o framework sem tocar no motor. |

### 2.4 Supervisor

- Filho criado **já dentro do Job** (`PROC_THREAD_ATTRIBUTE_JOB_LIST`), com lista explícita de handles herdáveis; handle do Job **não** herdável.
- **Memória:** `JOB_OBJECT_LIMIT_PROCESS_MEMORY` (teto rígido) **não encerra** o processo — só faz as alocações falharem. Por isso o supervisor também registra um **limite de notificação** (`JobObjectNotificationLimitInformation`, entrega garantida) em ~80% do teto; ao receber o aviso (ou o de teto atingido), chama `TerminateJobObject` e reinicia.
- **Crash:** detectado pelo handle do processo + `GetExitCodeProcess` (a mensagem `ABNORMAL_EXIT` do Job não dispara para *fast-fail* `0xC0000409`).
- **Travamento:** cada thread de janela do worker incrementa um contador a cada volta do loop de mensagens; o worker reporta os contadores no heartbeat (a cada 500 ms) e o supervisor também confere `IsHungAppWindow`. **1,5 s sem avanço = travado** → encerrar e reiniciar.
- **Proibido no Job do `rmw-wallpaper`:** `JOB_OBJECT_UILIMIT_HANDLES` (quebraria `SetParent`/mensagens ao Explorer) e `DIE_ON_UNHANDLED_EXCEPTION` (suprime o WER).
- Reinício com *backoff* (1 s, 2 s, 4 s…). **3 falhas em 5 minutos** → feature desativada, fallback aplicado, notificação com link para o log.
- No preset `asan` os limites de memória ficam desligados (o ASan reserva terabytes de memória sombra).
- Metas: crash → de volta em **< 3 s**; travamento → detectado em **< 2 s** e de volta em **< 3 s** depois disso.
- Importante: a janela do wallpaper é filha de uma janela do Explorer. Janelas pai/filha entre processos **anexam as filas de entrada** das duas threads, então **um renderizador travado pode travar o desktop** (há relatos assim no Lively em 25H2). Por isso: uma janela por monitor, cada uma numa thread que **só** processa mensagens (decodificação e renderização em outras threads), e o watchdog age rápido. Nunca encerrar uma árvore de processos que possa incluir o Explorer.

## 3. Camadas do código (dentro de cada processo)

```
 apps/  (executáveis: core [+ modos helper/worker], settings, wallpaper, cli)  ← UI/console só aqui
   │
 engine/  RiceEngine, Journal, Store, Supervisor, Scheduler                    ← orquestra
   │
 modules/ wallpaper, appearance, cursors, taskbar…                             ← regras de cada feature
   │                   usam apenas INTERFACES (portas) ↓
 domain/  Rice, Snapshot, Plan, Validation, MonitorMatch                       ← C++ puro, SEM <windows.h>
   │
 platform/ RegistryWin32, DesktopWallpaperCom, DisplayTopology, Dwm,           ← único lugar que fala
           SystemParams, ThemeManager, ShellEvents, FileSystem…                   com o Win32/COM
   │
 base/    Log, Error, Utf, CheckedCast, Limits, ScopeExit                      ← sem dependências
```

- **`domain` não inclui `<windows.h>`** → testável em milissegundos.
- **Módulos dependem de interfaces** (`IRegistry`, `IWallpaperApi`, `IDisplayTopology`, `IDwm`, `ISystemParams`, `IThemeApi`…). Em produção, implementação Win32; nos testes, *fakes*.

## 4. O contrato de módulo

```cpp
namespace rmw {

enum class Fidelity { Exact, Lossy };      // o snapshot consegue ser restaurado exatamente?

struct ModuleInfo {
    std::string_view                  id;             // "appearance", "cursors", "wallpaper"...
    SemVer                            schemaVersion;  // versão da seção no rice.toml
    std::span<const std::string_view> dependsOn;      // ordem de aplicação (ex.: wallpaper depende de appearance)
    Capabilities                      requires;       // ex.: build mínimo do Windows; nunca "admin"
};

struct CaptureResult {
    ModuleState state;
    Fidelity    fidelity;
    std::string lossyReason;   // ex.: "Windows Spotlight não pode ser restaurado exatamente"
};

class IModule {
public:
    virtual ~IModule() = default;
    virtual ModuleInfo info() const noexcept = 0;

    // --- parte transacional (roda no --apply-helper quando toca o sistema) ---
    virtual std::expected<CaptureResult, Error> capture() = 0;
    virtual std::expected<void, Error>          validate(const ModuleConfig& desired) const = 0;
    virtual std::expected<Plan, Error>          plan(const ModuleState& current,
                                                     const ModuleConfig& desired) const = 0;
    virtual std::expected<void, Error>          apply(const Plan& plan) = 0;   // idempotente
    virtual ModuleConfig                        toConfig(const ModuleState& snapshot) const = 0;
    virtual bool equivalent(const ModuleState& a, const ModuleState& b) const = 0; // ignora valores derivados pelo Windows

    // --- parte residente (roda no core) ---
    virtual bool needsResidency(const ModuleConfig& config) const noexcept = 0; // vídeo: sim; imagem: não
    virtual std::expected<void, Error> activate(const ModuleConfig& config, IHostServices& host) { return {}; }
    virtual void deactivate() noexcept {}

    // Reage a eventos SEM tocar no sistema: só pede uma reaplicação ao motor.
    virtual std::optional<ReapplyRequest> onSystemEvent(const SystemEvent& event) noexcept { return {}; }
};

} // namespace rmw
```

Decisões embutidas no contrato:
- **Restaurar = aplicar.** Não existe `restore()` separado: o motor faz `apply(plan(capture(), toConfig(snapshot)))`. Um caminho de código só, testado duas vezes mais.
- **Eventos nunca alteram o sistema direto**: devolvem um `ReapplyRequest` que o motor executa como transação (fora do histórico).
- **Residência depende da configuração**, não do módulo (o mesmo módulo de wallpaper é residente com vídeo e não-residente com imagem).
- **`IHostServices`** dá ao módulo residente o que ele pode usar: agendar timers, pedir ao supervisor para iniciar/parar um worker, publicar eventos para a UI.
- **Ordem** pelo grafo `dependsOn` (ex.: `appearance` antes de `wallpaper` e `cursors`, porque aplicar um `.theme` pode mexer neles — usamos as flags do `IThemeManager2` para evitar, mas a ordem garante).

Teste de contrato aplicado a **todos** os módulos: `capture() → S0`, aplicar X, `capture() → S1` com `equivalent(S1, X)`, restaurar S0, `equivalent(capture(), S0)`.

## 5. RiceEngine — aplicação transacional

```
aplicar(rice):                                             [só no core, uma transação por vez]
  1. validar o rice inteiro (todos os módulos)              ← nada mudou ainda
  2. copiar os assets referenciados para o Store (§5.3)
  3. capture() de todos os módulos envolvidos (no helper)
  4. GRAVAR snapshot + marcador "em andamento" no diário (durável, §7)
  5. para cada módulo, na ordem do grafo:
        apply(plan(atual, desejado))   (no helper, com timeout)
        se falhar (erro, timeout ou helper morto):
            restaurar TODOS os módulos tocados — inclusive o que falhou, que pode
            ter ficado pela metade — em ordem inversa
  6. gravar lastApplied por campo; remover marcador; registrar no histórico
  7. ativar/desativar a parte residente conforme needsResidency(config)
```

- **Queda no meio (crash, energia):** ao iniciar, o core encontra o marcador e **sempre desfaz** usando o snapshot (no MVP não existe "concluir"). Se o desfazer também falhar, grava o estado **"precisa restaurar"**, tenta de novo a cada inicialização e a UI mostra um aviso fixo até resolver.
- **Chamadas que tocam o Explorer** (capture e apply de wallpaper, tema, cursores, barra) rodam no `rmw-core --apply-helper` dentro de um Job, com **timeout de 15 s**; broadcasts usam `SendMessageTimeout(SMTO_ABORTIFHUNG)`. O spike S4 confirma se todos os módulos precisam do helper ou só o de tema.
- **Histórico:** só aplicações feitas pelo usuário (máx. 20). Reaplicações automáticas (agendamento, Explorer reiniciou) **não** entram.

### 5.1 Snapshot base ("meu Windows original")

- Capturado na **primeira execução**, com data visível na UI. Opção "recapturar original" com confirmação.
- Se a primeira execução detectar outras ferramentas de customização ativas (Wallpaper Engine, Lively, Auto Dark Mode, TranslucentTB, Windhawk, ExplorerPatcher, Rainmeter…), **avisa** que o "original" já está customizado e que elas podem disputar as mesmas configurações.
- **Recusa wallpaper animado** enquanto outro motor estiver ocupando a camada do desktop (janela não nossa sob `Progman`/`WorkerW`), explicando o motivo.
- "Padrão de fábrica do Windows" (valores conhecidos de uma instalação limpa) é um recurso separado, para a v1.x.

### 5.2 Drift — quando o usuário muda algo por fora

**Regra: depois de aplicar, o usuário manda.** Não existe reconciliação contínua ("brigar" com o Windows).

- O core guarda `lastApplied` **por campo** (ex.: wallpaper do monitor 2, modo escuro, cursor "texto").
- Quando um evento indica mudança externa (`WM_SETTINGCHANGE`, troca de wallpaper, etc.), o módulo compara o atual com `lastApplied`. Campo diferente → **desanexado**: o rice deixa de gerenciá-lo.
  - Exemplo: com vídeo no monitor 2, o usuário escolhe uma foto pelas Configurações do Windows (que ficaria escondida atrás do vídeo) → o renderizador do monitor 2 para, a foto aparece, e uma notificação diz "O monitor 2 saiu do rice. [Reaplicar] [Ok]".
- Explorer reiniciou / monitor reconectado → reaplica **só** campos ainda gerenciados.
- **Desfazer/restaurar fazem comparação de 3 vias** (snapshot / `lastApplied` / atual): campos que o usuário mudou depois são **pulados por padrão** e listados ("não restaurado porque você mudou depois: …"), com opção de forçar.
- Sobrescritores conhecidos, tratados explicitamente: Windows Spotlight, slideshow do Windows, sincronização de tema entre dispositivos, temas de contraste, Auto Dark Mode, outros motores de wallpaper.

### 5.3 Store de conteúdo

- Todo arquivo referenciado pelo rice ativo **ou** por um snapshot (imagens, cursores, vídeos, e a cópia do `TranscodedWallpaper` original do Windows no snapshot base) é copiado para `store\<sha256>.<ext>`, com contagem de referências no `state.db` e coleta de lixo quando ninguém mais referencia.
- O registro do Windows (ex.: caminhos de cursor) aponta **para o Store**, nunca para `rices\<id>\` → apagar um rice nunca quebra o cursor ou o wallpaper ativo, e o "original" sempre pode ser restaurado mesmo se o arquivo de origem sumiu.

## 6. Formato do rice (`rice.toml`)

Pacote = pasta ou arquivo `.rice` (zip) com `rice.toml` + `assets/`. Exemplo:

```toml
schema = 1

[meta]
name    = "Tokyo Night"
author  = "fulano"
version = "1.0.0"
license = "CC-BY-4.0"
min_app = "0.4.0"
# shareable = false                 # M12: o rice tem arquivos de terceiros (Wallpaper Engine): nunca é empacotado

[appearance]
mode              = "dark"          # light | dark | system | schedule
accent            = "#7aa2f7"       # "from-wallpaper" na Fase 2
transparency      = true
accent_on_taskbar = true

[appearance.schedule]               # usado quando mode = "schedule"
light_at = "07:00"
dark_at  = "19:00"                  # "sunset" na Fase 2

[wallpaper]
mode       = "per-monitor"          # per-monitor | span (uma imagem atravessando todos)
background = "#1a1b26"              # global no Windows

[wallpaper.policy]
pause_on_fullscreen = true
pause_on_battery    = true
pause_when_covered  = true
fps_limit           = 30
mute                = true

[[wallpaper.monitor]]
match          = "primary"          # primary | all | left | right | top | bottom | edid:<id>
kind           = "video"            # image | slideshow | video | color   (gif: v1.1, shader: Fase 2)
source         = "assets/city.mp4"
fallback_image = "assets/city.jpg"

[[wallpaper.monitor]]
match  = "all"                      # todos os que não casaram acima
kind   = "image"
source = "assets/mountains.jpg"
fit    = "fill"                     # fill | fit | stretch | center | tile
# M12 (imagem, vídeo e cor): desenhados pelo rmw-wallpaper por cima do wallpaper do Windows
effects   = { saturation = 1.1, vignette = 0.3, parallax = 0.4 }
            # brightness -1..1 | contrast 0..2 | saturation 0..2 | hue -180..180 | speed 0.25..4 (vídeo)
            # blur 0..40 | parallax 0..1 | zoom 0..1 (zoom e deslize lentos) | vignette 0..1
particles = [
    { kind = "sparkles", color = "palette.accent", count = 80, glow = true },
    { kind = "petals", size = 1.5, speed = 0.75, direction = 110 },
]           # sparkles | stars | snow | rain | petals | fireflies | magic | notes | hearts | bubbles
            # count 1..1000 | size 0.25..4 | speed 0..4 | direction em graus (90 = para baixo) | até 8 camadas

[cursors]
source = "assets/cursors"           # pasta com install.inf ou o mapeamento abaixo
size   = 32
[cursors.map]
normal = "pointer.cur"
busy   = "busy.ani"
text   = "beam.cur"

[taskbar]                           # barra NATIVA (quando não escondida)
alignment   = "left"                # left | center
autohide    = false
task_view   = false
search      = "icon"                # hidden | icon | box | button

[[bar]]                             # barra PRÓPRIA (pode haver várias)
monitor    = "all"                  # mesmos seletores do wallpaper
position   = "top"                  # top | bottom | left | right
mode       = "reserve"              # reserve (empurra as janelas) | float (flutuante)
thickness  = 36                     # px lógicos (escala com o DPI)
length     = "100%"                 # % ou px; centralizada quando < 100%
margin     = [8, 8, 0, 8]           # cima, direita, baixo, esquerda
radius     = 12
background = { kind = "acrylic", tint = "palette.base", opacity = 0.75 }  # solid | transparent | acrylic | mica
hide_native_taskbar = true
left   = ["start", "workspaces", "tasklist"]
center = ["clock"]
right  = ["io.github.fulano.now-playing", "tray", "volume", "battery"]

[plugins]                           # plugins que o rice usa (instalados à parte)
requires = ["io.github.fulano.now-playing@^1.2"]

[plugins."io.github.fulano.now-playing"]   # configuração do plugin (validada pelo esquema dele)
show_artist = true
max_width   = 260
```

Como as limitações do Windows são contornadas:
- **`fit` por monitor**: o Windows só tem um modo de ajuste global. O app **pré-renderiza** a imagem na resolução nativa de cada monitor (com o `fit` pedido) e define "preencher" globalmente. Fica idêntico e custa zero em tempo de execução.
- **`kind = "color"` por monitor**: pré-renderiza uma imagem sólida. `background` é a cor global do Windows.
- **Efeitos e partículas (M12)**: o Windows não sabe desenhá-los. Uma regra `image`, `video` ou `color` com `effects` ou `particles` é desenhada pelo `rmw-wallpaper` (Direct2D no mesmo dispositivo do vídeo) atrás dos ícones; o wallpaper do Windows continua por baixo, sem os efeitos, e aparece se o worker parar. Uma imagem parada custa 0% de CPU depois de desenhada; o que se mexe segue o `fps_limit` e pausa como um vídeo.
- **Slideshow**: se todos os monitores usam a mesma playlist e intervalo, usa o **slideshow nativo** do Windows (custo zero, não residente). Se diferem por monitor, o agendador do core troca as imagens (residente).
- **Identidade do monitor**: o caminho do dispositivo muda ao trocar de porta/dock. Identificamos por **EDID** (fabricante + produto + número de série), com fallback para o caminho do dispositivo. Seletores portáveis (`primary`, `left`…) servem para rices compartilhados; `edid:` serve para o rice pessoal. Monitor sem regra e sem `all` → mantém o wallpaper atual. (Validado no spike S5.)

Regras de segurança do formato: doc 06 §4. Seções desconhecidas geram aviso, nunca erro silencioso. Combinações impossíveis (ex.: `span` com `[[wallpaper.monitor]]`) são rejeitadas pelo validador.

## 7. Armazenamento

Tudo em `%LOCALAPPDATA%\RicingManagerWindows\` (nada no perfil móvel `%APPDATA%`: pacotes podem ter centenas de MB).

| O quê | Onde | Dono (único escritor) |
|---|---|---|
| Preferências, referência ao rice ativo | `config\*.toml` | core |
| Rices instalados | `rices\<id>\` (staging em `rices\.staging-<aleatório>\`, mesmo volume) | core |
| Store de conteúdo | `store\<sha256>.<ext>` | core |
| Diário, snapshots, histórico, `lastApplied`, refcounts | `state.db` (SQLite, `synchronous=FULL`) | core |
| Biblioteca de wallpapers (pastas monitoradas, tags, playlists) | `library.db` (SQLite) | UI (instância única) |
| Miniaturas | `cache\thumbs\` (JPEG/PNG, LRU com orçamento em MB) | UI |
| Logs | `logs\` (5 × 2 MB, rotação) | cada processo, arquivo próprio |
| Dumps | `crashes\` (máx. 5) | supervisor |

- **Escrita atômica** de arquivos: grava `*.tmp` → `FlushFileBuffers` → `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)`, com fallback `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`. (Sem garantia em FAT/exFAT — um motivo para adiar o modo portátil.)
- **`state.db` usa `synchronous=FULL`**: o padrão atual da classe `Database` (`NORMAL` em WAL) pode perder o último commit numa queda de energia — inaceitável para o diário.
- **Migrations** versionadas (`PRAGMA user_version`), cada uma dentro de transação.
- **Modo portátil**: adiado para a v1.x (conflita com entrada de inicialização, atalhos e cursores apontando para um pendrive).

## 8. IPC

**Core ↔ workers:** named pipe com **nome aleatório** (GUID), `PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE`, `nMaxInstances = 1`, DACL restrita. O core abre a ponta cliente e a passa **herdável** ao filho via `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (padrão libuv/Chromium). (Pipes anônimos de `CreatePipe` não suportam I/O assíncrono e são unidirecionais — não servem para um core orientado a eventos.)

**UI/CLI ↔ core:** named pipe `\\.\pipe\RicingManagerWindows-<SID>-<SessionId>`.
- DACL `D:P(A;;GA;;;<SID do usuário>)(A;;GA;;;SY)`; `PIPE_REJECT_REMOTE_CLIENTS`; `FILE_FLAG_FIRST_PIPE_INSTANCE`.
- O cliente **valida o servidor** (`GetNamedPipeServerProcessId` → caminho da imagem + sessão) e abre com `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`.
- O core registra o processo cliente (`GetNamedPipeClientProcessId`).
- Fronteira honesta: a DACL protege contra **outros usuários, processos Low IL e AppContainer**. Um processo do **mesmo usuário** com integridade média está dentro da fronteira de confiança (ele poderia, de qualquer forma, alterar os mesmos arquivos e registros) — documentado como risco aceito (doc 06, T3).

**Protocolo:**
- Mensagens com prefixo de tamanho (`uint32`) + JSON UTF-8, estilo **JSON-RPC 2.0**.
- **Handshake** `hello { protoMajor, protoMinor, buildId }`. Versão incompatível (ex.: core antigo ainda rodando após atualização) → o cliente pede `shutdown` e inicia o core novo.
- Limites: 1 MiB por mensagem, profundidade JSON ≤ 32, contagem de elementos limitada; mensagem inválida → desconecta.
- Métodos (exemplos): `rice.validate`, `rice.apply`, `rice.import`, `rice.export`, `snapshot.list`, `snapshot.restore`, `restore.original`, `monitors.list`, `status.get`, `events.subscribe`.
- Eventos para a UI: `rice.applied`, `field.detached`, `worker.crashed`, `monitors.changed`, `wallpaper.paused`.
- **API interna na v1.0**; vira pública e estável na Fase 2 (doc 05).

## 9. Eventos do sistema e threads do core

**Threads do core:** uma thread principal STA com loop de mensagens (janela oculta + I/O assíncrono dos pipes via `MsgWaitForMultipleObjectsEx`) + **uma** thread de trabalho para I/O lento. Tudo que toca o Explorer vai para o `--apply-helper` (§5). Menos threads = menos *race conditions*.

Uma **janela top-level oculta** (não pode ser *message-only*: essas não recebem broadcasts como `WM_DISPLAYCHANGE` e `TaskbarCreated`) + registros de notificação, todos convertidos em `SystemEvent`:

| Evento | Fonte | Cuidado |
|---|---|---|
| Explorer reiniciou | mensagem registrada `TaskbarCreated` | também chega quando o DPI do monitor principal muda → comparar o PID do dono de `Shell_TrayWnd` antes de reconstruir |
| Monitores/resolução/DPI | `WM_DISPLAYCHANGE`, `WM_SETTINGCHANGE` (`SPI_SETWORKAREA`), `WM_DPICHANGED`, `RegisterDeviceNotification(GUID_DEVINTERFACE_MONITOR)` | reenumerar **com atraso curto** (cedo demais devolve geometria errada) |
| Tema/cores mudaram por fora | `WM_SETTINGCHANGE` (`ImmersiveColorSet`), `WM_DWMCOLORIZATIONCOLORCHANGED` | alimenta a política de drift (§5.2) |
| Bloqueio / desbloqueio / RDP | `WTSRegisterSessionNotification`, `GetSystemMetrics(SM_REMOTESESSION)` | no logon pode falhar com `RPC_S_INVALID_BINDING` → tentar de novo após `Global\TermSrvReadyEvent` |
| Bateria / economia de energia / tela desligada / modo jogo | `RegisterPowerSettingNotification` com **os dois** GUIDs de economia: `GUID_POWER_SAVING_STATUS` (até 23H2) e `GUID_ENERGY_SAVER_STATUS` (24H2+, 3 níveis); `GUID_ACDC_POWER_SOURCE`; `GUID_SESSION_DISPLAY_STATUS`; `PowerRegisterForEffectivePowerModeNotifications` (versão V2, inclui *Game Mode*) | tudo por evento, sem *polling* |
| Suspensão / retomada | `PowerRegisterSuspendResumeNotification` | |
| Tela cheia / desktop coberto | `SHQueryUserNotificationState` + geometria das janelas por monitor | **não existe notificação** → *polling* (~500 ms) **só no `rmw-wallpaper`, só com wallpaper animado** |

## 10. Estrutura de pastas do repositório (alvo)

```
RicingManagerWindows/
├─ apps/
│  ├─ core/            rmw-core.exe  (modos: normal, --startup, --apply-helper, --import-worker, --safe-mode)
│  ├─ bar/             rmw-bar.exe
│  ├─ plugin-host/     rmw-plugin-host.exe (Luau)
│  ├─ settings/        rmw-settings.exe
│  ├─ wallpaper/       rmw-wallpaper.exe
│  └─ cli/             rmw.exe
├─ libs/
│  ├─ base/  domain/  platform/  modules/  engine/  ipc/  storage/  package/
│  ├─ plugin-api/      providers, ações, permissões, esquema de config (doc 12)
│  └─ widgets/         modelo de widgets e renderização (decidido no S7)
├─ plugins/            plugins oficiais de exemplo (Luau)
├─ tests/
│  ├─ unit/  contract/  integration/  system/ (Windows Sandbox)  fuzz/
├─ docs/               planejamento, ADRs, QA
├─ cmake/              Warnings.cmake, Hardening.cmake, triplets/
├─ CMakeLists.txt  CMakePresets.json  vcpkg.json
```

Rices de exemplo (com vídeos) **não** ficam neste repositório: vão como *assets* das releases ou num repositório separado (`rmw-rices`), para não inchar o Git.

Destino do código atual: `Database` → `libs/storage` (com `synchronous` configurável); `WinError` (corrigido) → `libs/base`. O resto vai para `legacy/` como referência e é **reescrito** com testes no M1 (cursores mudam para o esquema via registro, `MonitorID` é substituído por `DisplayTopology` via DisplayConfig/EDID, `WallpaperManager` é dividido entre `platform` e `modules`).

## 11. Pontos de extensão (escalabilidade)

**Tudo é extensível pela comunidade** — regra detalhada no [doc 12](12-plugins.md). Resumo:

| Nível | Quem estende | Como | Risco | Fase |
|---|---|---|---|---|
| Rice / asset-pack (P0) | qualquer usuário | `rice.toml` + assets | nenhum (só dados validados) | MVP |
| Tweak declarativo (P1) | qualquer usuário | `plugin.toml` com valores HKCU de lista permitida | baixo (lista permitida + desfazer) | MVP |
| Widget, skin, ação, regra (P2) | scripters | Luau no `rmw-plugin-host` com permissões | baixo (sandbox + Job) | MVP |
| Módulo nativo | contribuidores do projeto | `IModule` + teste de contrato | revisado no PR | MVP+ |
| Shader (P3) | artistas | HLSL | baixo (GPU + watchdog) | Fase 2 |
| Cliente IPC | qualquer dev | API JSON-RPC pública | limitado ao que a API permite | Fase 2 |
| Plugin nativo (P4) | devs externos | `rmw-pluginhost-native` isolado | médio, contido | Fase 3 |
