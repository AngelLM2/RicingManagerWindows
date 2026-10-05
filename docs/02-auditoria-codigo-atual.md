# 02 — Auditoria do código atual

> Estado analisado: commit `6b2cd41` + alteração local não commitada em `WallpaperPlaylist.cpp`.
> Toolchain usado na verificação: VS 2026 Build Tools (MSVC), CMake 4.2, vcpkg em `C:\dev\vcpkg`, Windows SDK 10.0.26100.

## Resumo

O projeto tem uma base com boas intenções (RAII, `std::format`, `std::span`, classe `Database` bem feita), mas **hoje não compila** e tem bugs que causariam crash ou comportamento errado assim que fosse usado de verdade. Nada disso é grave para um projeto em fase de treino — mas é exatamente o tipo de coisa que o plano precisa eliminar *por construção* (regras + ferramentas), não só corrigindo caso a caso.

| Severidade | Qtde | Significado |
|---|---|---|
| 🔴 Crítico | 7 | Impede build, causa crash, ou altera o sistema errado |
| 🟠 Alto | 8 | Comportamento incorreto, vazamento, bloqueia evolução |
| 🟡 Médio/Baixo | 10 | Qualidade, consistência, manutenção |

---

## 🔴 Crítico

### C1. O build está quebrado (verificado)
`src/managers/wallpaper/WallpaperPlaylist.cpp` — as 5 funções não têm `return`. O MSVC trata isso como erro (`C4716: must return a value`).

### C2. Depois de corrigir C1, o link falha (verificado numa cópia)
```
LNK2019: unresolved external symbol CursorHandler::release(void)
LNK2019: unresolved external symbol StringUtils::toLower(std::string)
```
- `StringUtils.hpp:8` declara `static std::string toLower(...)` como **membro** da classe, mas `StringUtils.cpp:7` define uma **função livre** `toLower` (falta `StringUtils::`).
- `CursorHandler.hpp:27,32` declara `get()` e `release()`, mas nunca são definidos; `CursorManager.cpp:32` usa `release()`.

### C3. Aplicar o cursor "Help", "Pin" ou "Person" substitui a **seta normal** do sistema
`CursorTypeConverter.cpp:13-65` — `OCR_HELP`, `OCR_PIN` e `OCR_PERSON` **não existem** no SDK (confirmei no `WinUser.h` do 10.0.26100; só existem `IDC_HELP`=32651, `IDC_PIN`=32671, `IDC_PERSON`=32672). Os `#ifdef` removem os `case`, o `switch` não casa e a função cai no `return OCR_NORMAL` do final.
Também: `NWPen → OCR_ICON` está errado (`OCR_ICON` é obsoleto e não representa a caneta).
No caminho novo (persistente, via registro) isso deixa de importar: o registro usa **nomes** de valor (`Arrow`, `Help`, `AppStarting`, `Wait`, `Crosshair`, `IBeam`, `NWPen`, `No`, `SizeNS`, `SizeWE`, `SizeNWSE`, `SizeNESW`, `SizeAll`, `UpArrow`, `Hand`, `Pin`, `Person`); constantes `OCR_*` só seriam necessárias para `SetSystemCursor`.

### C4. Varrer uma pasta pode derrubar o programa
`FolderScanner.cpp:50,62` — `directory_iterator`/`recursive_directory_iterator` **lançam** `std::filesystem::filesystem_error` se a pasta não existir, não tiver permissão, ou tiver junction/symlink quebrado. Os únicos `try/catch` do programa (`database.cpp:246` e `MonitorID.cpp:154`) não cobrem esse caminho → `std::terminate`. O mesmo vale para `std::filesystem::exists` em `WallpaperManager.cpp:39`, que lança em erros de acesso. Correção: usar as sobrecargas com `std::error_code` e `directory_options::skip_permission_denied`.

### C5. `FormatMessageW` sem `FORMAT_MESSAGE_IGNORE_INSERTS`
`WinError.cpp:12-20` — a documentação da Microsoft avisa (seção *Security Remarks*): sem essa flag e sem argumentos, mensagens do sistema que contêm `%1`, `%2`… fazem a função ler argumentos inexistentes → **access violation** em potencial. Também não remove o `\r\n` do final.

### C6. COM com ponteiros crus — vazamento garantido em caso de exceção
`WallpaperManager.cpp:46-88`, `119-146` e `MonitorID.cpp:12-47` — `CoCreateInstance` → … → `Release()` manual. Qualquer exceção no meio (`std::format` e `std::wstring` podem lançar `bad_alloc`/`format_error`) pula o `Release()`. Precisa de RAII (`wil::com_ptr` / `Microsoft::WRL::ComPtr`).

### C7. Logger pode "morrer" silenciosamente
`Logger.cpp:34` — `std::wcout` no locale padrão: ao escrever um caractere fora do Latin-1 (nome de arquivo em japonês, emoji…), o stream entra em estado de falha e **todos os logs seguintes somem**. Acentos saem corrompidos. Além disso, `std::cout` e `std::wcout` são misturados no mesmo console (`WallpaperManager.cpp:27`, `MonitorID.cpp:147`) — por isso o "conserto clássico" `_setmode(_O_U16TEXT)` **não serve aqui**: com ele, qualquer `std::cout` estreito derruba o CRT. Logs só no console = em produção (app sem console) não existe log nenhum. Correção real: logger novo gravando arquivo UTF-8; no console (só CLI), `WriteConsoleW`.

---

## 🟠 Alto

### A1. Nome amigável do monitor nunca é encontrado
`MonitorID.cpp:103-141` — compara o caminho do `IDesktopWallpaper` (formato `\\?\DISPLAY#DEL40F7#...#{e6f07b5f-...}`) com o nome do **adaptador** (`\\.\DISPLAY1`). Nunca casa → sempre `"Desconhecido"`. O jeito correto é `QueryDisplayConfig` + `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_TARGET_DEVICE_NAME)`, cujo `monitorDevicePath` é **a mesma string** (verificado nesta máquina; comparar sem diferenciar maiúsculas). O `monitorFriendlyDeviceName` pode vir vazio ou genérico, então é preciso fallback (EDID ou nome GDI `\\.\DISPLAYn`). Detalhe: `GetMonitorDevicePathAt` também lista monitores **desconectados** — filtrar com `GetMonitorRECT`.

### A2. `getWallpaper` ignora o monitor escolhido
`WallpaperManager.cpp:105-111` — o usuário escolhe um monitor (`serizalize_monitor`), mas o código usa `data.index` (sempre `0`).

### A3. `setWallpaper` retorna sucesso quando falha
`WallpaperManager.cpp:83-89` — se `SetWallpaper` falhar, loga o erro e retorna `1` (true). Também mistura `return 0`/`return 1` em função `bool`.

### A4. `GetLastError()` sem significado
`WallpaperManager.cpp:39-42` — `std::filesystem::exists` não define `GetLastError`. A mensagem de erro mostrada é aleatória.

### A5. Lógica de interface dentro dos managers
`WallpaperManager.cpp:27-30`, `MonitorID.cpp:143-165` — `std::cin`/`std::cout` dentro da camada de domínio. Isso impede GUI, testes automatizados, CLI e uso em segundo plano. Managers devem receber dados e devolver resultado, nunca perguntar ao usuário.

### A6. Cursor carregado duas vezes, um handle vaza
`CursorManager.cpp:19,28` — `LoadCursorFromFileW` é chamado duas vezes; o primeiro handle (`image`) nunca é destruído e o segundo não é verificado contra `nullptr`. E `release()` é chamado **antes** de `SetSystemCursor`: se a chamada falhar, ninguém destrói o cursor (só há transferência de posse quando ela tem sucesso). Além disso `SetSystemCursor` é **temporário** (o Windows restaura no próximo logon) — o modo persistente é registro (`HKCU\Control Panel\Cursors`) + `SystemParametersInfoW(SPI_SETCURSORS, …, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)`.

### A7. `CursorHandler` pode destruir o mesmo cursor duas vezes
`CursorHandler.hpp` — tem destrutor mas não deleta cópia nem implementa move (regra dos 5). Uma cópia acidental = `DestroyCursor` duplo.

### A8. Extensões ambíguas e "arquivo sem extensão vira cursor"
`FolderScanner.cpp:33` — `std::array<std::string_view, 17>` com só 2 itens → 15 `string_view` vazias. Como `path("arquivo").extension()` é `""`, **qualquer arquivo sem extensão é aceito como cursor**. Também `.webp` está em *Images* e *Animated*, e `.mp4/.webm` em *Animated* e *Videos*.

---

## 🟡 Médio / Baixo

| # | Onde | Problema |
|---|---|---|
| M1 | `FolderScanner.cpp:113` | `path::extension().string()` converte para a code page ANSI e **lança** se houver caractere não mapeável. Regra para o projeto: no Windows, usar sempre `wstring()`/`native()`. |
| M2 | `MonitorID.hpp:40` | `m_selectedIndex` não inicializado antes de `Initialize()`. |
| M3 | `database.cpp:1`, `CursorManager.cpp:5` | `#include "Database.hpp"` e `"logging/logger.hpp"` com caixa diferente do arquivo real. Funciona no NTFS, quebra em ferramentas/CI case-sensitive. |
| M4 | `database.*` | Falta wrapper RAII para `sqlite3_stmt` (prepared statements → evita SQL injection) e as migrations estão vazias. |
| M5 | `CMakeLists.txt` | Sem `/W4 /WX`, sem flags de hardening, sem `CMakePresets.json` (o toolchain do vcpkg precisa ser passado à mão), sem testes, `GLOB_RECURSE`, app de console. |
| M6 | `App.cpp:10` | `#pragma comment(lib, "ole32.lib")` redundante com o CMake; COM inicializado sem RAII e sem tratar `RPC_E_CHANGED_MODE`. |
| M7 | Vários | Arquivos vazios: `Config`, `ExplorerManager`, `PluginManager`, `RegistryManager`, `WallpaperTimer`, `WallpaperTransition`, `migrations.cpp`, `WallpaperController.cpp`. |
| M8 | `PluginManager` + `ScanType::Plugins` | Plano implícito de carregar `.dll` de terceiros **dentro do processo** — maior risco de segurança e estabilidade possível. Ver doc 06. |
| M9 | Estilo | `MonitorID` usa PascalCase, o resto camelCase; typo `serizalize_monitor`; `struct global` local; `README` cita pastas `GUI/` e `Settings/` que não existem. |
| M10 | `Logger` | Formata a string dentro do lock; sem arquivo, sem rotação, sem nível `debug`, sem timestamp. |

---

## O que está bom e será mantido

- **`Database`**: RAII correto, move semântico, `close()` `noexcept`, WAL, `foreign_keys`, `busy_timeout`, conversão UTF-8 correta do caminho. Vai ser mantido e completado (statements RAII + migrations). Um ajuste: `PRAGMA synchronous = NORMAL` (`database.cpp:414`) em WAL pode perder o último commit numa queda de energia — bom para cache, inaceitável para o diário de transações, que usará `FULL` (doc 03 §7).
- **Intenção de RAII** no `CursorHandler` (só falta completar).
- Uso de C++20 moderno (`std::format`, `std::span`, `std::string_view`, `std::filesystem`).
- Separação inicial por domínio (`wallpaper/`, `theme/cursor/`, `database/`) — a nova arquitetura aproveita essa ideia.
- Comentários explicando o *porquê* (ex.: `database.cpp`).

## Como o plano evita que esses problemas voltem

| Classe de problema | Mecanismo preventivo (ver doc 06 e 08) |
|---|---|
| Função sem retorno, warnings ignorados | `/W4 /WX /permissive-` + CI bloqueando merge |
| Símbolo declarado e não definido | Testes unitários que exercitam cada classe pública + CI linkando tudo |
| Vazamento de handle/COM | **WIL** (`wil::com_ptr`, `wil::unique_hkey`, `wil::unique_hcursor`…) obrigatório; `clang-tidy` proibindo `Release()`/`CloseHandle()` manuais |
| Exceção escapando | Fronteiras `noexcept` com `try/catch` nos limites de thread/callback/IPC; APIs de domínio retornam `std::expected` |
| Unicode | Regra: `std::wstring`/UTF-16 nas bordas Win32, UTF-8 em arquivos/IPC, proibido `path::string()` |
| UI no domínio | Arquitetura em camadas: domínio não conhece UI nem Win32 diretamente (doc 03) |
| Crash geral | Processos isolados + supervisor que reinicia workers (doc 03/06) |
