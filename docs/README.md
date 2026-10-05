# Planejamento — Ricing Manager for Windows

> Status: **versão 3, final, aguardando seu "OK"** · 2026-10-05 · Nenhum código foi alterado ainda.
> Passou por: 8 pesquisas técnicas, 1 revisão adversarial de consistência, 1 checagem técnica de fatos e as suas decisões. O que acontece depois do "OK" está no [doc 13](13-plano-de-execucao.md).

Customizador geral do Windows 11, open source (GPLv3), nativo em C++: wallpaper estático e em vídeo, temas, cores, cursores, **barra de tarefas própria totalmente dimensionável**, ícones, fontes — **tudo extensível por plugins da comunidade** — leve, seguro e reversível por padrão; poderoso por escolha explícita.

## Resumo

1. **O código atual não compila** (5 erros) e não linka (2 erros); há bugs de crash, vazamento e um que **troca a seta normal do mouse** ([doc 02](02-auditoria-codigo-atual.md)). `Database` e `WinError` são aproveitados; o resto é reescrito com testes.
2. **Tudo é um "rice"** (`rice.toml` + assets) e **tudo é extensível** ([doc 12](12-plugins.md)): widgets e skins da barra, tweaks, ações, regras, páginas de configuração — com a mesma API que nós usamos.
3. **Transacional e reversível**: snapshot, diário durável, "Restaurar Windows original" sempre disponível; depois de aplicar, **o usuário manda** (o app não briga com mudanças feitas pelo Windows).
4. **Processos pequenos e isolados**: core único que altera o sistema (e que nem fica rodando se nada dinâmico estiver ativo), barra própria, host de plugins em sandbox, renderizador de vídeo, UI que some ao fechar. Um crash derruba só uma parte, que volta sozinha.
5. **Barra própria logo depois do motor** (sua escolha "a"): qualquer tamanho/posição/monitor, flutuante ou reservando espaço, cantos, acrylic/mica, lista de janelas, bandeja do sistema, módulos e **widgets da comunidade em Luau**. A barra nativa é escondida e **sempre restaurada** em qualquer falha.
6. **Consumo estimado**: 0 MB com rice estático · ~12 MB com agendamento · barra ≤ 25 MB · ~100 MB e ≤ 3% CPU com vídeo 1080p · 0% durante jogo · UI ≤ 120 MB só aberta.
7. **Segurança em 7 camadas** contra problemas de memória; pacotes e plugins tratados como hostis; plugins com permissões e sandbox; **nosso código nunca injeta** e nunca patcha o sistema.
8. **Níveis de risco** ([doc 10](10-modo-avancado-e-integracoes.md)): Normal (padrão) → Integrações por clique (TranslucentTB, Windhawk — eles injetam, nós não) → Modo avançado com admin (ícones do sistema, `.msstyles` experimental).
9. **Pacote inicial** com 6 rices originais/livres ([doc 11](11-pacote-inicial.md)); **Miku = pacote da comunidade**, feito à parte para você postar, sem vínculo com o projeto.
10. **Caminho**: Fase 0 (fundação + 9 spikes) → MVP em 8 marcos (~27–34 semanas de trabalho focado; a barra aparece no 3º) → v1.1 → Fases 2–4.

## Decisões (todas tomadas)

| # | Decisão | Escolha |
|---|---|---|
| D1 | Licença | **GPLv3** |
| D2 | UI de configurações | **Slint** (troca para WinUI 3 se reprovar no spike S2) |
| D3 | Windows | **Win11 24H2/25H2 oficiais**; 23H2 e Win10 22H2 no melhor esforço |
| D4 | Vídeo no MVP | **sim** (MP4/H.264), sujeito ao critério do spike S1 |
| D5 | Assinatura | **Certum Open Source** (€25–69/ano) a partir da 1ª beta pública — compra sua, só perto da release |
| D6 | Idioma | código/commits em inglês; docs e UI em pt-BR + en |
| D7 | Nome | "Ricing Manager for Windows" (`rmw-*`) |
| D8 | Fluxo Git | branch por tarefa; `main` protegida |
| D9 | Modo avançado (admin) | Fase 3, experimental, componente opcional por máquina |
| D10 | TranslucentTB | v1.1 |
| D11 | Barra própria | **logo depois do motor** (marco M3) |
| D12 | Extensibilidade | **tudo extensível por plugins da comunidade** (doc 12) |
| D13 | Miku/anime | pacote da **comunidade**, fora do projeto |

## Para começar, só falta

1. Você reiniciar o PC com a **Área Restrita do Windows** ativada.
2. Responder **"OK"** — ou **"OK com push"** se quiser que eu também envie as branches para o GitHub (o repositório é público; sem push, tudo fica só local e o spike de CI espera).

Componentes do VS (Spectre libs + Clang 20.1.8): **instalados e verificados**. **Nenhum outro reinício** é necessário no plano.

## Documentos

| Doc | Conteúdo |
|---|---|
| [01 — Visão e princípios](01-visao-e-principios.md) | objetivo, 7 princípios, fora de escopo, metas de RAM/CPU |
| [02 — Auditoria do código atual](02-auditoria-codigo-atual.md) | bugs encontrados (com linha) e o que é reaproveitado |
| [03 — Arquitetura](03-arquitetura.md) | processos, core único escritor, contrato de módulo, transações, drift, Store, `rice.toml` (com barra e plugins), IPC, eventos |
| [04 — MVP](04-mvp.md) | Fase 0, 9 spikes com critérios, 8 marcos com aceite (barra no M3, plugins no M4, bandeja no M5) |
| [05 — Roadmap e escalabilidade](05-roadmap-e-escalabilidade.md) | v1.1 e Fases 2–4 |
| [06 — Segurança, memória e estabilidade](06-seguranca-memoria-e-estabilidade.md) | ameaças, 7 camadas, limites, zip seguro, plugins, dumps, IPC, assinatura |
| [07 — Interface (UI)](07-interface-ui.md) | UI em C++: Slint para configurações; motor da barra decidido no S7 |
| [08 — Engenharia](08-engenharia-build-testes-ci.md) | toolchain, flags, regras de código, testes (incl. Windows Sandbox), CI |
| [09 — Pesquisa técnica](09-pesquisa-tecnica.md) | fatos verificados com fontes e nível de confiança |
| [10 — Modo avançado e integrações](10-modo-avancado-e-integracoes.md) | níveis de risco, TranslucentTB/Windhawk por clique, ajudante admin, `.msstyles` |
| [11 — Pacote inicial](11-pacote-inicial.md) | 6 rices oficiais, licenças, pacote da comunidade Miku |
| [12 — Plugins](12-plugins.md) | pontos de extensão, níveis de confiança, manifesto, permissões, orçamentos, ferramentas |
| [13 — Plano de execução](13-plano-de-execucao.md) | o que o "OK" autoriza, ordem exata, onde eu paro e pergunto |
