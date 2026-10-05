# ADR-0009 — Spike S8: bandeja do sistema na barra própria ("spy window")

- **Status:** aceito — M5 segue como planejado; cobertura de apps reais confirmada no teste real do M5
- **Data:** 2026-10-05
- **Spike:** `spikes/s8-tray/` (executado **somente no Windows Sandbox**: intercepta as mensagens de bandeja de todos os apps)

## Pergunta

A técnica "spy window" (janela própria da classe `Shell_TrayWnd`, topmost, que recebe `WM_COPYDATA` do `Shell_NotifyIcon` e repassa à bandeja real — usada por Zebar/YASB) funciona no Win11 25H2, mantendo os ícones na bandeja real e encaminhando cliques aos apps? Critério do plano: ≥ 90% dos apps comuns → M5 como planejado.

## Método

- Dois "apps de teste" do próprio spike criam ícones (`NIM_ADD` + `NOTIFYICON_VERSION_4`, dica "rmw test N") e os recriam em `TaskbarCreated`; o app 1 começa **antes** da janela espiã, o app 2 **depois**.
- A espiã: `RegisterClass("Shell_TrayWnd")`, janela `WS_EX_TOPMOST` reafirmada periodicamente, `ChangeWindowMessageFilterEx(WM_COPYDATA)`, decodifica a estrutura de 32 bits (`dwSignature = 0x34753423`), **repassa primeiro** à `Shell_TrayWnd` real (`SendMessageTimeout` + `SMTO_ABORTIFHUNG`) e só então registra o ícone; difunde `TaskbarCreated` para os apps se registrarem de novo.
- Cliques: a espiã posta a mensagem de *callback* do app com `WM_LBUTTONUP` e `WM_CONTEXTMENU` (semântica da versão 4).
- Ao final, a espiã é destruída e `TaskbarCreated` é difundido de novo.

## Resultados

| Teste | Resultado |
|---|---|
| `FindWindow("Shell_TrayWnd")` encontra a espiã primeiro | ✅ |
| Ícones capturados | ✅ **4**: os 2 apps de teste (inclusive o registrado antes da espiã, recuperado via `TaskbarCreated`) **e** 2 ícones do próprio Windows ("Remover Hardware com Segurança", "Áudio Remoto") |
| Repasse à bandeja real | ✅ 0 falhas; `Shell_NotifyIconGetRect` confirma os ícones na bandeja real enquanto a espiã existe |
| Clique e menu de contexto encaminhados ao app | ✅ o app recebeu `click` e `menu` |
| Remover a espiã | ✅ os apps se registram de novo na bandeja real |

## Decisões

1. **M5 usa a técnica "spy window"**, sem injeção: decodificar `WM_COPYDATA` (`dwData = 1`), repassar **antes** de processar, reafirmar topmost, filtro de mensagens liberado, desenhar os ícones (`HICON` é objeto de sessão, utilizável direto), encaminhar cliques respeitando a versão do ícone (0–3 vs 4).
2. A bandeja vira o *provider* `tray` (doc 12 §5) → plugins podem desenhar bandejas alternativas.
3. **Robustez obrigatória**: se a espiã/barra cair, o core destrói a janela e difunde `TaskbarCreated` (os apps voltam à bandeja real — comportamento validado aqui); reinício do Explorer → reencontrar a bandeja real.
4. Fallback (critério do S8): se no teste real algum app não cooperar, botão "abrir bandeja nativa" com a barra nativa em auto-ocultar.

## A confirmar no teste real do M5 (na máquina do usuário, com permissão)

- Apps reais (Discord, Steam, OneDrive, software da GPU etc.) — não instaláveis no Sandbox sem downloads fora do combinado.
- Ícones que ignoram `TaskbarCreated` e apps iniciados antes da barra (limitações conhecidas do YASB).
- Balões/notificações (`NIF_INFO`) — no Win11 viram notificações do sistema.
