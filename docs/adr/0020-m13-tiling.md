# ADR-0020 — M13: *tiling*, áreas de trabalho e atalhos

- **Status:** aceito
- **Data:** 2026-10-06
- **Contexto:** doc 14 §M13. O dono pediu *tiling* com atalhos de Ctrl, áreas de trabalho "estilo Linux e Mac", tudo opcional e configurável, e aceitou um modo avançado que pede administrador. Como no M11, nada é injetado em outros processos.

## Decisões

1. **Um worker próprio: `rmw-tiler`.**
   - **Ligado só quando a pessoa liga** (`settings.toml` → `[tiling] enabled`). O core o supervisiona como a barra: heartbeat, teto de memória (64 MB), reinício com espera.
   - **Mexe nas janelas dos outros só por fora:**
     - eventos de janela fora de contexto (`SetWinEventHook` com `WINEVENT_OUTOFCONTEXT`);
     - `SetWindowPos` e `ShowWindowAsync` assíncronos: um app travado nunca trava o *tiler*;
     - a cor da borda do DWM (`DWMWA_BORDER_COLOR`, Windows 11) na janela em foco.
   - **Que janelas entram:** as de app, visíveis, sem dono (os diálogos ficam com a janela deles), com moldura, não camufladas, nem do shell, nem as nossas (a janela de configurações entra). De tamanho fixo: flutuam. De app rodando como administrador: ficam de fora, pois o Windows não deixa um processo comum movê-las.
   - **Onde cada uma vai:** a moldura visível (os limites estendidos do DWM, sem as bordas invisíveis de redimensionar) cai no lugar. Uma segunda passada, 200 ms depois, corrige os apps que respondem tarde ou mudam de tamanho ao mudar de DPI. Uma janela maximizada pela pessoa fica como está até ela restaurar.
   - **A área útil:** muda quando uma barra entra ou sai. O *tiler* ouve `WM_SETTINGCHANGE` e, a cada 2 s, compara as áreas (uma barra que morre não avisa).

2. **O modelo sem Windows (`libs/tiling`).**
   - Layouts (dwindle, master-stack, colunas, monocle, flutuante): funções puras, testadas ao pixel. Com janelas demais, o dwindle para de dividir abaixo de 64 px e as últimas dividem o resto.
   - As áreas de trabalho de cada monitor, os atalhos, as regras por app, o plano (core → *tiler*) e o arquivo de devolução.

3. **Áreas de trabalho.**
   - **Como no Linux** (padrão): 1 a 9 em cada monitor, nossas.
     - Trocar de área mostra as janelas da nova e só depois esconde as da antiga, sem piscar o desktop.
     - Os diálogos de uma janela escondida vão e voltam com ela.
     - Uma janela de uma área escondida que vem para a frente (uma notificação clicada, por exemplo) traz a área dela.
   - **Como no Mac:** as áreas de trabalho do próprio Windows, com a animação dele.
     - **Ler:** `IVirtualDesktopManager` (documentada) diz em que área cada janela está; a chave do Explorer no registro dá a ordem e a atual, só lida e nunca escrita.
     - **Ir:** o próprio Ctrl+Win+setas do Windows.
     - **Mandar a janela de outro app para outra área:** o Windows não deixa. Fica com o Win+Tab, e o atalho volta a ser do app.

4. **Atalhos.**
   - Um gancho de teclado de baixo nível numa thread própria, como o da tecla Win (ADR-0019). Só a tecla do atalho, e a soltura dela, é engolida.
   - Depois de um atalho com Win sai a tecla de máscara (`0xE8`), de fora do gancho, para o Windows (e o nosso Iniciar) não abrirem o Iniciar.
   - **AltGr nunca conta como Ctrl+Alt:** no ABNT2 e em vários teclados ele digita caracteres. Os atalhos de fábrica não usam Ctrl+Alt com números.
   - Win+L e Ctrl+Alt+Del são recusados, pois são do Windows. Um atalho igual a outro, ou a um `[[hotkey]]` com o *tiling* ligado, é erro no `settings.toml`, e a janela de configurações não salva (diz o porquê).

5. **Tudo volta para o lugar.**
   - **O que o *tiler* guarda:** em `<data>\tiler\windows.json`, onde cada janela estava antes de ser posta e quais ele escondeu. Esconder e mostrar gravam na hora.
   - **Desligar** (o plano some), **o core sair** (`--core`) ou **a sessão terminar** (`WM_QUERYENDSESSION`, para um "salvar alterações?" não ficar escondido): ele devolve tudo e sai.
   - **Cair ou travar:** o core roda `rmw-tiler --restore` na hora, e o próximo *tiler* devolve o que achar no arquivo antes de começar. Um identificador de janela que outro processo reaproveitou não é tocado.

6. **A barra:** o widget `workspaces` (com a cara do `desktops`) mostra as áreas em uso e a atual no monitor da barra.
   - Lê um bloco de memória compartilhada (`Local\RicingManagerWindows.Tiler.State`, com número de sequência) e acorda com um evento.
   - Um único ouvinte por processo, para que cada barra de cada monitor seja avisada.
   - O clique pede a área à janela de controle do *tiler*.

7. **Modo avançado (`[tiling] run_as_admin`, opcional): janelas de apps rodando como administrador também.**
   - **Por quê:** o Windows não deixa um processo comum mover a janela de um processo administrador, nem ouvir as teclas digitadas nela (um atalho não faz nada com ela na frente).
   - **Como:** o *tiler* supervisionado continua comum e pede ao UAC uma cópia de si mesmo como administrador (`ShellExecuteEx` com `runas`).
     - **A cópia** faz o trabalho e acaba junto com ele (`--elevated-by <pid>`).
     - **O *tiler* comum** fica batendo o heartbeat para o supervisor.
     - **Quem acaba com um deles** (desligar, o core sair, o supervisor achar que travou) acaba com os dois, e a cópia devolve as janelas.
     - **Se a pessoa diz não:** o *tiler* trabalha como comum e não pergunta de novo até começar outra vez.
   - **O que a cópia compartilha** com as barras (memória e evento) é do usuário, em integridade média, e não só dos administradores, como seria por padrão. A janela de controle dela aceita das barras "ir para a área N" e do core o pedido para sair.
   - **Uma cópia que não ganhou os direitos** (um Windows com o UAC desligado) trabalha como está e nunca pede de novo, para não abrir cópias sem fim.
   - **"Administrador" é a integridade do processo** (alta ou mais), não a marca de elevação do token: é ela que o Windows olha.
   - **`--restore` de um processo comum** devolve o que pode e deixa no arquivo só as janelas de administrador, para a próxima cópia elevada devolver.

8. **Também (correção do M10):** um rice só de barras, mantendo a barra do Windows, era recusado ("não muda nada"), pois a barra não é um módulo do motor. O motor agora aceita um rice sem mudanças de módulo quando o core diz que ele muda algo fora deles, e a CLI diz o que aconteceu.

## Números

| Medida | Resultado |
|---|---|
| Trocar de área (do atalho até as janelas visíveis, Sandbox) | 15 ms indo para uma área vazia, 80 ms voltando para 4 janelas (aceite: < 100 ms) |
| *Tiler* derrubado com janelas escondidas | todas visíveis de novo em 122–138 ms; ele volta e organiza tudo de novo |
| Como no Mac (Sandbox) | a janela de uma área nova é organizada sozinha; Ctrl+Win+1 volta à área 1 em 70 ms, com a animação do Windows |
| Modo avançado (Sandbox) | o *tiler* comum abre a cópia e continua batendo; matá-lo faz a cópia devolver tudo e sair; um *tiler* comum deixa a janela de um app de administrador no lugar exato. A Sandbox roda com o UAC desligado (tudo já é administrador): o pedido ao UAC em si se confere no PC real |
| Parado | 1,9 MB de memória privada, 0,00% de CPU em 10 s |
| Testes | suíte `tiler` na Sandbox (61 verificações), `settings`; layouts, áreas, atalhos, regras, plano e arquivo de devolução nos testes de unidade; alvo de *fuzzing* `tiling` (o plano, o arquivo de devolução e o modelo de áreas com operações ao acaso) |
