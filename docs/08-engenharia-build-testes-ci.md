# 08 — Engenharia: build, padrões de código, testes e CI

Este documento define **como** o código é escrito e verificado. A maior parte da segurança de memória vem daqui: regras simples que ferramentas conferem automaticamente, para que erros como os da auditoria (doc 02) não cheguem ao `main`.

## 1. Toolchain

| Item | Escolha | Observação |
|---|---|---|
| Compilador | MSVC (VS 2026 Build Tools — já instalado) | clang-cl opcional para `clang-tidy` e fuzzing |
| Padrão C++ | **C++23** no modo suportado pelo MSVC | Usamos `std::expected`, `std::span`, `std::format`, *concepts*. **Sem** C++ modules (tooling ainda instável). |
| Build | CMake ≥ 3.28 + **Ninja** (vem com o VS) | `CMakePresets.json` com presets prontos — ninguém precisa passar `-DCMAKE_TOOLCHAIN_FILE` à mão |
| Dependências | vcpkg em *manifest mode*, baseline fixado | Toda dependência nova precisa de justificativa no PR (tamanho, licença, histórico de CVEs) |
| Formatação | `clang-format` (arquivo no repo) | Checado no CI |
| Arquitetura | x64 (ARM64 na Fase 4) | |
| Linkagem | **estática** (CRT `/MT` + *overlay triplet* `x64-windows-static` endurecido) | por escolha: menos DLLs para distribuir/plantar; exceção provável é o `rmw-settings` (Slint → `vcruntime140.dll` app-local); ver doc 06 §5 |

**Pré-requisitos desta máquina** (VS Installer → Build Tools 2026 → Modificar): *MSVC Spectre-mitigated libs* (necessário para `/Qspectre`) e *C++ Clang tools for Windows* (`clang-format`, `clang-tidy`, `clang-cl`). **Instalados e verificados em 2026-10-05**: Spectre libs em `VC/Tools/MSVC/14.51.36231/lib/spectre/x64`; Clang, clang-tidy e clang-format 20.1.8. Ninja e CMake 4.2 já vêm com o Build Tools.

### Presets planejados

| Preset | Uso |
|---|---|
| `debug` | desenvolvimento diário (`_ITERATOR_DEBUG_LEVEL=2`, sem otimização) |
| `release` | build de distribuição (LTO, PDB separado, hardening completo) |
| `asan` | testes com AddressSanitizer do MSVC (`/fsanitize=address`), baseado em RelWithDebInfo com `CMAKE_MSVC_RUNTIME_CHECKS` vazio (o `/RTC1` padrão do Debug é incompatível) |
| `fuzz` | alvos de fuzzing (`/fsanitize=fuzzer` ou clang-cl + libFuzzer) |
| `tidy` | gera `compile_commands.json` para `clang-tidy` |

## 2. Dependências planejadas

| Biblioteca | Para quê | Licença | Quando |
|---|---|---|---|
| **WIL** (Microsoft) | RAII para HANDLE, HKEY, COM, HCURSOR…; macros de erro | MIT | MVP |
| **toml++** | `rice.toml` e configs legíveis | MIT | MVP |
| **SQLite** (já usado) | índice da biblioteca de wallpapers, snapshots, histórico | Domínio público | MVP |
| **GoogleTest** | testes | BSD-3 | MVP |
| **Slint** (SDK C++ pré-compilado, versão fixada, renderizador por CPU) | UI de configurações e, se o S7 escolher, widgets da barra (doc 07) | GPLv3 | MVP (S2/S7 → M3/M7) |
| **libzip** (só leitura, *features* opcionais desligadas) | importar pacotes `.rice` e `.rmwp` (auditada, OSS-Fuzz; doc 06 §4) | BSD-3 | MVP (M8) |
| **nlohmann-json** | mensagens IPC (JSON-RPC); no OSS-Fuzz | MIT | MVP (M1 — o IPC existe desde o primeiro marco) |
| **Luau** | plugins da comunidade em sandbox no `rmw-plugin-host` (doc 12, doc 06 §6) | MIT | MVP (M4) |

APIs do próprio Windows (sem dependência extra): Direct3D 11, DXGI, Direct2D, DirectWrite, WIC, Media Foundation, DWM, Shell (`IDesktopWallpaper`), DisplayConfig, Registry.

## 3. Regras de código ("carta de segurança de memória")

São obrigatórias. O CI e o code review verificam.

1. **Sem `new`/`delete`/`malloc` manuais.** Use `std::make_unique`, containers e `std::vector`.
2. **Todo recurso do Windows dentro de RAII.** `wil::unique_handle`, `wil::unique_hkey`, `wil::com_ptr`, `wil::unique_hcursor`, `wil::unique_cotaskmem_string`… Proibido chamar `CloseHandle`, `RegCloseKey`, `Release()`, `CoTaskMemFree`, `LocalFree` diretamente fora de wrappers.
3. **Ponteiro cru nunca é dono.** `T*`/`T&` = emprestado. Dono = `unique_ptr`, container ou wrapper RAII.
4. **Classes com recurso seguem a regra dos 5** (ou deletam cópia explicitamente).
5. **Erros:** APIs de domínio retornam `std::expected<T, Error>`. Exceções podem existir internamente, mas **nunca atravessam fronteiras**: entrada de thread, `WndProc`, callbacks COM/Win32, handlers de IPC, destrutores. Toda fronteira é `noexcept` com `try/catch` que loga e converte em erro.
6. **Strings:** UTF-16 (`std::wstring`) na fronteira Win32; **UTF-8** em arquivos, TOML, IPC e logs. Conversão só via `base::utf8To16/utf16To8`. Proibido `std::filesystem::path::string()`, `std::wcout`, funções `*A` do Win32.
7. **Conversões numéricas checadas** (`base::checkedCast`) entre `size_t`, `UINT`, `DWORD`, `int`.
8. **Toda entrada externa tem limite**: tamanho de arquivo, dimensão de imagem, profundidade de TOML, tamanho de mensagem IPC, número de itens. Limites ficam centralizados em `base/Limits.hpp`.
9. **Threads:** só `std::jthread` com `std::stop_token`; nada de `detach()`. Estado compartilhado mínimo — preferir troca de mensagens com *owner* único. Toda thread COM declara apartamento (STA com loop de mensagens, ou MTA). Chamadas que podem travar indefinidamente (COM do Explorer, broadcasts) **não** rodam em thread nossa: vão para o processo `--apply-helper`, que pode ser encerrado (doc 03 §5); broadcasts sempre com `SendMessageTimeout(SMTO_ABORTIFHUNG)`.
10. **Sem estado global mutável** (exceto o logger). Dependências entram pelo construtor → testável com *fakes*.
11. **Domínio não inclui `<windows.h>`.** Só `libs/platform` fala com o Win32 (doc 03).
12. **Nada de I/O de console nas bibliotecas.** Interação com usuário só nos executáveis (UI/CLI).

### Flags de compilação (Release)

- Compilador (nossos alvos): `/W4 /WX /utf-8 /Zc:__cplusplus /sdl /guard:cf /guard:ehcont /Qspectre /external:anglebrackets /external:W0` + `_MSVC_STL_HARDENING=1`.
- Linker: `/guard:cf /guard:ehcont /CETCOMPAT /DEPENDENTLOADFLAG:0x800` (só System32; `0xA00` apenas no `rmw-settings`). `/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GS` já são padrão no x64.
- Dependências: as mesmas flags de mitigação via *overlay triplet* do vcpkg (incluindo `/guard:ehcont` e `/Gy`, senão o link falha com LNK2047), **sem** `/sdl`/`/WX`.
- `/Qspectre` com Ninja: o ambiente precisa ser aberto com `vcvarsall.bat x64 -vcvars_spectre_libs=spectre` (só o MSBuild troca as libs sozinho); o preset cuida disso.
- Debug/teste: `/RTC1` no `debug`; no `asan`, sem `/RTC` e sem link incremental (incompatíveis com ASan).
- Detalhes e compatibilidade com D3D/Media Foundation: doc 06 §3 e §5.

### Convenções de nome (mantendo o estilo atual do projeto)

| Elemento | Estilo | Exemplo |
|---|---|---|
| Namespace | minúsculo | `rmw::wallpaper` |
| Classe/struct/enum | PascalCase | `WallpaperService` |
| Método/função | camelCase | `applyRice()` |
| Membro | `m_` + camelCase | `m_monitors` |
| Constante | `k` + PascalCase | `kMaxThemeSizeBytes` |
| Arquivo | igual à classe | `WallpaperService.hpp/.cpp` |

## 4. Estratégia de testes

| Camada | O que testa | Como | Onde roda |
|---|---|---|---|
| **Unitário (domínio)** | modelo do rice, validação, diff/plano de aplicação, snapshot, parsers | GoogleTest, sem Windows (fakes) | CI, a cada commit |
| **Contrato de módulo** | todo módulo: `capture → apply → restaurar` deixa o sistema **equivalente** (`IModule::equivalent`, que ignora valores que o Windows deriva sozinho) | `FakeSystem` em memória + teste parametrizado para todos os módulos | CI |
| **Integração (plataforma)** | adaptadores Win32 que só mexem em registro | Registro em **sandbox**: os adaptadores recebem a chave raiz; testes usam `HKCU\Software\RicingManagerWindows\TestSandbox\<guid>` e nunca tocam as configurações reais | CI (runner Windows) |
| **Sistema** | o que não dá para redirecionar (`SPI_SETCURSORS`, `IDesktopWallpaper`, `IThemeManager2`, `SHAppBarMessage`, encaixe do vídeo) | **Windows Sandbox** (`.wsb`, disponível no Win11 Pro) ou VM Hyper-V com checkpoints: Windows descartável onde o agente pode aplicar e restaurar à vontade **sem tocar no seu Windows** | local (e CI, se o S0 mostrar que dá) |
| **ASan** | uso de memória inválida | preset `asan` rodando todos os testes | CI |
| **Fuzzing** | **nossos** parsers: nomes de entrada do zip, leitor de pacote, carregador do `rice.toml`, `install.inf`, RIFF/ICO de cursores, pré-checagem de cabeçalho de imagem, framing/JSON do IPC | libFuzzer; 60 s por alvo no CI, horas localmente/agendado | CI (smoke) + agendado |
| **Desempenho** | memória/CPU do núcleo; renderizador; UI | script mede *Private Bytes* e CPU por N segundos; falha se passar do orçamento (doc 01) | CI (núcleo/UI) / máquina de referência (vídeo) |
| **Manual (checklist de release)** | multi-monitor, DPI misto, HDR, reinício do Explorer, suspensão, bloqueio | roteiro em `docs/qa/` | antes de cada release |

Regra: **bug corrigido = teste que reproduz o bug**.

## 5. CI (GitHub Actions)

O repositório `AngelLM2/RicingManagerWindows` é **público** (verificado em 2026-10-05), então CodeQL, Scorecard, atestação e proteção de branch são gratuitos. O que os runners conseguem testar de verdade (Explorer, DWM, GPU) é medido no spike S0.

Jobs em todo PR (runner `windows-latest`, que desde jun/2026 traz o VS 2026 — o mesmo compilador desta máquina):

1. `format` — clang-format em modo verificação.
2. `build` — presets `debug` e `release`, com cache binário do vcpkg (NuGet no GitHub Packages ou `files` + `actions/cache`; o antigo `x-gha` foi removido).
3. `test` — GoogleTest (unit + contrato + integração).
4. `asan` — testes com AddressSanitizer.
5. `analyze` — `clang-tidy` (regras `bugprone-*`, `cppcoreguidelines-*` selecionadas, `modernize-*`, `performance-*`) e/ou MSVC `/analyze`.
6. `codeql` — análise de segurança (gratuita para repositórios públicos; build manual com CMake).
7. `fuzz-smoke` — 60 s em cada alvo de fuzzing.
8. `budget` — orçamento de memória do núcleo (e da UI, quando existir).
9. `scorecard` — OpenSSF Scorecard (semanal).

Release (em tag `vX.Y.Z`): build `release`, assinatura de código, instalador, SBOM SPDX, `actions/attest-build-provenance`, checksums SHA-256, notas a partir do CHANGELOG.

## 6. Fluxo de trabalho Git

- `main` protegido: só entra via PR com CI verde.
- Branches `feat/…`, `fix/…`, `docs/…`, `refactor/…`.
- **Conventional Commits** (`feat(wallpaper): …`, `fix(cursor): …`) → CHANGELOG automático.
- Versionamento semântico; `0.x` até o MVP estável.
- Template de PR com checklist: testes, limites de entrada, RAII, sem `<windows.h>` no domínio, docs atualizadas.

## 7. Definition of Done (vale para toda tarefa)

- [ ] Compila sem warnings nos presets `debug` e `release`
- [ ] Testes novos cobrindo o comportamento (e o bug, se for correção)
- [ ] `asan` verde
- [ ] Se altera o sistema: implementa o contrato `IModule` (capture/plan/apply/toConfig/equivalent), passa no teste de contrato e no teste de sistema (Windows Sandbox)
- [ ] Se lê entrada externa: tem limites e alvo de fuzzing
- [ ] Logs úteis (sem dados pessoais)
- [ ] Docs/ADR atualizados se mudou arquitetura

## 8. Arquivos de comunidade (Fase 0)

`LICENSE`, `README.md` (en + pt-BR), `CONTRIBUTING.md`, `SECURITY.md` (como reportar vulnerabilidade), `CODE_OF_CONDUCT.md`, templates de issue/PR, `docs/adr/`.
