# 14 — Plano depois da v1.0: o que o teste real pediu

> Origem: o primeiro teste do dono no PC real (v1.0.0, 2026-10-05) e as respostas sobre Wallpaper Engine, menu Iniciar, *tiling* e ícones. O dono aprovou: tudo executado em sequência, revisado em *loop*, testado na Windows Sandbox; o teste real e os dois monitores ficam com ele no fim de cada entrega.

## Regras que continuam valendo

- **Nada de injetar código** em processos do Windows (Explorer, Iniciar) nem de alterar arquivos do sistema.
- **Admin só no Modo avançado.** É opcional, pedido pelo UAC na hora e explicado na tela. O dono aceitou pedir admin quando fizer sentido.
- **Tudo reversível** e tudo testado na Sandbox antes do teste real.
- **Conteúdo de terceiros** (Wallpaper Engine, anime) é **só para uso pessoal**: nunca entra no repositório nem nos pacotes oficiais, e um rice com ele não pode ser compartilhado.
- **Configurável e opcional:** atalhos, *tiling*, Iniciar próprio, animações. Nada liga sozinho sem o usuário escolher, exceto o que ele pediu como padrão (ícone na bandeja).

## O que o teste achou (prioridade 1)

| # | Problema | Causa encontrada | Correção |
|---|---|---|---|
| A1 | Hover quadrado numa barra arredondada (Tokyo Midnight) | Os destaques dos widgets têm raio fixo de 6 px e a barra não recorta os filhos | O raio dos destaques segue o da barra (`bar-item-radius`), a barra recorta (`clip`), e os cantos e o contorno são desenhados pela própria barra |
| A2 | Ícones da barra em baixa resolução | Os ícones das janelas e da bandeja chegam como HICON de 16/32 px e são esticados | Ícone do app pelo executável no tamanho físico (`IShellItemImageFactory`), reduzido com filtro de qualidade; bandeja no maior tamanho que o app oferece |
| A3 | Texto da barra sem nitidez, "Vol 100%" em texto | Texto pequeno no renderizador por software; widgets sem ícones | Ícones da fonte **Segoe Fluent Icons** (a do Windows 11) em volume, rede, bateria, Iniciar e bandeja; tamanhos e pesos de fonte revistos; teste de nitidez a 100/125/150% |
| A4 | A janela de configurações fica transparente depois de sair da tela ou ao mexer, até a tela mudar | A pintura parcial do renderizador não repinta a área que estava fora da tela | Repintura completa ao mover, ao voltar a ficar visível e ao mudar de monitor; teste que move a janela para fora da tela e confere os pixels |
| A5 | Ícone "em segundo plano" | O core só fica na bandeja quando algo precisa dele | Ícone sempre na bandeja (padrão; pode desligar); menu da bandeja com "Iniciar com o Windows" marcável; o instalador já liga o core; o ícone é promovido para fora do "^" uma vez, a pedido |
| A6 | Interface difícil de usar | Páginas com muitos controles, sem prévia, editor separado | Redesenho: Rices como galeria com prévia grande e aplicar com um clique; editor com prévia ao vivo; seções recolhíveis; textos mais curtos; atalhos de teclado; mesma linguagem visual da barra |

## As entregas

Cada entrega termina com testes de unidade, as suítes da Sandbox (as antigas e as novas), clang-tidy, ASan, fuzzing dos parsers novos, docs e um instalador novo para o teste real.

### M9 — Polimento (v1.1)

- **Correções:** A1–A5.
- **Barra redesenhada:**
  - ícones Fluent em todos os widgets embutidos;
  - destaque e pressionado arredondados;
  - animações curtas (hover, aparecer e esconder, ícone ativo, *popups*), que respeitam "Efeitos de animação" do Windows;
  - contorno opcional;
  - ícones dos apps nítidos.
- **Bandeja do core:** sempre visível, com "Abrir", "Iniciar com o Windows", "Pausar wallpaper", "Restaurar o Windows original" e "Sair".
- **Aceite:**
  - o hover respeita o raio em todos os rices oficiais;
  - ícones e texto nítidos a 100%, 125% e 150% (comparação de pixels na Sandbox);
  - a janela nunca fica transparente;
  - o ícone fica na bandeja;
  - o liga/desliga de iniciar com o Windows funciona.

### M10 — Barras e animações (v1.2)

- **Estilos prontos:**
  - **Windows** (embaixo, centralizada);
  - **Dock** (flutuante, centralizada, apps fixados e abertos, ampliação ao passar o mouse);
  - **Ilha** (pílula flutuante no topo);
  - **Status** (barra fina no topo estilo Waybar/Polybar);
  - **Lateral** (vertical).
  - Cada um é um modelo que o rice escolhe e ajusta.
- **Widgets novos:**
  - dock de apps (fixados e abertos);
  - workspaces (do M13);
  - Iniciar (do M11);
  - separador e espaçador;
  - texto ou ícone personalizado.
- **Várias barras por monitor:** o editor visual cria, duplica e organiza.
- **Animações:** entrada da barra, auto-ocultar deslizando, troca de rice com transição, indicador de janela ativa que desliza.
- **Aceite:** os 5 estilos aplicados na Sandbox sem sobreposição; memória da barra < 25 MB com animações; 0% de CPU parada.

### M11 — Iniciar próprio (v1.3)

- **Uma janela nossa**, leve, no processo da barra:
  - todos os apps (lista e categorias), busca instantânea, fixados, recentes;
  - pasta do usuário, Configurações, energia (bloquear, suspender, desligar, reiniciar, sair).
- **Visual do rice:** raio, acrylic, cores, ícones e animação de abrir.
- **Como abre:**
  - pelo botão Iniciar da nossa barra;
  - opcionalmente pela tecla **Win**, com um gancho de teclado no nosso processo, como o PowerToys: Win sozinho abre o nosso Iniciar e Win+qualquer tecla continua do Windows;
  - opcionalmente, o botão Iniciar nativo some junto com a barra nativa.
- **Ajustes do menu nativo** que o Windows permite (categorias, esconder Recomendados), aplicados pelo rice quando o usuário prefere o nativo.
- **Aceite:** abre em < 150 ms; busca em < 30 ms com 300 apps; a tecla Win volta ao normal quando a opção é desligada ou o processo cai.

### M12 — Wallpapers vivos e Wallpaper Engine (v1.4)

- **Efeitos no wallpaper** (imagem, vídeo e apresentação):
  - brilho, contraste, saturação e matiz (no processador de vídeo do D3D11, sem custo extra);
  - velocidade do vídeo;
  - desfoque;
  - *parallax* com o mouse;
  - zoom lento (Ken Burns) em imagens;
  - camadas de partículas desenhadas por nós: brilhos, estrelas, neve, chuva, pétalas, vaga-lumes, poeira mágica, com cor, densidade, tamanho, velocidade e direção;
  - vinheta e tingimento.
  - Tudo configurável por wallpaper, com prévia na janela.
- **Ler o Wallpaper Engine do usuário** (só leitura, uso pessoal):
  - **Onde:** acha `steamapps\workshop\content\431960` em todas as bibliotecas do Steam (`libraryfolders.vdf`) e aceita também uma pasta que o usuário escolher ou arrastar.
  - **Lista:** galeria com a prévia (`preview.jpg/gif`), o título e o tipo (`project.json`).
  - **Vídeo:** toca no nosso `rmw-wallpaper` com as propriedades do `project.json` (cores, velocidade, alinhamento) traduzidas para os nossos efeitos.
  - **Scene:** importa a prévia (imagem ou GIF) como base e aplica os nossos efeitos; o renderizador completo de *scene* fica para depois (um port do linux-wallpaperengine, GPL-3.0).
  - **Web:** fica para depois, opcional (WebView2).
  - **Application:** nunca.
  - **Compartilhar:** um rice com wallpaper do Wallpaper Engine é marcado "de terceiros" e o "Compartilhar…" fica bloqueado.
- **Aceite:**
  - efeitos a 30 fps com < 5% de CPU numa GPU integrada, medidos na Sandbox (WARP) e no PC real;
  - importar um vídeo de uma pasta de teste com `project.json` igual ao do Wallpaper Engine;
  - bloqueio do compartilhamento.

### M13 — Janelas: *tiling*, áreas de trabalho e atalhos (v1.5)

- **`rmw-tiler`** (worker supervisionado, opcional, desligado até o usuário ligar):
  - layouts dwindle, master-stack, colunas, monocle e flutuante;
  - *gaps*;
  - borda colorida na janela em foco (`DWMWA_BORDER_COLOR`);
  - regras por app (flutuar, workspace, ignorar);
  - restaura todas as janelas se cair (um vigia, como o GlazeWM).
- **Áreas de trabalho:**
  - **Linux:** por monitor, 1–9, com widget na barra.
  - **Mac:** usa as áreas de trabalho do Windows, com a animação nativa de deslizar.
  - Mover a janela para outra área de trabalho.
- **Atalhos (padrão com Ctrl, como o dono pediu, todos editáveis):**

  | Ação | Atalho |
  |---|---|
  | Foco | Ctrl+Alt+setas |
  | Mover a janela | Ctrl+Alt+Shift+setas |
  | Área de trabalho anterior/próxima | Ctrl+Win+←/→ |
  | Ir para a área de trabalho N | Ctrl+Win+1…9 |
  | Mandar a janela para a área de trabalho N | Ctrl+Win+Shift+1…9 |
  | Flutuar | Ctrl+Alt+F |
  | Tela cheia | Ctrl+Alt+M |
  | Trocar o layout | Ctrl+Alt+Espaço |

  - **Teclados ABNT2:** Ctrl+Alt+número não é usado, porque no ABNT2 é AltGr (¹ ² ³ £ ¢ ¬).
  - Atalhos com **Win** que o Windows já usa passam pelo gancho de teclado.
  - Win+L e Ctrl+Alt+Del não mudam.
- **Modo avançado (admin, pedido pelo UAC):** organiza também as janelas de apps rodando como administrador (o *tiler* roda elevado).
- **Aceite:**
  - 4 janelas organizadas e reorganizadas por atalho na Sandbox;
  - trocar de área de trabalho em < 100 ms;
  - desligar devolve todas as janelas;
  - cair devolve todas as janelas.

### M14 — Cara de outro sistema: ícones, sons, tela de bloqueio (v1.6)

- **Pacote de ícones do rice** (por usuário, sem admin):
  - ícones da área de trabalho (Este Computador, Lixeira, Rede, usuário);
  - pastas (`desktop.ini`);
  - atalhos do Iniciar, da área de trabalho e da barra (inclusive o do Explorer fixado);
  - tipos de arquivo (`HKCU\Software\Classes`);
  - ícones por app na nossa barra e no nosso Iniciar.
  - Tudo com desfazer, como os outros módulos.
- **Modo avançado:** ícone padrão de pasta e de unidade ("Shell Icons", HKLM), com aviso dos efeitos colaterais.
- **Esquema de sons** (por usuário) e **imagem da tela de bloqueio**.
- **Janelas de outros apps:** Mica/Acrylic e cor de borda via `DwmSetWindowAttribute` onde o app aceitar, com lista por app.
- **Aceite:** aplicar e desfazer na Sandbox sem resíduo (comparação do registro e dos arquivos).

### Rices pessoais do dono (fora do repositório)

- **Onde ficam:** `C:\dev\treino\rmw-personal-rices\` (Little Witch Academia e Hatsune Miku), nunca commitados e só para o dono.
- **Conteúdo nosso:**
  - paleta, barra, cursores próprios nas cores;
  - wallpaper com brilhos e partículas no estilo do anime (estrelas, poeira mágica, a "luz" das varinhas, notas musicais para a Miku), desenhados pelo gerador e pelos efeitos do M12.
- **Imagens oficiais:** baixadas só depois de o dono confirmar a lista (nome, origem, licença, tamanho). Para a Miku, as ilustrações oficiais da Crypton (CC BY-NC 3.0, com crédito).
- **Quando:** o primeiro rascunho sai junto com o M12 (precisa dos efeitos); antes disso, uma versão com vídeo e imagem.

## Ordem

M9 → rices pessoais (rascunho) → M12 → M10 → M11 → M13 → M14. O M9 vem primeiro porque é o que incomoda no uso diário; o M12, logo depois, porque os rices pessoais dependem dele.
