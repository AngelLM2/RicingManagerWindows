# ADR-0003 — Spike S3: barra de tarefas nativa sem injeção

- **Status:** aceito
- **Data:** 2026-10-05
- **Spike:** `spikes/s3-taskbar/` (executado **dentro do Windows Sandbox**, Win11 25H2 build 26200)

## Pergunta

Os ajustes sem injeção da barra nativa (doc 09 §5) funcionam no Windows atual? Esconder a barra nativa (para a barra própria do M3) é confiável e o menu Iniciar continua funcionando?

## Resultados (verificados via UI Automation na barra real)

| Item | Resultado |
|---|---|
| Botão Visão de Tarefas (`ShowTaskViewButton` + broadcast `TraySettings`) | ✅ aplica **ao vivo** |
| Modo da Pesquisa (`SearchboxTaskbarMode`) | ✅ aplica **ao vivo** (0 = oculto; 1 = ícone) |
| Ocultar automaticamente (`SHAppBarMessage(ABM_SETSTATE)`) | ✅ ao vivo |
| Alinhamento (`TaskbarAl`) | ⚠️ **não** aplica ao vivo (nem com broadcast, nem mensagem direta à `Shell_TrayWnd`, nem `WM_SETTINGCHANGE` genérico); ✅ aplica **após reiniciar o Explorer** |
| Esconder a barra nativa (auto-ocultar → `ShowWindow(SW_HIDE)`) | ✅ ficou escondida (uma chamada bastou nesta execução) |
| Menu Iniciar com a barra escondida (`WM_SYSCOMMAND/SC_TASKLIST` e tecla Windows) | ✅ abre igual ao caso com a barra visível |
| Restaurar a barra nativa (`SW_SHOW` + `ABS_ALWAYSONTOP`) | ✅ |

Observação: no 25H2 a janela que vira primeiro plano ao abrir o Iniciar é uma `Windows.UI.Core.CoreWindow` do `SearchHost.exe` (título "Search"), não do `StartMenuExperienceHost.exe`. A detecção do Iniciar precisa considerar os dois.

## Decisões

1. **MVP (M1)**: Visão de Tarefas, modo da Pesquisa e auto-ocultar como ajustes ao vivo.
2. **Alinhamento** fica no MVP, mas a UI informa *"aplica ao reiniciar o Explorer"* e oferece um botão explícito **"Reiniciar o Explorer agora"** (fecha janelas do Explorer abertas — por isso nunca é automático). Sem o clique, vale no próximo logon.
3. **Barra própria (M3)**: esconder a nativa com *auto-ocultar → `SW_HIDE`*, mais um vigia (`SetWinEventHook(EVENT_OBJECT_SHOW)`) que reesconde quando o Explorer a mostrar de novo (reinício do Explorer, DPI, monitor). Restaurar sempre: saída, modo seguro, desinstalação, crash do `rmw-bar` (pelo core) e atalho de emergência.
4. O botão Iniciar da nossa barra abre o menu com `WM_SYSCOMMAND/SC_TASKLIST` para a `Shell_TrayWnd` (funciona com a barra escondida).

## Limitações / a confirmar

- **Win11 24H2 não testado** (não há ambiente 24H2 disponível; o Windows Sandbox usa o mesmo build do host). 23H2/Win10 seguem como "melhor esforço" (D3).
- **Multi-monitor** (`Shell_SecondaryTrayWnd`) não testável no Sandbox (um monitor virtual) → teste real no fim do M3.
- Confirmar visualmente, no teste real do M3, que a superfície aberta é o Iniciar (e não só a Pesquisa) no 25H2.
