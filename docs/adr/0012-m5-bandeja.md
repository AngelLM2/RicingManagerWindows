# ADR-0012 — M5: a bandeja do sistema na barra

- **Status:** aceito
- **Data:** 2026-10-05
- **Contexto:** doc 04 (M5), ADR-0009 (S8: "spy window"), ADR-0010 (barra própria), ADR-0011 (plugins)

## Decisões

1. **A janela da bandeja mora no `rmw-bar`, numa thread própria.** É uma janela da classe `Shell_TrayWnd`, topmost, marcada com uma propriedade (`RicingManagerWindows.TrayWindow`). Fica à frente da do Explorer, reafirmada a cada 250 ms quando outra passa na frente, e assim o `Shell_NotifyIcon` dos apps a encontra primeiro. Ela só existe enquanto algo usa a bandeja (o widget `tray` ou um plugin com o provedor `tray`) e sai 3 s depois do último uso: uma reconstrução da barra não a derruba.

2. **Os apps não esperam por nós; o Explorer é mantido em dia em outra thread.** Uma mensagem de ícone (`WM_COPYDATA`, `dwData = 1`) é decodificada, aplicada ao nosso modelo e respondida na hora. Outra thread (`TrayForwarder`) leva a mesma mensagem ao Explorer.
   - **Por quê, medido na Sandbox:** quando o Explorer reinicia, todos os apps readicionam seus ícones ao mesmo tempo e o Explorer novo demora a responder. As chamadas `SHAppBarMessage` da própria barra também chegam a essa janela; com o repasse síncrono, elas ficavam na fila atrás de todos os ícones. A interface da barra passava dos 2 s que o supervisor tolera e era desligada.
   - **O que continua síncrono:** mensagens de AppBar (`dwData = 0`), consultas de retângulo e o resto, porque a resposta delas importa. Mas nada mais fica na frente delas.

3. **O Explorer recebe uma cópia nossa do ícone, e o que ele recusa é reenviado.** Os apps destroem o `HICON` logo depois do `Shell_NotifyIcon`. Por isso a cópia (`CopyIcon`) é feita enquanto o app ainda espera e é destruída quando o Explorer a recebe.
   - **Explorer recém-reiniciado:** ele recusa ícones por até ~30 s (medido na Sandbox). O que foi recusado volta a ser enviado a cada 2 s, por até 2 min, na ordem de cada ícone.
   - **Ícone que o Explorer já tem:** um `NIM_ADD` repetido vira `NIM_MODIFY`. Nós pedimos aos apps que readicionem; responder "falhou" faria alguns apps tentarem sem parar.

4. **Pedir os ícones aos apps.** O `TaskbarCreated` é postado a todas as janelas de outros processos, inclusive às *message-only*, que o broadcast do Explorer não alcança. Por isso os ícones delas se perdem quando o Explorer reinicia; os nossos voltam. Isso acontece:
   - quando a janela da bandeja sobe;
   - 2 s depois de o Explorer reiniciar;
   - quando a janela sai;
   - quando o core vê a barra cair.

   Às janelas do nosso processo nunca é postado: a barra se reconstruiria.

5. **Ninguém mais procura a barra do Windows com `FindWindow("Shell_TrayWnd")`.** Com a janela da bandeja à frente, isso devolveria a nossa. `platform::nativeTrayWindow()` pula a janela marcada e é usada no botão Iniciar, no esconder/mostrar, no auto-ocultar e no PID do Explorer do core.

6. **Cliques como o Explorer faz, por versão do ícone:**
   - **Mensagens por versão:** antes da 4, `wParam` é o id e `lParam` o evento. Na 4, `wParam` leva o ponto e `lParam` leva o evento com o id na palavra alta. A partir da 3, o clique esquerdo ganha `NIN_SELECT` e o direito, `WM_CONTEXTMENU`.
   - **Clique duplo:** gera `WM_LBUTTONDBLCLK`, e o segundo clique simples do Slint é descartado.
   - **Foco:** antes de postar, `AllowSetForegroundWindow(pid)`, porque o app precisa vir para a frente para mostrar o menu.
   - **`Shell_NotifyIconGetRect`:** responde com um retângulo em volta do clique na nossa barra, durante 5 s. Fora disso, quem responde é o Explorer. O shell pergunta primeiro o canto superior esquerdo e depois o **tamanho** (medido: ele soma a segunda resposta à primeira).

7. **Dicas e excesso:**
   - **Dicas:** a dica padrão é um tooltip nativo (comctl32 6) posicionado à mão, acima da barra quando ela está embaixo. Um ícone versão 4 sem `NIF_SHOWTIP` recebe `NIN_POPUPOPEN`/`NIN_POPUPCLOSE` e mostra o próprio balão.
   - **O que aparece na barra:** os ícones que o Windows mostra na barra dele (`HKCU\Control Panel\NotifyIconSettings`: `IsPromoted`, `ExecutablePath` com pastas conhecidas, `UID`), vigiados por `RegNotifyChangeKeyValue`. O resto fica atrás de uma seta que expande na própria barra (a barra não hospeda janelas popup); `show = "all"` mostra todos.

8. **Plugins:**
   - **Provedor `tray`:** é sensível, porque as dicas dizem o que o usuário faz. A barra é dona da bandeja: o host pede com `Watch` e a barra manda os valores com `Provide`.
   - **Ação `tray.click`:** passa pela barra, como as de janela.
   - **Imagens:** `tray:<id>` desenha o ícone e exige o provedor.
   - **Exemplo:** o `mini-tray` refaz a bandeja como plugin.

9. **Plugin parado avisa na tela:** é um balão do ícone do core (pendência do M4).

## Adiado

- **Fallback do S8 ("abrir a bandeja nativa"):** a captura cobre os apps que usam `Shell_NotifyIcon`, inclusive janelas *message-only*. Se o teste real mostrar um app que não coopera, o fallback entra.
- **Ícones com GUID que trocam de executável:** o casamento com `NotifyIconSettings` é por caminho e `UID`.

## Números (Sandbox, build debug)

| Medida | Resultado |
|---|---|
| Suíte `tray` | 43/43 |
| Ícones de antes da barra (janela comum e *message-only*) | capturados em < 0,1 s depois de a janela da bandeja subir |
| Cliques (versões 0, 3 e 4), duplo, menu, dica, `GetRect` no ponto do clique | ok |
| Explorer reiniciado | ícones de volta, barra sem travar (antes do encaminhamento assíncrono: desligada pelo supervisor) e no lugar certo |
| Barra morta | ícones continuam na bandeja do Windows; de volta com a barra |
| Ícones promovidos (`NotifyIconSettings`) e excesso | ok |
| Plugin vendo e clicando a bandeja; `mini-tray` | ok |
| Barra com a bandeja | 8,9 MB privados |
