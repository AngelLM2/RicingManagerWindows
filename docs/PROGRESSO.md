# Diário de progresso

> Atualizado a cada tarefa. Uma nova sessão retoma daqui ("continua").

## Como o trabalho está organizado no Git (local, sem push)

- `main` — intocada até o dono do projeto decidir mesclar.
- `develop` — branch de integração. Cada tarefa é feita numa branch própria (`plan/…`, `fix/…`, `chore/…`, `feat/…`, `spike/…`) e mesclada em `develop` com `--no-ff`.
- Para ver tudo o que foi feito: `git log --oneline --graph develop`.

## Como compilar e testar

```powershell
.\scripts\build.ps1 -Preset debug -Test      # build + 290 testes (283 de unidade + 7 da barra, sem janela)
.\scripts\build.ps1 -Preset release -Test    # LTO, PDB, Spectre
.\scripts\build.ps1 -Preset asan -Test       # AddressSanitizer (dependências também instrumentadas)
.\scripts\sandbox.ps1 -Preset debug          # testes de unidade dentro do Windows Sandbox
.\scripts\sandbox.ps1 -Preset debug -Suite system   # aceites M1 + M2 no Windows Sandbox (54 verificações)
.\scripts\sandbox.ps1 -Preset debug -Suite bar      # aceites M3 (barra própria)
.\scripts\sandbox.ps1 -Preset debug -Suite plugins  # aceites M4 (plugins, 43 verificações)
.\scripts\sandbox.ps1 -Preset debug -Suite tray     # aceites M5 (bandeja, 43 verificações)
.\scripts\tidy.ps1                             # clang-tidy completo (-Path aceita UMA pasta por vez)
.\scripts\fetch-deps.ps1 -Name slint         # SDK do Slint fixado (hash verificado, nunca executa instalador)
.\scripts\build.ps1 -Preset release -CMakeArgs '-DRMW_BUILD_SPIKES=ON'   # spikes
```

Particularidade desta máquina: o perfil `C:\Users\user'\` tem apóstrofo, o que quebra ports do vcpkg baseados em meson. O `scripts\env.ps1` detecta isso e põe os caches do vcpkg em `build\.vcpkg\`.

## Estado atual

| Item | Estado |
|---|---|
| Planejamento (docs 01–13) | ✅ aprovado ("OK" em 2026-10-05), sem push |
| **Fase 0 — fundação (F0.1–F0.9)** | ✅ build moderno, libs, CLI, 47 testes (debug/release/ASan/Sandbox), CI pronto, comunidade, ADR-0001 |
| **Spikes** | ✅ 8 de 9 concluídos (ADR-0002 a ADR-0009); **S0 (CI) aguarda push** |
| **M1 — Motor** | ✅ `v0.1.0-alpha` (tag local): core + `--apply-helper` + CLI; 4 módulos; 203 testes de unidade (debug/ASan/Sandbox) + suíte de sistema na Sandbox com os 4 critérios de aceite |
| **M2 — Residente** | ✅ `v0.2.0-alpha` (tag local): core residente (janela + eventos do sistema), `settings.toml` com atalhos e regras, aparência agendada, slideshow por monitor, drift ao vivo, bandeja só quando residente, `--startup`/autostart, supervisor de workers, modo seguro; 222 testes de unidade (debug/release/ASan/Sandbox) + 54 verificações de sistema na Sandbox |
| **M3 — Barra** | ✅ `v0.3.0-alpha` (tag local) — testado no seu PC (sem DPI misto nem 2 monitores, que o PC não tem) — implementado (ADR-0010): `rmw-bar` com janela própria (AppBar, DPI, camada com alfa / acrylic / mica), 11 widgets embutidos, barra nativa escondida e sempre devolvida, atalho de emergência, auto-ocultar, recarga ao salvar o rice; aceites verificados na Sandbox |
| **M4 — Plugins** | ✅ `v0.4.0-alpha` (tag local) — implementado (ADR-0011): `rmw-plugin-host` (Luau em sandbox, uma VM por plugin, culpa por plugin), widgets `.slint` validados e compilados pela barra, skins, tweaks (módulo do motor, com desfazer), ações e regras; permissões aprovadas pelo usuário; `rmw plugin new/dev/validate/pack/install/approve/enable/disable/remove/list/status`; 7 exemplos; guia em `docs/plugins/` com `rmw.d.luau` conferido pelos testes; 290 testes (debug/release/ASan) + aceites na Sandbox (suíte `plugins`, 43 verificações) |
| **M5 — Bandeja** | ✅ `v0.5.0-alpha` (tag local) — implementado (ADR-0012): widget `tray` com os ícones dos apps (cliques por versão, menus, dicas, excesso como no Windows), janela da bandeja numa thread própria com o Explorer mantido em dia por outra, provedor `tray` e ação `tray.click` para plugins (exemplo `mini-tray`), aviso na tela quando um plugin é parado; aceites na Sandbox (suíte `tray`, 43 verificações). M6 — Vídeo é o próximo |

### Resultado dos spikes

| Spike | Decisão | ADR |
|---|---|---|
| S6 — mitigações | core e `--apply-helper` com ACG + extension points + carga de imagens + MicrosoftSignedOnly | 0002 |
| S3 — barra nativa | ajustes ao vivo (alinhamento só após reiniciar o Explorer); esconder/restaurar ok; Iniciar funciona com a barra escondida | 0003 |
| S5 — monitores/snapshot | identidade em níveis (EDID nem sempre existe e o seu tem série genérica); restaurar wallpaper e slideshow ok | 0004 |
| S4 — aparência | `.theme` derivado do tema ativo + `IThemeManager2`: 199/200 trocas, ~200 ms | 0005 |
| S1 — vídeo | fica na v1.0: 0,2% CPU; 54–106 MB (meta de 90 MB vira tarefa do M6); encaixe atrás dos ícones ok | 0006 |
| S2 — UI | Slint com renderizador por software: 10–18 MB, ~123 fps, abre em 65 ms, acessível | 0007 |
| S7 — barra + plugins | widgets `.slint` interpretados (7,8 MB, 0,01% CPU) + Luau em host isolado (0,35 MB/plugin); scripts hostis contidos | 0008 |
| S8 — bandeja | "spy window" funciona: captura, repassa, encaminha cliques e devolve à bandeja real | 0009 |

## Registro

| Data | Passo | Branch | Resultado |
|---|---|---|---|
| 2026-10-05 | 0 — planejamento versionado | `plan/mvp` | docs 01–13 + este diário |
| 2026-10-05 | F0.1 — build verde | `fix/green-build` | protótipo compila e linka |
| 2026-10-05 | F0.2–F0.9 — fundação | `chore/build-system` | build moderno, libs base/platform/storage, CLI, 47 testes, CI, docs de comunidade, ADR-0001; binário com ASLR/DEP/CFG/CET/EH-continuation e só `KERNEL32.dll` importada |
| 2026-10-05 | S6 | `spike/s6-mitigations` | ADR-0002 |
| 2026-10-05 | S3 (Sandbox) | `spike/s3-taskbar` | ADR-0003 |
| 2026-10-05 | S5 (host + Sandbox) | `spike/s5-monitors-snapshot` | ADR-0004 |
| 2026-10-05 | S4 (Sandbox) | `spike/s4-appearance` | ADR-0005 |
| 2026-10-05 | S1 (host + Sandbox) | `spike/s1-video` | ADR-0006 |
| 2026-10-05 | S2 (host + Sandbox) | `spike/s2-ui` | ADR-0007 + `fetch-deps.ps1` |
| 2026-10-05 | S7 (host + Sandbox) | `spike/s7-bar-plugins` | ADR-0008 + Luau no vcpkg |
| 2026-10-05 | S8 (Sandbox) | `spike/s8-tray` | ADR-0009 |
| 2026-10-05 | M1 — domínio | `feat/domain-rice` | `rice.toml` com diagnósticos (linha/coluna), PathRules, seletores de monitor; **achado:** o toml++ percorre aninhamentos de forma recursiva → profundidade medida antes do parse (arquivo de 64 KB com `a.a.a…` derrubaria o processo) |
| 2026-10-05 | M1 — clang-tidy | `feat/domain-rice` | o preset `tidy` nunca tinha rodado de fato (o CMake perde as flags `/…` do `cl.exe`); agora `scripts/tidy.ps1` + job no CI, zero achados |
| 2026-10-05 | M1 — storage, JSON, arquivos | `feat/domain-rice` | `Statement`/`Transaction`/migrations; JSON com limites; SHA-256, cópia sem links, troca atômica |
| 2026-10-05 | M1 — engine | `feat/domain-rice` | `RiceEngine` transacional (diário durável, desfazer em ordem inversa, recuperação após queda, restauração em 3 vias, histórico de 20), Store por conteúdo, `state.db` v1 |
| 2026-10-05 | M1 — módulos | `feat/domain-rice` | aparência (`IThemeManager2`), barra nativa, cursores (validação RIFF/ICO própria + `install.inf`), wallpaper estático; **achado:** no seu PC o `QueryDisplayConfig` devolve caminho de monitor vazio → identidade e casamento por retângulo (adendo no ADR-0004) |
| 2026-10-05 | M1 — IPC, core, CLI | `feat/domain-rice` | pipe com DACL e checagem do servidor, JSON-RPC, `rmw-core` (instância única, ocioso → sai, recupera ao iniciar), `--apply-helper` em Job com timeout de 15 s, CLI completa (`apply/validate/status/history/restore/export/monitors/inspect/action`) |
| 2026-10-05 | M1 — teste de sistema (Sandbox) | `feat/domain-rice` | ✅ aplica os 4 módulos e restaura tudo; `busy`; matar o core no meio → desfeito no próximo início; export válido. **Achados:** `SPI_SETCURSORS` com `SPIF_UPDATEINIFILE\|SPIF_SENDCHANGE` retorna FALSE mesmo funcionando (agora sem flags; doc 09 corrigido); cursores do Windows gravados com caminho absoluto eram copiados para o Store |

| 2026-10-05 | M2 — `settings.toml` | `feat/resident` | atalhos (`KeyChord`), regras (horário, bateria/tomada, monitor conectado), `keep_in_tray`; motor ganha drift/desanexar/reaplicar e operações automáticas sem histórico |
| 2026-10-05 | M2 — core residente | `feat/resident` | janela oculta na thread principal (eventos do doc 03 §9, pasta `config\` vigiada sem polling), pipe numa thread, fila de trabalho; agendador; drift com debounce; bandeja pt-BR/en; `rmw settings reload`, `rmw autostart`; modo seguro (contador fora do `state.db`). **Achados:** `CreateWindowEx` falhava porque o `WM_NCCREATE` chegava antes de guardarmos o HWND; a CLI conectava num core de outra pasta de dados (o pipe é por usuário) — agora avisa; um comando inválido não sobe mais o core |
| 2026-10-05 | M2 — supervisor | `feat/resident` | Job com limite de notificação a 80% via IOCP, heartbeat por contador compartilhado, backoff 1/2/4 s, 3 falhas em 5 min desativam; `rmw_test_worker` vaza/trava/cai de propósito e os testes usam processos e Jobs reais |
| 2026-10-05 | M2 — slideshow por monitor + testes de sistema | `feat/resident` | slideshow que o Windows não consegue tocar (pastas diferentes por monitor) é trocado pelo core; ✅ Sandbox: atalho, modo escuro no horário, drift do wallpaper, autostart, `--startup`, modo seguro após 3 crashes. **Achados:** consultas de leitura (`inspect`) renovavam a janela de "mudança nossa" e escondiam o drift; o log só gravava Warn+ na hora (agora Info+); o `tidy.ps1 -Path a,b` não checava nada (a lista chega como uma string só) — a execução completa achou e corrigimos o que faltava; o README tinha `\t` virado TAB |

| 2026-10-05 | M3 — biblioteca da barra | `feat/bar` | plano (JSON do core para a barra), geometria (AppBar, margens, comprimento, DPI), geração do `.slint` raiz por zonas |
| 2026-10-05 | M3 — `rmw-bar` | `feat/bar` | Slint sobre loop Win32 próprio; translucidez por duas passadas (o renderizador só escreve RGB); supervisor com worker que sobrevive ao core para devolver a barra nativa. **Achados:** o Slint limpa o quadro para preto mesmo com fundo `transparent` (daí o *matte* preto/branco); o `run_event_loop` padrão sai quando a última janela some (a reconstrução após reiniciar o Explorer fechava a barra); `Screen.WorkingArea` do .NET fica em cache sem fila de mensagens (o teste passou a usar `SPI_GETWORKAREA`); `CaptureBlt` é recusado como enum pelo `CopyFromScreen` |
| 2026-10-05 | M3 — widgets e acabamento | `feat/bar` | lista de janelas, áreas de trabalho, mídia (C++/WinRT do SDK), CPU/RAM/rede, bateria, volume, notificações; auto-ocultar; recarga ao salvar o `rice.toml`; a CLI substitui um core de outra versão. ✅ Sandbox (suíte `bar`): reserva/flutuante/duas barras, nativa escondida, matar a barra → nativa de volta em 120–240 ms, atalho de emergência, reinício do Explorer, `restore`; 9,1 MB e ~0% de CPU. **Achado:** a versão do projeto ficou em 0.1.0 durante o M2 (a tag é `v0.2.0-alpha`); agora 0.3.0 |

| 2026-10-05 | M3 — teste real no seu PC | `fix/bar-real-test` | ✅ barra, widgets, ícones dos seus apps, mídia do Spotify, kill → nativa de volta em 153 ms, emergência, `restore` completo. **Achados:** a lista de janelas passava por baixo do relógio do centro; a mica do DWM fica clara e chapada numa janela sem foco. Os dois foram corrigidos e conferidos no seu PC |

| 2026-10-05 | M4 — manifesto e validador de widgets | `feat/plugins` | `plugin.toml` (tipos, permissões, esquema de configuração, `[widget]` com structs/propriedades/callbacks, skins, tweaks, ações) com diagnósticos por caminho; validador do `.slint` com *tokenizer* próprio (imports, `@image-url`, `Timer`, janelas, prefixos `Rmwp<n>-`) |
| 2026-10-05 | M4 — runtime Luau e host | `feat/plugins` | `rmw::scripting` (VMs em sandbox, API `rmw.*` 1.0, timers, provedores, ações, widgets, storage, HTTP pelo host, JSON); `rmw-plugin-host` (provedores compartilhados com a barra, WinHTTP sem redirecionamento/cookies e sem endereços locais, pipes de controle e de ponte, registro de culpa); barra com widgets de plugin e cache de valores; core com `PluginManager`, aprovação e plano do host; CLI `rmw plugin …` |
| 2026-10-05 | M4 — tweaks e exemplos | `feat/plugins` | módulo `tweaks` (allowlist/block-list de `HKCU`), motor lembra originais de campos novos; 7 plugins de exemplo; cores `palette.*` resolvidas com a paleta do rice |
| 2026-10-05 | M4 — aceites na Sandbox | `feat/plugins` | ✅ suíte `plugins` 43/43: exemplos instalados e rodando, valores na barra, bomba de memória / laço infinito / padrão patológico parados sem afetar os outros, host morto e reiniciado, permissões negadas, aprovação de permissões novas, tweak aplicado e restaurado, `Run` recusado, modo foco, recarga < 1 s (221–317 ms), host 7,4 MB e barra 10,9 MB com 6 plugins. **Achados:** qualquer restrição de UI no Job esconde as janelas dos outros processos (`windows.list` vazio) — removidas; o host recebe eventos de janela com segundos de atraso → `window.activate` decidido pela barra e lista lida também a cada 1 s; o Luau interrompe até `string.find` patológico; scripts salvos com BOM não compilavam |
| 2026-10-05 | M4 — documentação e modelo | `feat/plugins` | guia `docs/plugins/` (manifesto, API, widgets, tweaks, segurança), `plugins/template` (que o `rmw plugin new` embute), `rmw.d.luau` conferido pelo analisador do Luau contra todos os exemplos, ADR-0011. **Achados:** a conferência de tipos pegou exemplos usando valores que podem ser `nil`; um erro do Slint num widget derrubava todos os widgets de plugin da barra e apontava a linha do fonte gerado → agora cada widget é tentado sozinho, só o quebrado sai, e o erro aponta a linha do `widget.slint` |

| 2026-10-05 | M5 — bandeja | `feat/tray` | `rmw::bar::tray` (mensagens do `Shell_NotifyIcon`, modelo, cliques por versão), `TrayHost` (janela `Shell_TrayWnd` numa thread própria no `rmw-bar`, cópia dos ícones em pixels, convite aos apps inclusive janelas *message-only*, ícones promovidos do `NotifyIconSettings`), widget `tray` com dica nativa e excesso, `platform::nativeTrayWindow()` no lugar de todo `FindWindow("Shell_TrayWnd")`, provedor `tray`/ação `tray.click`/imagens `tray:<id>` para plugins, balão quando um plugin é parado |
| 2026-10-05 | M5 — aceites na Sandbox | `feat/tray` | ✅ suíte `tray` 43/43. **Achados:** a segunda pergunta do `Shell_NotifyIconGetRect` é o tamanho, não o canto; um `NIM_ADD` repetido (nós pedimos aos apps) volta "falhou" — vira `NIM_MODIFY`; o Explorer recém-reiniciado recusa ícones por ~30 s — o que ele recusa é reenviado; e o principal: com o repasse síncrono, as chamadas de AppBar da própria barra ficavam na fila atrás dos ícones de todos os apps enquanto o Explorer reiniciava, a interface passava de 2 s e o supervisor desligava a barra — agora a janela da bandeja responde aos apps na hora e outra thread mantém o Explorer em dia; a dica ficava sobre o cursor (tamanho lido antes de existir) e piscava |
| 2026-10-05 | M5 — regressões | `feat/tray` | ✅ `bar` 28/28 e `plugins` 43/43 de novo. **Achado (vinha do M3, não do M5):** a suíte `system` falhava em "restaurar depois de reiniciar o Explorer" — bisseção em *worktree* (v0.2.0 passa; v0.3.0, v0.4.0 e M5 falham; entre o último commit bom e a v0.3.0 só muda código da barra, que nem roda ali): o Windows às vezes aceita o tema e mantém o acento antigo logo depois de o Explorer reiniciar. Agora o tema gerenciado alterna entre dois arquivos e é reaplicado (até 3 vezes, a última pelo gerenciador legado) quando a cor não aparece; a mensagem de tempo esgotado diz o esperado e o que o registro mostra. `system` 54/54 |

## Pendências conhecidas

- **S0 (CI)** só pode rodar com push — aguardando autorização ("OK com push").
- **Combinado em 2026-10-05:** eu termino M5–M8 sem parar para pedir permissão (sem push, sem compras, sem mexer no seu Windows: tudo testado na Sandbox) e você faz o teste real no final, com o monitor secundário conectado por último. O que conferir lá: bandeja com os seus apps (Discord, Steam, OneDrive, GPU), vídeo, interface, instalador, dois monitores e DPI misto, slideshow por monitor.
- Meta de 90 MB do renderizador de vídeo (M6).
- Wallpaper: ajuste diferente por monitor precisa de pré-renderização (WIC no `--apply-helper`); por enquanto vale o ajuste da primeira regra (o parser avisa).
- Slideshow por monitor feito pelo core: coberto por testes de unidade; o teste de ponta a ponta precisa de 2 monitores (a Sandbox tem 1) — no teste real final.
- Regras de bateria/tomada e de monitor conectado: lógica pronta, sem teste de sistema (a Sandbox não simula bateria nem monitor novo).
- Regras de horário perdidas durante o sono não rodam depois (só a aparência agendada é corrigida ao acordar).
- Plugins: instalar de `.rmwp` (M8, com libzip); `launch.app` e a página de configurações gerada do esquema (M7); notificação na tela quando um plugin é parado (M5, bandeja); alvos de *fuzzing* para o manifesto, o validador do `.slint`, os tweaks e a ponte (com os demais, antes da beta).
