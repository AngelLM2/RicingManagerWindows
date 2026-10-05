# 06 — Segurança, memória e estabilidade

> Um customizador mexe em registro, no desktop, em arquivos baixados da internet e roda o tempo todo. Ele é um alvo natural (malware escondido dentro de um "tema") e um risco natural (um bug pode deixar o Windows feio, lento ou instável). Este documento define como evitamos as duas coisas. Fontes no doc 09 §7.

## 1. Modelo de ameaças

| # | Ameaça | Exemplo concreto | Controle principal |
|---|---|---|---|
| T1 | **Pacote de tema malicioso** | `.rice` com `../../…/Startup/x.exe` (zip slip), *stream* alternativo NTFS (técnica do CVE-2025-8088 do WinRAR, explorado ativamente), symlink (CVE-2025-11001 do 7-Zip), zip bomb, imagem 60000×60000, `.ani` malformado | Pacote = só dados; leitura restrita num processo isolado; limites; validação por conteúdo; fuzzing (§4) |
| T2 | **Código de terceiros** (plugins da comunidade) | script que lê arquivos do usuário, baixa malware, trava a barra; tweak que grava em `Run` para persistir | Luau só no `rmw-plugin-host` (sandbox + Job), permissões verificadas, rede só pelo host; tweaks só em lista permitida; barra e core nunca executam código de plugin (§6, doc 12) |
| T3 | **Outro processo abusando do app** | processo de outro usuário/Low IL/AppContainer conecta no nosso pipe; DLL plantada ao lado do `.exe` | DACL + validação do pipe; busca de DLL restrita (§5, §8). **Risco aceito:** um processo do *mesmo usuário* em integridade média está dentro da fronteira — ele já poderia alterar os mesmos arquivos e registros sem a nossa ajuda |
| T4 | **Escalada de privilégio** | malware do usuário usa um componente nosso elevado para virar admin | **Nada roda elevado por padrão.** Sem serviço, sem tarefa como SYSTEM. No Modo avançado: ajudante em `%ProgramFiles%`, assinado, um processo por operação com UAC, lista fechada de comandos, cópia de arquivos à prova de TOCTOU, RedirectionGuard (doc 10 §4) |
| T5 | **Cadeia de suprimentos** | dependência comprometida, release adulterada | baseline fixo, CodeQL, Scorecard, SBOM, atestação de build, assinatura (§9) |
| T6 | **Bug nosso danificando o sistema** | aplicar pela metade e cair; sobrescrever o cursor errado (bug C3) | Transação + diário durável + snapshot + Store + "restaurar original" (doc 03 §5) |
| T7 | **Falha de disponibilidade** | renderizador trava o desktop; vazamento come a RAM; Explorer trava uma chamada nossa | Processos isolados, Job Object, watchdog, chamadas ao Explorer num helper com timeout (§7) |
| T8 | **Reputação / antivírus** | o app é marcado como malware | Sem injeção, sem *packer*/ofuscação, sem tocar HKLM/IFEO/AppInit/Defender, binários assinados (§9) |

Fora de escopo: malware que já é administrador.

## 2. Princípios inegociáveis

1. **Sem admin por padrão.** Tudo em `HKCU` e pastas do usuário. Admin só no **Modo avançado** opt-in, por um ajudante mínimo instalado em `%ProgramFiles%`, com UAC a cada operação e lista fechada de comandos (doc 10 §4).
2. **Nenhum arquivo do sistema é modificado.** Nunca patchamos `uxtheme.dll`, `imageres.dll` etc. Temas `.msstyles` copiados para `%WinDir%\Resources\Themes` pelo Modo avançado não alteram arquivos existentes.
3. **Nosso código nunca injeta em processos de terceiros.** Além do risco técnico, há precedente: em out/2025 a Microsoft **revogou o certificado do Windhawk** por "realizar injeção de processo intencionalmente"; o Defender marcou o ExplorerPatcher em 2024. Ferramentas que injetam (TranslucentTB, Windhawk) só são **instaladas por clique explícito**, como pacotes separados via winget/Store (doc 10 §3).
4. **Tema é dado, não código.** Um `.rice` nunca executa nada.
5. **Toda mudança é reversível**, e a restauração funciona mesmo se a UI não abrir (CLI + atalho "Restaurar Windows original").
6. **Sem rede por padrão.** O MVP não faz nenhuma conexão.
7. **Sem telemetria.** Logs e dumps ficam locais (dumps contêm caminhos e nome de usuário → dados pessoais pela LGPD); enviar é sempre decisão explícita do usuário.

## 3. Segurança de memória em C++ (defesa em camadas)

| Camada | O que pega | Onde |
|---|---|---|
| 1. **Regras de código** (RAII/WIL, sem `new`/`delete`, sem dono cru, `std::span`, conversões checadas) | vazamentos, *double free*, *use-after-free* comuns | doc 08 §3 |
| 2. **Compilador**: `/W4 /WX /sdl` + **STL hardened** (`_MSVC_STL_HARDENING=1`: UB da STL vira `__fastfail` em vez de corromper memória) | retorno faltando, não inicializado, índice fora do limite | CMake |
| 3. **Análise estática**: `clang-tidy`, `/analyze`, CodeQL | padrões perigosos, *null deref*, *lifetime* | CI |
| 4. **AddressSanitizer** em todos os testes | *heap overflow*, *use-after-free*, *double free* | CI |
| 5. **Fuzzing dos nossos parsers** (§4) | entradas malformadas | CI + agendado |
| 6. **Mitigações do SO**: CFG, EH continuation, CET *shadow stack*, ASLR, DEP, `/Qspectre` | dificultam transformar um bug restante em exploit | compilador/linker |
| 7. **Isolamento de processo** + Job Object | o que sobrou derruba só um worker, que é reiniciado | arquitetura |

Notas de compatibilidade (detalhes no doc 08):
- `/DYNAMICBASE`, `/HIGHENTROPYVA`, `/NXCOMPAT` e `/GS` já são padrão no x64; `/permissive-` já vem com C++20+; terminação por corrupção de heap já é padrão em 64-bit.
- CET em modo **compatível** (não estrito): drivers de GPU antigos e *overlays* (Discord, RTSS, Steam) não são CET.
- ASan não instrumenta corrotinas C++20 (não usamos), nunca devolve memória ao SO e é incompatível com ACG → em builds ASan, orçamentos de memória, limites de Job e ACG ficam desligados.

### Memória sob controle (não só "não vazar", mas "não crescer")

- **Nenhum cache ou fila sem limite.** Miniaturas: LRU com orçamento em MB. Fila de eventos: tamanho máximo. Histórico: 20 itens.
- **Teto por processo via Job Object** — atenção à semântica: o teto rígido (`JOB_OBJECT_LIMIT_PROCESS_MEMORY`) **só faz alocações falharem**, não encerra o processo. Por isso o supervisor usa também um **limite de notificação** (~80% do teto, entrega garantida) e encerra/reinicia o worker ao recebê-lo (doc 03 §2.4).
- **Decodificar no tamanho da tela**, nunca maior (imagens via `IWICBitmapScaler`).
- **Pausado = para de apresentar na hora**; após 2 min pausado, libera os buffers do decodificador e mantém só o último quadro.
- **Medição contínua** de *Private Bytes* dos workers (tela de Diagnóstico + teste de orçamento no CI).

## 4. Entrada não confiável

### Onde é processada

- **Importação de `.rice` e `.rmwp` (plugins)**: no `rmw-core --import-worker`, dentro de um Job com teto de memória. O core só recebe o resultado validado e faz o *rename* final.
- **Miniaturas**: no `rmw-settings` (processo descartável).
- **Vídeo/imagem do wallpaper**: no `rmw-wallpaper`.
- O core nunca decodifica imagem, vídeo nem zip.

### Limites (centralizados em `libs/base/Limits.hpp`; valores iniciais)

| Entrada | Limite |
|---|---|
| Entradas no pacote | 1 000 |
| Total descompactado | 1 GB, contando bytes **reais** (nunca o tamanho declarado no cabeçalho) |
| Por arquivo | vídeo 512 MB · imagem 64 MB · cursor 1 MB · `install.inf` 64 KB · `rice.toml` 64 KB |
| Razão de compressão | ≤ 100:1 |
| Métodos de compressão | só *stored* (0) e *deflate* (8); entradas criptografadas e zips aninhados rejeitados |
| Imagem estática | ≤ 16 384 px por lado e ≤ **64 MP** (checado no cabeçalho, **antes** de decodificar) |
| Imagem animada (v1.1) | ≤ 4 096 px por lado, nº máximo de quadros e orçamento total de pixels |
| Nome dentro do zip | componente ≤ 255, caminho relativo ≤ 160 caracteres |
| TOML | profundidade ≤ 32 |
| Mensagem IPC | 1 MiB, profundidade JSON ≤ 32, contagem de elementos limitada |

### Regras para nomes de entrada do zip (todas rejeitam)

- Caminho absoluto, letra de unidade, UNC, prefixos `\\?\`/`\\.\`.
- `\` tratado como separador; componentes vazios, `.` ou `..`.
- Caracteres `< > : " | ? *` (o `:` cobre *streams* alternativos NTFS) e caracteres de controle.
- Componente terminando em ponto ou espaço (em **qualquer** componente).
- Nomes reservados `CON PRN AUX NUL COM0-9 LPT0-9` e as variantes `COM¹²³`/`LPT¹²³`, com ou sem extensão.
- Componentes no formato de nome curto 8.3 (`~` seguido de dígito).
- Entradas de symlink; nomes duplicados (comparação sem caixa, ordinal).
- Campo extra "Unicode Path" (0x7075) diferente do nome do diretório central — vale só o diretório central.
- Extensão fora da lista permitida para aquele tipo de asset.

### Leitura e gravação

- Biblioteca **libzip** (auditada, no OSS-Fuzz), aberta com `ZIP_RDONLY | ZIP_CHECKCONS`, com as *features* opcionais do vcpkg desligadas (bzip2/lzma/zstd/crypto). **Nunca** funções "extrair tudo" de biblioteca (a minizip-ng, por exemplo, cria symlinks).
- Arquivos pequenos (TOML, INF, imagens, cursores) são lidos **para a memória**; só vídeos são gravados em disco.
- Gravação em `rices\.staging-<aleatório>\` (mesmo volume do destino, para o *rename* final ser atômico), com caminhos `\\?\`, `CREATE_NEW` + `FILE_FLAG_OPEN_REPARSE_POINT`; depois, `GetFinalPathNameByHandle` do arquivo comparado (ordinal, sem caixa) com o da pasta de staging.
- Validação **por conteúdo** (assinatura do arquivo, não extensão). Decodificador de imagem **instanciado por CLSID** conforme a assinatura detectada (`CLSID_WICPngDecoder2`, `CLSID_WICJpegDecoder`, `CLSID_WICBmpDecoder`, `CLSID_WICGifDecoder`) — `WICComponentEnumerateBuiltInOnly` não serve para isso (só vale para enumeração). WebP/HEIF/AVIF só por decodificador assinado pela Microsoft/Store (ou libwebp no futuro). O WIC teve CVEs de execução remota em 2025–2026.
- **Cursores (`.cur`/`.ani`) merecem atenção especial**: depois de aplicados, **todo processo do sistema** os lê. Validamos a estrutura RIFF/ICO com **parser próprio** com limites (alvo de fuzzing) e rejeitamos na dúvida.
- No fim, o core move a pasta validada para `rices\<id>\`. Qualquer falha → apagar o staging.

### Alvos de fuzzing (código nosso — não adianta "fuzzar o WIC")

Validação de nomes de entrada do zip · leitor de pacote · `rice.toml` e `plugin.toml` (nosso carregador sobre o toml++) · validador de tweaks (lista permitida/bloqueada) · `install.inf` · parser RIFF/ICO de cursores · pré-checagem de cabeçalho de imagem · framing e JSON do IPC · árvore de widgets recebida do host de plugins.

## 5. Endurecimento de processo

- **CRT estático (`/MT`) por escolha**: menos DLLs para distribuir e plantar, instalação mais simples. Exceção provável: o `slint_cpp.dll` (feito em Rust) importa o `vcruntime140.dll`, que então vai **app-local** ao lado do `rmw-settings.exe` (distribuição app-local é suportada oficialmente; verificado no spike S6). Regra: **nunca** passar memória alocada ou objetos `std::` entre módulos com CRTs diferentes.
- **DLLs**: `/DEPENDENTLOADFLAG:0x800` (só System32) em todos os executáveis com `/MT`; `0xA00` (System32 + pasta do app) **só** no `rmw-settings`. Atenção: a pasta do app *é* a pasta Downloads quando o instalador roda de lá — o instalador recebe o mesmo cuidado. A flag vale só para importações estáticas do próprio módulo; todo `LoadLibraryExW` do nosso código passa `LOAD_LIBRARY_SEARCH_SYSTEM32` explicitamente, e `SetDefaultDllDirectories` é chamado no início de cada processo.
- Instalação em `%LOCALAPPDATA%\Programs\RicingManagerWindows\` (só o usuário escreve).

| Processo | Mitigações | Quando |
|---|---|---|
| `rmw-core` e modos helper/worker | CFG, CET, *strict handle checks* | MVP |
| `rmw-wallpaper` | CFG, CET, *strict handle checks* | MVP |
| `rmw-core` (endurecido) | sem código dinâmico (ACG), *extension points* desabilitados, sem imagens remotas/baixa integridade — se compatíveis com o COM do Shell | conforme spike S6 |
| `rmw-wallpaper` (endurecido) | política de assinatura `MitigationOptIn` (aceita Microsoft/Store/WHQL — `MicrosoftSignedOnly` quebraria drivers de GPU) e ACG **aplicadas só depois** de inicializar D3D e Media Foundation, com chave para desligar | Fase 2 |
| decodificador isolado (Low IL / AppContainer) | §5.1 | Fase 3+ |
| `rmw-settings` | conforme o framework de UI permitir | MVP |

### 5.1 Sandbox do decodificador (evolução)

Um processo de **baixa integridade não consegue se encaixar** na janela do Explorer (UIPI bloqueia), e um *target* DirectComposition precisa pertencer ao processo dono da janela. O padrão (Chromium/Firefox) é o processo privilegiado — o "broker" — ser dono da janela. Plano:
- **MVP:** `rmw-wallpaper` em integridade média (dono da janela + decodificação), com mitigações, Job e watchdog.
- **Depois:** dividir em **host** (integridade média, dono da janela, só apresenta) + **decodificador** (Low IL / AppContainer, Job restrito) que entrega quadros por **textura D3D11 compartilhada** (handle NT + keyed mutex) ou `DCompositionCreateSurfaceHandle`.

## 6. Código de terceiros (plugins — doc 12)

- **Tweaks declarativos (P1, MVP):** só chaves HKCU de uma **lista permitida** (ex.: `Explorer\Advanced`, `Themes\Personalize`, `Control Panel\Desktop`, `AppEvents\Schemes`), com tipo e faixa de valor validados; **lista de bloqueio fixa** que vence sempre (`Run`, `RunOnce`, `Software\Classes\CLSID` e afins — sequestro de COM —, `Policies`, `Environment`, `Winlogon`, `Image File Execution Options`…); valores que pareçam caminho para `.exe`/`.dll`/script são rejeitados; a tela de instalação lista exatamente as chaves tocadas; snapshot e desfazer como qualquer módulo.
- **Scripts (P2, MVP) → Luau** (MIT, no vcpkg) rodando no **`rmw-plugin-host`** com Job Object:
  - `luaL_sandbox` + `luaL_sandboxthread` por script; bibliotecas somente-leitura; sem `io`, `package`, `string.dump`; **`loadstring` removido**; só código-fonte (nunca bytecode de terceiros); JIT desligado.
  - **Três travas**, porque nenhuma basta sozinha: *interrupt callback* (só dispara em chamadas e voltas de loop — uma função C longa como `string.rep` ou `table.sort` escapa dele), **alocador com limite de memória**, e **watchdog de tempo** que mata o processo host.
  - Permissões do manifesto verificadas a cada chamada de *provider*/ação; rede só pelo host, para domínios declarados, com limite de tamanho e frequência; dados sensíveis (títulos de janelas, mídia) só com permissão destacada.
  - A barra e o core **nunca** executam código de plugin: recebem só valores/árvores de widgets por IPC, validados (tamanho, profundidade, tipos).
- **Plugins nativos** (Fase 3): nunca dentro do core; `rmw-pluginhost` com token Low IL, Job Object, manifesto de permissões aprovado pelo usuário, IPC por handle herdado. Aviso claro: código nativo é confiança total dentro do processo dele.

## 7. Estabilidade e recuperação de crash

| Mecanismo | Como funciona |
|---|---|
| Supervisor | Job + limite de notificação + heartbeat por thread de janela + `IsHungAppWindow` + backoff; 3 falhas/5 min → fallback + notificação (doc 03 §2.4) |
| Helper para o Explorer | capture/apply que tocam o Explorer rodam no `--apply-helper` com timeout de 15 s; se travar, é encerrado e a transação é desfeita (doc 03 §5) |
| **Modo seguro** | `Shift` ao iniciar, `rmw-core --safe-mode`, ou automático após 3 crashes seguidos na inicialização: não aplica rice, não inicia workers, abre a UI com "Restaurar Windows original" em destaque |
| Diário durável | `state.db` com `synchronous=FULL`; queda no meio do `apply` → desfazer na próxima inicialização; falha ao desfazer → estado "precisa restaurar" com nova tentativa a cada início |
| **Dumps — v1.0** | gravar dump de dentro do processo que está quebrando é arriscado (heap corrompido, *loader lock*). Nos workers: o filtro de exceção apenas **sinaliza** o supervisor e espera; o **supervisor grava o minidump** de fora. Crashes de *fast-fail* (CFG, CET, `/GS`, STL hardened) não passam pelo filtro → registramos o código de saída. No core: marcador de "saída não limpa" + código de saída no log. Máx. 5 dumps, nunca enviados sozinhos. |
| **Dumps — v1.x** | DLL auxiliar registrada com `WerRegisterRuntimeExceptionModule` em HKCU (valor `REG_DWORD` com o caminho completo da DLL; registrar em runtime e remover ao sair) para capturar também *fast-fails* (Win10 20H1+, mesma abordagem do Chrome/Crashpad). Cuidados: a DLL roda dentro do `WerFault.exe` (mínima, sem dependências) e a chave é um local de persistência conhecido, que EDRs podem sinalizar — por isso fica fora da v1.0. |
| Falha "degradada" | API indisponível nesta versão do Windows → feature some da UI com explicação; **nunca** crash |
| GPU removida / TDR | `DXGI_ERROR_DEVICE_REMOVED/RESET` → recriar dispositivo, swapchain, gerenciador DXGI e media engine |
| Explorer reiniciou | módulos pedem reaplicação dos campos ainda gerenciados |

## 8. IPC seguro

- **Core ↔ workers:** named pipe de **nome aleatório**, overlapped, duplex, `FILE_FLAG_FIRST_PIPE_INSTANCE`, `nMaxInstances = 1`, DACL restrita; ponta cliente passada herdável via `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.
- **UI/CLI ↔ core:** nome com SID **e** ID de sessão (`\\.\pipe\` é global entre sessões); DACL `D:P(A;;GA;;;<SID>)(A;;GA;;;SY)`; `PIPE_REJECT_REMOTE_CLIENTS`; o core cria o pipe primeiro com `FILE_FLAG_FIRST_PIPE_INSTANCE`; o cliente valida o servidor (`GetNamedPipeServerProcessId` + caminho da imagem + sessão) e abre com `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`.
- Toda mensagem é não confiável: tamanho e profundidade limitados, esquema validado, erro → desconecta.

## 9. Privacidade, cadeia de suprimentos e releases

- Logs registram caminhos como `%USERPROFILE%\…`. "Gerar pacote de suporte" mostra o conteúdo antes de salvar.
- vcpkg: `builtin-baseline` e versão da ferramenta fixados; cache binário via NuGet no GitHub Packages ou `files` + `actions/cache`.
- SBOM SPDX (o vcpkg gera por port) + `actions/attest-build-provenance` (SLSA Build L2, gratuito em repositório público).
- CodeQL (build manual com CMake) + OpenSSF Scorecard.
- Builds reproduzíveis (`/Brepro`, toolset fixo) como meta da Fase 4.
- Release **somente pelo CI**, a partir de tag, com checksums SHA-256.

### Assinatura de código (decisão D5 — ver README)

| Opção | Custo | Requisitos / observações |
|---|---|---|
| **Certum Open Source** | €25–69/ano | certificado em nome do desenvolvedor ("Open Source Developer"); identidade **constante** desde a primeira versão pública → a reputação no SmartScreen se acumula entre versões |
| **SignPath Foundation** | grátis | licença OSI **sem componentes proprietários** e sem dual-licensing comercial; projeto **já lançado** e com reputação; build automatizado no CI; MFA; aprovação manual por release; o "editor" exibido é "SignPath Foundation" (trocar de Certum para SignPath depois **zera** a reputação) |
| **Microsoft Store** (MSIX) | grátis para pessoa física desde set/2025 | a Microsoft assina (sem aviso do SmartScreen); MSIX virtualiza escritas em HKCU — é preciso declarar `unvirtualizedResources` para os ajustes funcionarem. O Lively é distribuído assim. Fase 4. |
| Sem assinatura | grátis | SHA-256 + atestação; o **Smart App Control** do Win11 bloqueia; SmartScreen avisa |
| Azure Artifact Signing | — | **indisponível** para pessoa física no Brasil (só EUA/Canadá) |

Certificado EV não pula mais o SmartScreen (desde 2024). winget não exige assinatura.
