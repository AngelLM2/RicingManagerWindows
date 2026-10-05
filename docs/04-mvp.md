# 04 — MVP: escopo, marcos e critérios de aceite

## Definição do MVP (v1.0)

> Um usuário instala sem admin, abre uma interface bonita e escolhe um rice (ou monta o seu) com **wallpaper estático ou em vídeo por monitor, modo claro/escuro (inclusive por horário), cor de destaque, cursores** e uma **barra de tarefas própria totalmente dimensionável** (tamanho, posição, cantos, transparência, módulos, bandeja do sistema) — **extensível por plugins da comunidade** — e consegue **desfazer tudo com um clique**. O app fica praticamente invisível em consumo, pausa o vídeo quando um jogo está em tela cheia e nunca deixa o Windows num estado quebrado.

**Não está no MVP** (roadmap, doc 05): GIF/WebP animado, integração TranslucentTB e modo portátil (v1.1); widgets de área de trabalho, wallpaper de shader, paleta automática, bordas de janela, sons, tela de bloqueio, ícones/fontes, Windhawk (Fase 2); Modo avançado (admin), plugins nativos, galeria online (Fase 3).

**Plataformas:** Windows 11 24H2 e 25H2 oficiais; 23H2 e Windows 10 22H2 no "melhor esforço" (D3).

---

## Fase 0 — Fundação (antes de qualquer feature nova) · ~2–3 semanas

Objetivo: base sólida, build reproduzível, testes e CI. **Nenhuma feature nova** até isso estar verde.

| # | Tarefa | Tamanho |
|---|---|---|
| F0.1 | Build verde imediato: corrigir C1/C2 da auditoria + incorporar sua correção de include pendente no `WallpaperPlaylist.cpp` (commit pequeno e isolado) | P |
| F0.2 | `CMakePresets.json` (debug/release/asan/tidy/fuzz), Ninja, `cmake/Warnings.cmake` + `cmake/Hardening.cmake`, *overlay triplet* endurecido | P |
| F0.3 | vcpkg: `wil`, `tomlplusplus`, `gtest`, `nlohmann-json`, `luau`; baseline fixado | P |
| F0.4 | Nova estrutura `apps/` + `libs/`. **Não consertar código que será reescrito**: `Database` → `libs/storage` (com `synchronous` configurável) e `WinError` (corrigido: C5) → `libs/base`; todo o resto vai para `legacy/`, fora do build | P |
| F0.5 | `libs/base`: `Log` (arquivo UTF-8 por processo, rotação, níveis, timestamp), `Error`, `Utf`, `CheckedCast`, `Limits` | M |
| F0.6 | Infra de testes: GoogleTest; registro em sandbox; **Windows Sandbox** (`.wsb` com o build mapeado somente-leitura) para testes de sistema | M |
| F0.7 | CI GitHub Actions: format, build, test, asan, codeql | M |
| F0.8 | Arquivos de comunidade: LICENSE (GPLv3), README (pt-BR + en), CONTRIBUTING, SECURITY, CODE_OF_CONDUCT, templates de issue/PR | P |
| F0.9 | ADR-0001 com as decisões D1–D10 já tomadas | P |

### Spikes (experimentos com prazo, código descartável em `spikes/`)

Tiram dúvidas técnicas **antes** de investir. Cada um tem prazo, critério de "não seguir" escrito **antes** de começar, e termina num ADR em `docs/adr/`. **Quando o critério está escrito, eu aplico a decisão sozinho** e te reporto — só paro se o resultado cair fora de todas as opções previstas.

**Máquinas de referência:** (1) este PC — Ryzen 5 5500, 16 GB, Radeon RX 6600, Win11 Pro 25H2; (2) notebook com bateria, se houver; (3) Windows Sandbox para testes que alteram o sistema.

| Spike | Prazo | Pergunta | Critério de decisão (definido agora) |
|---|---|---|---|
| **S0** — CI | 1 dia | O que os runners do GitHub conseguem rodar (Explorer/DWM? GPU? sessão interativa)? | define quais testes rodam no CI e quais só no Windows Sandbox |
| **S1** — Vídeo | 5 dias | Encaixe atrás dos ícones (layout novo e antigo) com DirectComposition; custo de 1080p30/60 e 4K com Media Foundation; pausa em tela cheia | > **150 MB** ou > **5% CPU** em 1080p30 → vídeo sai da v1.0 e vai para a v1.1 |
| **S2** — UI de configurações | 4 dias | Slint entrega a galeria com 500 miniaturas, bonita e dentro do orçamento? Narrador lê a galeria? | p95 de quadro > 16 ms rolando, início a quente > 600 ms, RAM > 120 MB, ou Narrador falhando → **WinUI 3** |
| **S3** — Barra nativa | 1 dia | Os ajustes sem injeção (doc 09 §5) funcionam em 24H2/25H2? Esconder a barra nativa + restaurar é confiável? O menu Iniciar/Win+X continuam funcionando com ela escondida? | ajuste que falhar sai do MVP; se esconder a nativa quebrar o Iniciar → barra própria convive com a nativa em auto-ocultar |
| **S4** — Aparência | 3 dias | `.theme` gerenciado + `IThemeManager2` aplica modo escuro e destaque sem logoff, em quanto tempo? Quais chamadas precisam do `--apply-helper`? | < **99% de sucesso em 200 alternâncias** → fallback só por registro + broadcast |
| **S5** — Identidade e fidelidade | 2 dias | Monitor reconectado em outra porta é reconhecido (EDID)? Snapshot restaura `TranscodedWallpaper`, Spotlight e slideshow? | o que não for restaurável vira `Fidelity::Lossy` com aviso |
| **S6** — Mitigações e CRT | 2 dias | ACG/*extension points* no core que usa COM do Shell; CRT estático convivendo com `slint_cpp.dll` | mitigação incompatível fica para depois; DLL exige runtime app-local → documentar |
| **S7** — Barra e plugins | 4 dias | UI dos widgets: (A) `.slint` interpretado ou (B) árvore declarativa própria em Direct2D? Luau no `rmw-plugin-host` + IPC para a barra: latência e RAM por VM | escolher a opção que cumprir: barra ≤ **25 MB** com 10 widgets, atualização ≤ **4 ms**, lote IPC ≤ **2 ms**, **e** imagens/fontes restritas à pasta do plugin; se as duas cumprirem → (A) Slint (menos código); se nenhuma → (B) com conjunto de widgets reduzido |
| **S8** — Bandeja | 2 dias | A técnica "spy window" (janela `Shell_TrayWnd` própria + repasse ao tray real) recebe ícones e encaminha cliques no 25H2, com os apps comuns (Discord, Steam, OneDrive, software da GPU)? | funciona com ≥ 90% dos apps testados → M5 como planejado; abaixo disso → bandeja da barra mostra o que conseguir + botão "abrir bandeja nativa" (barra nativa em auto-ocultar) |

**Critério de saída da Fase 0:** CI verde; estrutura nova com `Database` e `WinError` migrados e testados; código antigo isolado em `legacy/`; ADRs dos 9 spikes escritos; orçamentos do doc 01 confirmados (ou a decisão prevista aplicada).

---

## Fase 1 — MVP em 8 marcos

| Marco | Versão | Estimativa (semanas de trabalho focado) | O que você vê |
|---|---|---|---|
| M1 — Motor | `v0.1.0-alpha` | 3–4 | aplicar/desfazer rice pela CLI |
| M2 — Residente | `v0.2.0-alpha` | 2 | modo escuro por horário, bandeja do app, atalhos |
| **M3 — Barra** | `v0.3.0-alpha` | 4–5 | **sua barra própria, do tamanho e posição que quiser** |
| **M4 — Plugins** | `v0.4.0-alpha` | 4–5 | **widgets da comunidade em Luau**, tweaks, ações |
| **M5 — Bandeja** | `v0.5.0-alpha` | 3–4 | ícones da bandeja do sistema na sua barra |
| M6 — Vídeo | `v0.6.0-beta` | 3–4 | wallpaper animado por monitor |
| M7 — Interface | `v0.7.0-beta` | 5–7 | app de configurações, editor visual da barra, gerenciador de plugins |
| M8 — Pacote | `v1.0.0` | 3 | instalador, 6 rices, plugins de exemplo, importação `.rice`/`.rmwp` |

Total: ~27–34 semanas de trabalho focado (um desenvolvedor + agente de IA), recalibrado ao fim de cada marco. Cada marco termina com uma versão utilizável.

### M1 — "Motor" → `v0.1.0-alpha`

O coração: rice, snapshot, desfazer — **já na arquitetura final** (core + IPC + CLI cliente).

- `domain`: modelo `Rice`, parser de `rice.toml`, validação com mensagens claras (linha/coluna), seleção de monitores; leitor de **manifesto de pacote** comum a rices e plugins (doc 12 §4).
- `engine`: `RiceEngine` transacional, diário em `state.db` (`synchronous=FULL`), Store de conteúdo, snapshot base com detecção de outras ferramentas, histórico, `lastApplied` por campo, desfazer com comparação de 3 vias.
- `rmw-core` mínimo: instância única, pipe com handshake, encerra quando ocioso, modo `--apply-helper` com timeout.
- Módulos: **wallpaper estático** (pré-renderização por monitor, cor, slideshow nativo, identidade por EDID), **aparência** (claro/escuro, destaque, transparência — via `.theme` gerenciado + `IThemeManager2` ou fallback do S4), **cursores** (registro + `SPI_SETCURSORS`, Store, `install.inf`, tamanho), **barra nativa sem injeção** (alinhamento, Visão de Tarefas, Pesquisa, auto-ocultar).
- **Registro de ações** (`rice.apply`, `restore.original`, `appearance.toggle`, `wallpaper.next`…) — a mesma lista que plugins, atalhos e regras usarão.
- **CLI `rmw`** (inglês na v1.0): `apply`, `validate`, `status`, `restore [--original | --snapshot N]`, `export`, `monitors`, `action <nome>`.

**Aceite M1:** aplicar pela CLI um rice com wallpaper por monitor + modo escuro + destaque + cursores + barra nativa; `rmw restore --original` volta ao estado inicial (teste de contrato + teste de sistema no Windows Sandbox); matar o core no meio de um `apply` → na próxima execução o sistema é restaurado; duas CLIs ao mesmo tempo → a segunda recebe `busy`.

### M2 — "Residente" → `v0.2.0-alpha`

- Bandeja do app (só enquanto residente), entrada `Run` com `--startup`, modo zero residente.
- Eventos do sistema (doc 03 §9) e **drift ao vivo**.
- Agendador: **aparência agendada** e slideshow com regras por monitor.
- **Atalhos globais** (`RegisterHotKey`) ligados a ações; **regras básicas** (horário, bateria/tomada, monitor conectado) → ação/rice.
- Supervisor completo (Job com limite de notificação, heartbeat por thread de janela, backoff, fallback), testado com worker de teste.
- Modo seguro (`Shift` ao iniciar, `--safe-mode`, automático após 3 crashes na inicialização).

**Aceite M2:** modo escuro troca sozinho no horário; atalho dispara ação; mudar o wallpaper pelas Configurações do Windows desanexa o campo sem brigar; worker com vazamento proposital é encerrado e reiniciado; após 3 crashes, o modo seguro abre.

### M3 — "Barra" → `v0.3.0-alpha`

`rmw-bar.exe`: barra própria, processo supervisionado, **nunca executa código de plugin** (doc 12 §3).

- **Dimensionamento total**, por monitor, várias barras por monitor se quiser:
  - posição: cima, baixo, esquerda, direita, ou **flutuante** (sem reservar espaço, com margens);
  - espessura/altura, largura (inteira, centralizada, percentual), margens, raio dos cantos, borda, sombra;
  - fundo: sólido, transparente, **acrylic/mica/blur** (aplicado na **nossa** janela via DWM — sem injeção);
  - auto-ocultar da própria barra; respeitar apps em tela cheia (`ABN_FULLSCREENAPP`).
- Reserva de espaço como *AppBar* (`ABM_NEW → QUERYPOS → SETPOS`, `ABN_POSCHANGED`, `ABM_REMOVE`), DPI por monitor, reinício do Explorer, monitor conectado/desconectado.
- **Esconder a barra nativa** (auto-ocultar + `SW_HIDE` em `Shell_TrayWnd`/`Shell_SecondaryTrayWnd`, reesconder quando o Explorer mostrar) e **restaurá-la**: ao sair, no modo seguro, na desinstalação, e **pelo core se o `rmw-bar` cair**. **Atalho de emergência** que sempre restaura a barra nativa.
- Zonas esquerda/centro/direita (estilo Waybar) definidas em `[bar]` no rice, com **recarga automática** ao salvar o arquivo.
- Módulos embutidos (nativos, mas expostos como *providers*/ações para plugins poderem refazê-los):
  - botão Iniciar; **lista de janelas abertas** (regras do Alt+Tab, ícones, agrupar por app, ativar/minimizar, apps fixados, atualização via *shell hook* `RegisterShellHookWindow`);
  - relógio/data; desktops virtuais; mídia tocando (sessões de mídia do Windows); CPU/RAM/rede; bateria; volume; atalhos para Central de Ações/Notificações.
- Renderização conforme o spike S7.

**Aceite M3:** barra em qualquer tamanho/posição/monitor conforme o rice; barra nativa escondida e **restaurada em < 3 s** se o `rmw-bar` for morto; atalho de emergência funciona; DPI misto, reinício do Explorer e tela cheia corretos; ≤ 25 MB e ~0% CPU parada.

### M4 — "Plugins" → `v0.4.0-alpha`

Sistema de plugins do doc 12, ponta a ponta:

- `rmw-plugin-host.exe` (Job com teto, sem rede/arquivos/processos) com **Luau** sandboxed; um VM por plugin com cota de memória; interrupt + watchdog.
- *Providers* e **ações** com **permissões** verificadas; armazenamento por plugin (cota); rede **pelo host** só para domínios declarados.
- Pontos de extensão do MVP: `bar.widget`, `bar.skin`, `tweak` (P1 com lista permitida/bloqueada), `action`, `rule`, `settings.page` (esquema → formulário; até o M7, configurado por TOML).
- Ferramentas: `rmw plugin new | dev | validate | pack`, recarga automática, console de log, definições de tipo `rmw.d.luau`.
- **Prova da API**: 2 módulos embutidos da barra refeitos como plugins Luau + 5 plugins de exemplo (tocando agora, clima com rede, gráfico de CPU, lista de janelas alternativa, ação "modo foco").
- Documentação `docs/plugins/` + repositório-modelo.

**Aceite M4:** plugin com loop infinito / bomba de memória → host reiniciado, barra continua de pé; permissão negada é bloqueada; tweak que tenta gravar em `Run` é rejeitado na validação; recarga automática < 1 s; os 5 exemplos funcionam.

### M5 — "Bandeja" → `v0.5.0-alpha`

- Bandeja do sistema na barra própria pela técnica do S8: ícones, dicas, clique esquerdo/direito/duplo e menus de contexto repassados ao app, menu de excesso (overflow), reinício do Explorer, `TaskbarCreated`.
- Exposta como *provider* `tray` → plugins podem desenhar bandejas alternativas.
- Fallback do S8 se algum app não cooperar.

**Aceite M5:** os apps comuns testados aparecem e respondem; matar o Explorer → a bandeja se recompõe.

### M6 — "Vídeo" → `v0.6.0-beta`

- `rmw-wallpaper`: **MP4/H.264** por monitor, decodificação por hardware, apresentação via **DirectComposition** (HWND flip/blt como fallback); outros codecs se a extensão estiver instalada (testado com `CanPlayType`, com explicação do que falta).
- Encaixe nos dois layouts (24H2+ "raised desktop" e antigo) com auto-recuperação (doc 09 §1).
- Pausa em tela cheia/jogo, bateria/economia de energia, desktop coberto, sessão bloqueada, tela desligada; limite de FPS; mudo; pausado = para de apresentar na hora, libera buffers após 2 min.
- Robustez: Explorer, monitores, DPI, GPU removida (TDR), suspensão.

**Aceite M6:** orçamento do doc 01 na máquina 1; jogo em tela cheia → 0% CPU/GPU do renderizador; matar `rmw-wallpaper` → volta em < 3 s; travar a thread de janela → detectado em < 2 s; reiniciar o Explorer → wallpaper volta sozinho.

### M7 — "Interface" → `v0.7.0-beta`

`rmw-settings` (Slint, ou WinUI 3 se o S2 mandar), cliente do IPC, instância única, dona do `library.db` e do cache de miniaturas.

| Tela | Conteúdo |
|---|---|
| Início | rice ativo, prévia, campos desanexados, "Desfazer última mudança", "Restaurar Windows original" |
| Wallpapers | biblioteca, galeria virtualizada, mapa dos monitores (arrastar **ou** "Aplicar ao monitor ▸"), políticas de pausa |
| Aparência | claro/escuro/agendado, destaque, transparência |
| Cursores | esquemas com prévia, importar, tamanho |
| **Barra** | **editor visual**: arrastar widgets entre zonas, tamanho/posição/cantos/fundo com prévia ao vivo, uma barra por monitor |
| **Plugins** | instalados, permissões (com destaque das sensíveis), configurar (formulário gerado do esquema), ativar/desativar, logs |
| Rices | instalados, prévia, aplicar, duplicar, editar, exportar |
| Atalhos e regras | ações ligadas a teclas e gatilhos |
| Diagnóstico | processos (RAM/CPU), erros, logs, pacote de suporte |
| Configurações | iniciar com o Windows, manter na bandeja, idioma, sobre/licenças |

- i18n com `@tr()` + gettext `.po` (pt-BR e en); acessibilidade com papéis/nomes nos componentes próprios e alternativa por teclado para todo arrastar.

**Aceite M7:** tudo do M1–M6 possível pela UI; abre em < 600 ms (a quente); fechar libera 100% da memória dela; Narrador opera as telas principais; escala 100%–200%.

### M8 — "Pacote" → `v1.0.0`

- **Importar `.rice` e `.rmwp`** no `rmw-core --import-worker` (Job com teto), regras do doc 06 §4, fuzzing; exportar.
- **6 rices oficiais** (doc 11) com skins de barra, publicados como *assets* da release / repositório `rmw-rices`; **plugins de exemplo** publicados.
- **Instalador por usuário com Inno Setup** (`PrivilegesRequired=lowest`, Restart Manager) — ADR:
  - **Desinstalar:** parar o core; oferecer `rmw restore --original` (inclui **restaurar a barra nativa**); remover entrada `Run`, atalhos e chaves de teste; perguntar se mantém `%LOCALAPPDATA%\RicingManagerWindows`.
  - **Atualizar:** parar processos → migrar bancos e esquemas → reiniciar o core (o handshake garante versões compatíveis).
- **winget** + assinatura Certum (D5) + checksums SHA-256 + SBOM + atestação de build.
- Documentação de usuário e de plugins; checklist de QA executado.

**Aceite v1.0 (Definition of MVP):**

- [ ] Instala e desinstala sem admin; desinstalar oferece restaurar o Windows original e sempre devolve a barra nativa
- [ ] Rice completo aplicado em < 3 s típicos (medido) e desfeito sem resíduos (comparação por equivalência)
- [ ] Barra própria em qualquer tamanho/posição, com bandeja; barra nativa restaurada em qualquer falha
- [ ] Plugins de terceiros rodam isolados: plugin malicioso/travado não afeta barra, core nem sistema
- [ ] Orçamentos do doc 01 cumpridos e medidos (métrica: *Private Bytes*)
- [ ] Nenhum crash conhecido em aberto; workers se recuperam sozinhos
- [ ] Windows 11 24H2/25H2 testados; demais versões no melhor esforço (o que não funcionar é ocultado/explicado, nunca quebra)
- [ ] Multi-monitor com DPI misto e reconexão de monitor testados
- [ ] Pacotes maliciosos de teste (zip slip, ADS, symlink, zip bomb, imagem gigante, TOML profundo, `.ani` malformado, tweak em chave proibida) rejeitados sem crash
- [ ] CI verde com ASan e fuzzing
