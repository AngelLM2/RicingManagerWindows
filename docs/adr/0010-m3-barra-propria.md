# ADR-0010 — M3: a barra própria (`rmw-bar`)

- **Status:** aceito
- **Data:** 2026-10-05
- **Contexto:** doc 04 (M3), ADR-0003 (barra nativa), ADR-0007 (Slint), ADR-0008 (widgets e plugins)

## Decisões

1. **Janela nossa, Slint só desenha.** O `rmw-bar` implementa a *Platform API* do Slint sobre um loop Win32 próprio. Cada barra é uma janela nossa por monitor, com AppBar, DPI por monitor, entrada de mouse e o lugar dela na ordem Z. O Slint (renderizador por software, interpretador) só faz layout e desenho. Essa é a decisão 5 do ADR-0008, confirmada.

2. **Translucidez com um renderizador só RGB: duas passadas.** O renderizador por software do Slint 1.18 escreve RGB e limpa o quadro com o fundo da janela, e `transparent` vira preto. Por isso cada quadro translúcido é desenhado duas vezes: com o fundo da janela preto e depois branco (a propriedade `bar-matte` da raiz gerada). Para cada pixel, `alfa = 255 − (branco − preto)`, e a passada sobre preto já é a cor pré-multiplicada. Isso foi verificado num teste sem janela: opaco 255, meio-transparente 128 ±2, vazio 0. O custo (duas renderizações) é irrelevante numa barra (~0% de CPU com um relógio por segundo). Barras sólidas sem cantos usam uma passada só.

3. **Apresentação.**
   - Sólida, transparente e cantos arredondados: janela *layered* (`UpdateLayeredWindow`, alfa por pixel). Pixels totalmente transparentes deixam o mouse passar.
   - Acrylic: janela normal, com `SetWindowCompositionAttribute(ACCENT_ENABLE_ACRYLICBLURBEHIND)`. A API não é documentada, mas é estável desde o Windows 10; é a única que mantém o acrylic numa janela que nunca recebe foco. Ela é aplicada **na nossa janela** (sem injeção); se faltar, só o tom é desenhado.
   - Mica: o acrylic acima, sob um tom espesso do tema (80%). O `DWMWA_SYSTEMBACKDROP_TYPE` foi testado e descartado no teste real (ver abaixo): a mica do DWM só aparece na janela com foco, e a barra nunca tem foco, então ela ficava clara e chapada, com o texto ilegível no modo escuro.

4. **O `rmw-bar` sobrevive a um crash do core, de propósito.** O doc 03 §2.3 diz que os workers morrem com o core (`KILL_ON_JOB_CLOSE`). A barra é a exceção: se morresse junto, a barra nativa ficaria escondida até o Explorer reiniciar. O Job dela não tem `KILL_ON_JOB_CLOSE`; ela recebe `--core <handle>`, vigia o processo do core e, quando ele some, mostra a barra nativa e sai. O supervisor continua com teto de memória, heartbeat e backoff, e a encerra explicitamente ao parar.

5. **A barra nativa volta em qualquer saída.** Quem a mostra de volta:
   - a própria barra, ao sair;
   - o core, no instante em que a barra cai, trava, vaza ou sai (evento do supervisor; medido na Sandbox: 120–240 ms);
   - o core, ao iniciar sem barra para mostrar e ao parar;
   - o atalho de emergência **Ctrl+Alt+Win+T**: fixo e documentado, desliga a barra (`bar.off`) e mostra a nativa na hora, mesmo com uma operação em andamento ou a barra travada.

   O espaço da nativa sai pelo **auto-ocultar**, um campo do módulo `taskbar` aplicado pelo motor. Assim, `restore` o devolve como qualquer outra mudança.

6. **A barra segue o arquivo.** O core escreve o plano em `<data>\bar\bars.json` (escrita atômica) e a barra o vigia: mudou, ela se reconstrói; sumiu, ela sai. Salvar o `rice.toml` aplicado atualiza as barras ao vivo; o resto do rice espera o próximo apply. Um arquivo com erro mantém as barras como estão.

7. **Widgets embutidos** = componentes `.slint` + provedores C++:
   - `start`, `clock`, `windows` (regras do Alt+Tab, sem janelas *cloaked* de outras áreas de trabalho, WinEvents fora de contexto, ícones por `WM_GETICON` com timeout de 50 ms);
   - `desktops` (registro do Explorer, vigiado por evento; trocar = Ctrl+Win+setas, pois não há API pública);
   - `media` (GSMTC via C++/WinRT do SDK);
   - `cpu`, `memory`, `network`, `battery`, `volume` (Core Audio, notificação de mudança via COM);
   - `notifications`.

   Plugins (M4) trazem os próprios componentes; os dados deles vêm do *plugin host*, nunca de código rodando na barra.

## Números (Sandbox, build debug)

| | Medido | Meta |
|---|---|---|
| Memória privada, todos os widgets | 9,1 MB | ≤ 25 MB |
| CPU parada (relógio por segundo) | 0,00–0,16% | ~0% |
| `rmw-bar` morto → barra nativa de volta | 120–240 ms | < 3 s |

## Teste real (PC do dono do projeto, com permissão, 2026-10-05)

Um monitor 1920×1080 a 100%, modo escuro, transparência ligada, build release, numa pasta de dados separada; no fim, `restore` devolveu tudo (barra nativa, área de trabalho, auto-ocultar).

| | Resultado |
|---|---|
| Barra sólida com todos os widgets | ✅ reserva 44 px; a nativa fica escondida; os ícones dos apps aparecem (Spotify, Teams, Discord e Claude, inclusive os da Store); a mídia mostra o que o Spotify toca |
| Memória / CPU (release) | `rmw-bar` com 11,7 MB privados e 0,00% em 10 s; `rmw-core` com 2,3 MB |
| `rmw-bar` morto | a nativa volta em **153 ms**; a barra reinicia e a esconde de novo |
| Ctrl+Alt+Win+T e `bar.on` | ✅ |
| Acrylic | ✅ |
| **Achado:** muitas janelas abertas | a lista passava por baixo do relógio do centro → as zonas das pontas agora param antes do centro e a lista encolhe (teste novo `TheStartZoneNeverRunsIntoTheCenter`) |
| **Achado:** mica | clara e chapada (a do DWM é "inativa" sem foco) → agora é acrylic sob um tom do tema. As cores que o rice não define seguem o claro/escuro do Windows, e a barra se refaz quando o tema muda |

**Não verificado:** DPI misto e dois monitores (o PC tem um monitor só). O código trata DPI por monitor (`WM_DPICHANGED`, `GetDpiForMonitor`) e uma barra por monitor, mas isso continua sem teste em hardware.

Melhoria futura: ícones de apps da Store que não respondem a `WM_GETICON` (ícone por AppUserModelID).
