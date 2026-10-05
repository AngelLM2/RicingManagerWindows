# 13 — Plano de execução (o que acontece depois do seu "OK")

> Objetivo: depois que você responder **"OK"**, eu sigo sozinho este roteiro, sem precisar de novas instruções, parando **só** nos pontos listados em §3.

## 1. O que o "OK" autoriza

| Autorizado | Detalhe |
|---|---|
| Git local | criar a branch `plan/mvp` e commitar estes docs; depois uma branch por tarefa (`feat/…`, `fix/…`, `chore/…`) com commits locais em Conventional Commits |
| Sua mudança pendente | incorporar a correção de include que está pendente em `src/managers/wallpaper/WallpaperPlaylist.cpp` no primeiro commit de código (F0.1) |
| Reestruturar o código | mover o código antigo para `legacy/`, criar `apps/` e `libs/`, escrever código novo com testes, seguindo os docs 03, 08 e 12 |
| Dependências de desenvolvimento | via vcpkg (fixado no baseline): `wil`, `tomlplusplus`, `sqlite3`, `gtest`, `nlohmann-json`, `luau` (e `libzip` no M8); **SDK C++ do Slint** (releases oficiais em github.com/slint-ui/slint, algumas dezenas de MB) nos spikes S2/S7; `clickgen` (pip, MIT) para gerar cursores no M8 |
| Testes | compilar, rodar testes unitários/contrato/ASan/fuzzing localmente; criar configurações `.wsb` e testar no **Windows Sandbox** à vontade (descartável, não toca no seu Windows) |
| Spikes | executar S0–S8 e **aplicar a decisão** pelos critérios já escritos no doc 04, registrando em ADR |
| Arquivos de CI | criar os workflows em `.github/workflows/` (só rodam no GitHub depois de um push) |
| Docs | manter `docs/` e um diário de progresso `docs/PROGRESSO.md` atualizado a cada tarefa |

## 2. Ordem de execução

### Fase 0 — Fundação

| Passo | Tarefa (doc 04) | Resultado |
|---|---|---|
| 0 | branch `plan/mvp` + commit dos docs | planejamento versionado |
| 1 | F0.1 — build verde | o projeto atual compila e linka |
| 2 | F0.2 + F0.3 — presets CMake/Ninja, warnings, hardening, triplet, dependências | `cmake --preset debug` funciona do zero |
| 3 | F0.4 + F0.5 — estrutura nova, `legacy/`, `libs/base`, `Database`/`WinError` migrados | base limpa |
| 4 | F0.6 — GoogleTest, registro em sandbox, Windows Sandbox | `ctest` verde |
| 5 | F0.7 — workflows de CI | arquivos prontos |
| 6 | F0.8 + F0.9 — LICENSE GPLv3, README pt-BR/en, CONTRIBUTING, SECURITY, CODE_OF_CONDUCT, templates, ADR-0001 (decisões D1–D10) | projeto "apresentável" |
| 7 | Spikes, nesta ordem: **S6** (mitigações/CRT) → **S3** (barra nativa) → **S5** (monitores/snapshot) → **S4** (aparência) → **S1** (vídeo) → **S2** (UI) → **S7** (barra e plugins) → **S8** (bandeja) → **S0** (CI, precisa de push) | 9 ADRs com decisões aplicadas |

### Fase 1 — MVP

M1 Motor → M2 Residente → **M3 Barra** → **M4 Plugins** → **M5 Bandeja** → M6 Vídeo → M7 Interface → M8 Pacote (detalhes e critérios de aceite no doc 04). Cada marco termina com: todos os critérios de aceite verificados, tag local `v0.N.0`, resumo para você.

Em paralelo, depois do M3: rascunho do **pacote da comunidade Miku** fora do repositório (doc 11 §6).

### Ciclo de cada tarefa

1. Implementar a menor fatia que entrega valor.
2. Compilar (`debug` e `release`) sem warnings.
3. Testar: unitários + contrato + ASan; Windows Sandbox quando mexer no sistema.
4. Revisar o próprio diff (correção, segurança, memória, extensibilidade) e corrigir.
5. Commitar; atualizar `docs/PROGRESSO.md`.

## 3. Onde eu paro e pergunto

| Situação | Por quê |
|---|---|
| **Push / PR no GitHub** | o repositório é público: push publica o código. Se quiser que eu faça push das branches, responda **"OK com push"** em vez de "OK" (nunca faço push na `main` direto; sempre branch). O spike S0 (CI) só acontece com push. |
| Testar algo que **altere o seu Windows real** fora do Sandbox (ex.: esconder sua barra nativa, trocar seu wallpaper/cursor) | é a sua máquina de uso; no Sandbox fica tudo descartável. Vou concentrar esses testes reais em poucos momentos (fim do M3, M5 e M6), com o roteiro de desfazer pronto |
| Baixar algo **fora** da lista do §1 (ex.: as ilustrações oficiais da Miku) | aviso nome, origem e tamanho antes |
| Qualquer coisa que exija **admin** | não executo elevação por conta própria |
| Resultado de spike **fora** das opções previstas | decisão nova é sua |
| Achado de **segurança** sério ou bloqueio técnico sem alternativa no plano | você decide o rumo |
| Custo em dinheiro (ex.: certificado Certum) | é sua compra |

## 4. Continuidade entre sessões

- Todo o estado fica no Git (commits por tarefa) e em `docs/PROGRESSO.md` (o que foi feito, o que vem a seguir, decisões tomadas, pendências).
- Se a conversa terminar no meio, uma nova sessão retoma a partir desse diário — basta dizer "continua".

## 5. Reinicializações

- **Windows Sandbox**: precisa do reinício que você já vai fazer.
- **Componentes do VS** (Spectre libs + Clang): instalados com `--norestart`; normalmente não pedem reinício (se o instalador pedir, eu aviso).
- **Nada mais no plano exige reiniciar o PC.** Testes no Windows Sandbox não precisam de reinício; dependências do vcpkg/Slint/pip também não.
