# Diário de progresso

> Atualizado a cada tarefa. Uma nova sessão retoma daqui ("continua").

## Como o trabalho está organizado no Git (local, sem push)

- `main` — intocada até o dono do projeto decidir mesclar.
- `develop` — branch de integração. Cada tarefa é feita numa branch própria (`plan/…`, `fix/…`, `chore/…`, `feat/…`, `spike/…`) e mesclada em `develop` com `--no-ff`.
- Para ver tudo o que foi feito: `git log --oneline --graph develop`.

## Como compilar e testar

```powershell
.\scripts\build.ps1 -Preset debug -Test      # build + 222 testes de unidade
.\scripts\build.ps1 -Preset release -Test    # LTO, PDB, Spectre
.\scripts\build.ps1 -Preset asan -Test       # AddressSanitizer (dependências também instrumentadas)
.\scripts\sandbox.ps1 -Preset debug          # testes de unidade dentro do Windows Sandbox
.\scripts\sandbox.ps1 -Preset debug -Suite system   # aceites M1 + M2 no Windows Sandbox (54 verificações)
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
| M3 — Barra | 🔜 próximo |

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

## Pendências conhecidas

- **S0 (CI)** só pode rodar com push — aguardando autorização ("OK com push").
- Testes reais na sua máquina (fim do M3, M5, M6) — vou pedir permissão na hora.
- Meta de 90 MB do renderizador de vídeo (M6).
- Wallpaper: ajuste diferente por monitor precisa de pré-renderização (WIC no `--apply-helper`); por enquanto vale o ajuste da primeira regra (o parser avisa).
- Slideshow por monitor feito pelo core: coberto por testes de unidade; o teste de ponta a ponta precisa de 2 monitores (a Sandbox tem 1) — conferir no teste real do fim do M3.
- Regras de bateria/tomada e de monitor conectado: lógica pronta, sem teste de sistema (a Sandbox não simula bateria nem monitor novo).
- Regras de horário perdidas durante o sono não rodam depois (só a aparência agendada é corrigida ao acordar).
