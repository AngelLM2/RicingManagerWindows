# 01 — Visão, princípios e metas

## Visão

Um **customizador geral do Windows 11 (Windows 10 no "melhor esforço", decisão D3), open source, nativo em C++**, que junta num só lugar o que hoje exige 6–10 ferramentas diferentes (Wallpaper Engine/Lively, TranslucentTB, Auto Dark Mode, Rainmeter, YASB, pacotes de cursor, ajustes de registro…), com três diferenças:

1. **Leve de verdade** — o que fica residente custa poucos MB e ~0% de CPU quando nada está acontecendo. Peso só existe enquanto uma feature pesada está ativa (ex.: vídeo de fundo), e some quando ela é desligada.
2. **Seguro e reversível** — nada é alterado sem um *snapshot* do estado anterior. Um clique (ou um comando) desfaz tudo. Nenhum arquivo do sistema é modificado. Nada de admin por padrão.
3. **Tudo é um "rice"** — o visual inteiro (wallpaper por monitor, cores, modo claro/escuro, cursores, barra, bordas, sons…) é descrito num arquivo de texto compartilhável (`rice.toml` + assets). Dá para versionar no Git, trocar com amigos, aplicar por linha de comando.

## Para quem

| Perfil | O que quer | Como o app atende |
|---|---|---|
| Usuário comum | "Deixar o PC bonito" sem medo de quebrar | UI bonita, galeria de temas, botão "desfazer tudo" |
| Ricer / entusiasta | Controle total, dotfiles, scripts | `rice.toml` legível, CLI, widgets programáveis (fase 3) |
| Gamer / notebook | Wallpaper animado sem perder FPS/bateria | Pausa automática em tela cheia, bateria, janela maximizada |
| Contribuidor | Código limpo para ajudar | Arquitetura modular, testes, docs, CI |

## Princípios (em ordem de prioridade — em conflito, o de cima vence)

1. **Não quebrar o computador do usuário.**
   Toda mudança é transacional (aplica tudo ou nada), tem snapshot e é reversível. Nunca patchamos arquivos do sistema. **Nosso código nunca injeta** em outros processos. Falha de uma feature nunca derruba as outras.
2. **Seguro por padrão — poderoso por escolha.**
   Sem admin e sem injeção **por padrão**. O que é arriscado existe, mas só por **escolha explícita** do usuário, com explicação clara do risco e de como desfazer: o *Modo avançado* (admin com UAC a cada operação) e as *integrações* que instalam ferramentas de terceiros que injetam, como o TranslucentTB (doc 10). Pacotes de tema são *dados*, nunca código. Código de terceiros só roda em sandbox. Entrada externa (zip, imagens, TOML, IPC) é tratada como hostil: limites de tamanho, validação, fuzzing.
3. **Leve por padrão.**
   Orientado a eventos (sem *polling*). Processos pesados só existem enquanto necessários. Orçamentos de memória/CPU medidos no CI (ver metas abaixo).
4. **Nativo e respeitoso com o Windows.**
   Usar APIs públicas quando existirem; APIs não documentadas só atrás de uma camada de compatibilidade com detecção de versão e *fallback* (degradar, nunca crashar).
5. **Declarativo e compartilhável — e o usuário manda.**
   O estado desejado é um arquivo; ao **aplicar**, o app leva o sistema até ele. Depois disso, se o usuário mudar algo pelo próprio Windows, o app **não briga**: aquele item deixa de ser gerenciado e o usuário é avisado (doc 03 §5.2). A UI é só uma forma de editar esse arquivo.
6. **Tudo extensível pela comunidade.**
   Toda feature nasce como ponto de extensão: dados expostos (*providers*), ações, configuração com esquema e estilo por *tokens*. O que nós fazemos usa a mesma API que os plugins da comunidade usam — e plugins rodam isolados, com permissões (doc 12).
7. **Aberto e transparente.**
   Sem telemetria. Logs e dumps ficam locais. Builds reproduzíveis e assinados (fase de release).

## Fora de escopo (decisões conscientes)

| Não vamos fazer | Motivo |
|---|---|
| Patchar `uxtheme.dll`, `imageres.dll` ou qualquer arquivo do sistema | Altera arquivos do sistema, quebra em updates, é vetor de malware. (Temas `.msstyles` completos existem pelo Modo avançado + ferramenta de terceiros escolhida pelo usuário — doc 10) |
| Injetar código a partir do nosso binário | Risco técnico e de reputação (a Microsoft revogou o certificado do Windhawk por isso). Quem quiser, instala TranslucentTB/Windhawk pelo nosso botão — doc 10 |
| Substituir o `explorer.exe` como shell | Alto risco, enorme superfície; preferimos complementar |
| Exigir admin para features básicas | Superfície de ataque e desconfiança do usuário; admin só no Modo avançado opt-in |
| Plugins `.dll` carregados no processo principal | Um plugin ruim derruba tudo ou rouba dados |
| Telemetria, contas, nuvem | Privacidade; não é necessário |
| Suporte a Windows 7/8.1 | APIs modernas (DWM, `IDesktopWallpaper`, WinRT) |
| Window manager tiling próprio | Já existem ótimos (komorebi, GlazeWM); podemos integrar depois |
| Substituir a fonte do sistema (`FontSubstitutes`) | Exige HKLM, não é suportado pela Microsoft, quebra negrito/símbolos e deixa a interface inconsistente. Fontes do rice são instaladas por usuário e aplicadas onde é seguro (doc 10 §2) |

## Metas mensuráveis (orçamentos)

Metas definidas **antes** de medir. Os spikes S1/S2 (doc 04) medem na máquina de referência; se estourar, vale o critério de "não seguir" de cada spike (a meta **não** é ajustada para caber no que foi medido). Depois viram testes de regressão no CI (doc 08).

**Métrica de memória:** *Private Bytes* (memória comprometida), não o "conjunto de trabalho" do Gerenciador de Tarefas. Em builds ASan os orçamentos não se aplicam.

| Componente | Estado | Private Bytes | CPU | Outros |
|---|---|---|---|---|
| Núcleo (`rmw-core`) | sem nada residente | **0 MB** (encerra sozinho) | 0% | — |
| Núcleo (`rmw-core`) | residente (agendamento, vídeo…) | ≤ 12 MB | ~0% (0 wakeups periódicos) | — |
| Barra própria (`rmw-bar`) | por barra, com 10 widgets | ≤ 25 MB | ~0% parada | atualização ≤ 4 ms |
| Host de plugins (`rmw-plugin-host`) | 10 plugins Luau | ≤ 20 MB + até 16 MB por plugin (teto) | ~0% parado | — |
| Wallpaper estático / slideshow nativo | ativo | **0 MB extra** (usa o Windows) | ~0% | — |
| Renderizador de vídeo (`rmw-wallpaper`) | 1 monitor, 1080p H.264, decodificação por hardware | ≤ 90 MB | ≤ 3% | GPU ≤ 5%; calibrar por fabricante de GPU (o driver também conta na memória) |
| Renderizador de vídeo | cada monitor adicional | + ≤ 30 MB | + ≤ 1% | — |
| Renderizador de vídeo | pausado (jogo em tela cheia) | libera buffers após 2 min | 0% | 0 quadros apresentados desde o 1º instante |
| Interface (`rmw-settings`) | aberta | ≤ 120 MB | ~0% parada | abre em < 600 ms (início a quente) |
| Interface | fechada | **0 MB** (processo encerrado) | 0% | — |
| Aplicar um rice completo | — | — | — | < 3 s típicos; aplicação de tema com timeout de 15 s; sempre reversível |
| Instalador | — | ≤ 25 MB | — | sem admin (instalação por usuário) |

Metas de qualidade:

- 0 crashes conhecidos sem issue aberta; crash de *worker* recuperado em < 3 s; travamento detectado em < 2 s.
- 100% dos módulos passam no teste de contrato "aplicar → restaurar → estado **equivalente**" (contra um sistema falso no CI e contra o Windows real no Windows Sandbox).
- Parsers de entrada externa (zip, TOML, INF, cursores, IPC) com fuzzing contínuo.
- Funciona com: múltiplos monitores, DPI misto, monitor HDR (conteúdo SDR exibido corretamente; HDR10 nativo fica para depois), troca de monitor a quente, reinício do Explorer, bloqueio/desbloqueio, suspensão.
