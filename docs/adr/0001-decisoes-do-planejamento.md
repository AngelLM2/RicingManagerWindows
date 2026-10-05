# ADR-0001 — Decisões do planejamento

- **Status:** aceito
- **Data:** 2026-10-05
- **Decisores:** dono do projeto (aprovação "OK"), com recomendação do agente

## Contexto

O planejamento (docs 01–13) listou decisões que dependiam do dono do projeto. Todas foram tomadas antes de iniciar a Fase 0.

## Decisões

| # | Decisão | Escolha | Consequência principal |
|---|---|---|---|
| D1 | Licença | **GPL-3.0-or-later** | Slint utilizável sob GPLv3; elegível à assinatura gratuita da SignPath; forks precisam continuar abertos |
| D2 | UI de configurações | **Slint**, com critério objetivo de troca para WinUI 3 no spike S2 | build 100% CMake; RAM baixa com renderizador por CPU |
| D3 | Versões do Windows | **Win11 24H2/25H2 oficiais**; 23H2 e Win10 22H2 no melhor esforço | menos matriz de testes; caminho de desktop "antigo" só como fallback |
| D4 | Vídeo no MVP | **sim** (MP4/H.264), sujeito ao critério do spike S1 | wallpaper animado na v1.0 se o orçamento for cumprido |
| D5 | Assinatura de código | **Certum Open Source** a partir da 1ª beta pública | compra do dono do projeto, perto da release |
| D6 | Idioma | código/commits em inglês; docs e UI em pt-BR + en | — |
| D7 | Nome | "Ricing Manager for Windows", executáveis `rmw-*` | — |
| D8 | Fluxo Git | branch por tarefa → `develop` (`--no-ff`); `main` só por decisão do dono | sem push até autorização ("OK" sem push) |
| D9 | Modo avançado (admin) | Fase 3, experimental, componente opcional por máquina | doc 10 |
| D10 | TranslucentTB | v1.1 | doc 10 §3 |
| D11 | Barra própria | **logo após o motor** (marco M3) | MVP passa a 8 marcos (~27–34 semanas) |
| D12 | Extensibilidade | **tudo extensível por plugins da comunidade** | doc 12; API de plugins no M4 |
| D13 | Miku/anime | pacote da **comunidade**, fora do projeto | doc 11 §3 e §6 |

## Observações de execução

- Licença registrada como **GPL-3.0-or-later** (recomendação da FSF para "GPLv3").
- `WinError` foi para `libs/platform` (e não `libs/base`, como o doc 04 F0.4 dizia): ele precisa de `<windows.h>`, e `libs/base` é portátil.
- `luau` entra no `vcpkg.json` só quando for usado (spike S7 / M4), para manter os builds da Fase 0 rápidos.
