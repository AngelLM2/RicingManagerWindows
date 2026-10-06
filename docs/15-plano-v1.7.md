# 15 — Plano v1.7: o segundo teste real

> Origem: o teste do dono no PC real com a v1.6.0 (2026-10-06): a barra cobre os apps, a janela de configurações fica branca depois de trocar de área de trabalho, voltar ao Windows original não volta, o editor de widgets quebra, a troca de área de trabalho não é fluida, texto e ícones da barra serrilhados, e as funções da bandeja não funcionam. O dono pediu um plano detalhado, executado em *loop* até o fim. As regras do doc 14 continuam valendo: nada injetado em processos do Windows, tudo reversível, tudo testado na Windows Sandbox antes do teste real.

## O que o teste achou e a causa encontrada

As causas abaixo vêm dos logs e do `state.db` do PC do dono (lidos sem alterar nada) e do código.

| # | Problema | Causa encontrada |
|---|---|---|
| B1 | "Voltar ao Windows original" falha e nada volta (pela bandeja e pelas configurações) | O "Windows original" foi guardado logo depois de uma reinstalação (2026-10-06 00:30). Nesse momento o papel de parede do Windows apontava para uma cópia no armazém da instalação anterior, que já tinha sido apagada. O `wallpaper` falha com "arquivo não encontrado" e o motor desfaz **tudo** (`restore failed … undoing`). O mesmo original tem como tema um tema nosso (`RicingManager-2.theme`), que não existe mais. |
| B2 | A barra cobre os apps maximizados | A barra de cima pede o espaço (`ABM_SETPOS`), mas o Windows deixa a faixa na área de trabalho (`Windows still counts the strip 0,0 1920x44…`, várias vezes no log); a barra só registra isso no log. O dock (Miku 39) é `float` por padrão: as janelas passam por baixo dele de propósito. |
| B3 | A janela de configurações fica branca ao voltar de outra área de trabalho, até trocar de página | O renderizador por software só repinta o que mudou. A correção da v1.1 repinta ao mover, minimizar e restaurar, mas nenhuma mensagem avisa quando o Windows volta a mostrar uma janela escondida (*cloak*) por outra área de trabalho. |
| B4 | Editor de widgets quebrado: itens por cima dos botões e para fora do cartão; a janela não se adapta | Cada zona tem `min-height: 96px`, e um `min-height` explícito **substitui** o mínimo do conteúdo; sem `clip`, os itens vazam. Várias linhas de largura fixa (botões, caixas de 200–260 px) não quebram linha; a barra lateral não rola em telas baixas. |
| B5 | Texto e ícones da barra serrilhados | O renderizador por software do Slint amostra imagens pelo vizinho mais próximo (ignora `image-rendering: smooth`), e os ícones chegam em 48/64/256 px para serem mostrados em 16–32 px, sem redução prévia. Ícones da bandeja de 16 px são ampliados a 125/150%. Nas barras acrylic com raio > 8 px, o desfoque do DWM é retangular atrás dos cantos arredondados. |
| B6 | Troca de área de trabalho (modo Linux) com um "tranco" | As janelas novas aparecem antes de as antigas sumirem e só depois vão para o lugar (`layout()` depois de `SW_SHOWNA`, com duas correções a 200 ms); tudo assíncrono, janela por janela. |
| B7 | Funções da bandeja não funcionam | O item usado foi "Restaurar o Windows original" (B1). As saídas do core no log são "Sair" pela bandeja, não quedas. O aviso de falha não dizia o que fazer. |

**Lição de diagnóstico:** o app do Claude é empacotado (MSIX), e o que os testes locais antigos gravaram em `%LOCALAPPDATA%\RicingManagerWindows` ficou numa cópia virtualizada que escondia os arquivos reais do dono. A cópia foi movida para fora; nunca rodar `rmw-core` sem `--data-dir` aqui.

## As correções

### R1 — Restaurar sempre volta o que dá (B1, B7) — prioridade 1

1. **Restaurar não é tudo-ou-nada.** Ao restaurar (original ou histórico), um módulo que falha fica como está e entra no relatório ("voltou tudo, menos: papel de parede — arquivo não encontrado"); os outros continuam. Aplicar um rice continua tudo-ou-nada (um rice pela metade é pior que nenhum).
2. **Um arquivo que sumiu não impede a volta:**
   - papel de parede: o caminho original se existir, senão a cópia no armazém, senão a cópia do Windows (`TranscodedWallpaper`, guardada ao capturar), senão só a cor de fundo;
   - o mesmo para tela de bloqueio, cursores, sons e ícones (cursores voltam ao esquema padrão do Windows se nada existir).
3. **O papel de parede guarda o caminho do usuário e a cópia ao lado** (`file` e `copy`, como cursores e sons desde a v1.6): o Windows volta a apontar para o arquivo do usuário, nunca para o nosso armazém, então apagar os dados ao desinstalar não quebra nada.
4. **Capturar o que já está quebrado:** se o papel de parede atual aponta para um arquivo que não existe, a captura guarda a cópia do Windows (`TranscodedWallpaper`), que é a imagem na tela.
5. **Um original que é nosso:** se o tema guardado como original é um tema nosso (sobra de uma instalação anterior) ou sumiu, a volta usa o tema padrão do Windows do modo guardado (`aero.theme` claro, `dark.theme` escuro) e depois o modo e o destaque guardados.
6. **A bandeja diz o que aconteceu:** "Windows original de volta" ou "Voltou tudo, menos …", com o motivo.
7. **Sair é sair** (achado nos logs: o `state.db` sobreviveu à desinstalação que apagou o resto, porque a janela de configurações aberta reiniciou o core logo depois do `rmw shutdown`): o core fecha a janela ao sair pela bandeja ou por `rmw shutdown`, e a janela não o reinicia; `rmw shutdown --wait` para um core que voltou e, em último caso, encerra o que sobrou da instalação e devolve a barra do Windows.
8. **Desinstalar apagando os dados** só depois de tudo parado; se algo ficou, o desinstalador diz qual pasta apagar.
- **Aceite (Sandbox):**
  - aplicar em sequência os rices oficiais, os pessoais e "Meu visual" (exportado), sem desfazer entre eles, e restaurar o original: o registro fica igual ao de antes do primeiro uso;
  - o mesmo com o arquivo do papel de parede original apagado e com a cópia no armazém apagada: restaurar termina, o resto volta, o relatório diz o que faltou;
  - "Restaurar o Windows original" pela bandeja, clicando pela nossa barra;
  - depois de desinstalar apagando os dados, nenhum valor do registro do usuário aponta para `RicingManagerWindows\store`;
  - com a janela de configurações aberta, `rmw shutdown` fecha a janela e nenhum core volta.

### R2 — A barra não cobre os apps (B2)

1. **Quando o Windows ignora o pedido de espaço**, a barra ajusta a área de trabalho daquele monitor ela mesma (`SPI_SETWORKAREA`: o monitor menos as faixas de todas as nossas barras nele), confere de novo depois de cada mudança e devolve a área do Windows ao sair. Se o Explorer desfizer, a barra refaz, com limite para não brigar em *loop*.
2. **Janelas já maximizadas** naquele monitor são reencaixadas na área nova.
3. **Dock, ilha e as barras de baixo reservam espaço por padrão.** `float` (as janelas passam por baixo) continua como opção, explicada na tela. Os rices oficiais e os pessoais passam a reservar.
4. **O que a bandeja repassa ao Explorer** e não chega (tempo esgotado) vai para o log.
- **Aceite (Sandbox):** com cada estilo (Windows, Dock, Ilha, Status, Lateral) e com barra em cima e dock embaixo juntos, um Bloco de Notas maximizado não fica por baixo de nenhuma barra; ao sair da barra a área de trabalho volta a ser a do Windows; o Explorer reiniciado não deixa sobra.

### R3 — A janela nunca fica branca (B3)

1. Repintar tudo quando o Windows volta a mostrar a janela: evento de *uncloak* da própria janela (`EVENT_OBJECT_UNCLOAKED`, dentro do processo), `WM_SHOWWINDOW`, ativação, e `WM_PAINT` com área inválida de verdade.
- **Aceite (Sandbox):** trocar de área de trabalho do Windows (Ctrl+Win+D, Ctrl+Win+←) e de área do *tiling* (modo Linux e Mac) com a janela aberta numa página parada: a captura depois de voltar é igual à de antes.

### R4 — Configurações que se adaptam (B4)

1. **Editor de widgets:** as zonas crescem com os itens (`min-height: max(96px, conteúdo)`) e recortam; em telas estreitas as três zonas ficam uma embaixo da outra; os botões quebram linha.
2. **Linhas de largura fixa** (atalhos, regras, *tiling*, ícones, sons, wallpapers, barra) passam a esticar e a quebrar linha abaixo de uma largura.
3. **A barra lateral rola** quando a janela é baixa.
- **Aceite (Sandbox):** todas as páginas no tamanho mínimo (820×560), no padrão e em 200%: nenhum controle sobrepõe outro (caixas da árvore de acessibilidade) e não há rolagem horizontal; editor de widgets com 8 itens numa zona.

### R5 — Barra nítida (B5)

1. **Ícones reduzidos antes de chegar ao Slint**, no tamanho físico exato em que aparecem (área média, de qualidade), e de novo quando a escala muda. A bandeja pede o maior ícone que o app oferece. O dock guarda alguns tamanhos para a ampliação ao passar o mouse.
2. **Superamostragem da barra** em 100–150%: a barra é desenhada em 2× e reduzida com filtro, o que suaviza bordas, cantos arredondados e texto. Desligada a partir de 200%, onde não ajuda. Orçamento: a barra continua < 25 MB e 0% de CPU parada.
3. **Cantos das barras acrylic:** o desfoque segue o formato da barra (região com o raio do rice), e a borda é coberta pela borda suavizada da barra.
- **Aceite (Sandbox):** a 100%, 125% e 150%, a borda dos ícones e do texto tem tons intermediários (não só 0 e 255) e os ícones reduzidos ficam próximos da referência do Windows (diferença média pequena); memória e CPU dentro do orçamento.

### R6 — Troca de área fluida (B6)

1. **Primeiro o lugar, depois a aparição:** as janelas da área nova vão para o lugar ainda escondidas; depois todas aparecem e as antigas somem de uma vez (um lote só), já na ordem certa, e o foco vai direto.
2. **Sem correções visíveis:** a acomodação a 200 ms só mexe numa janela que não está onde devia.
- **Aceite (Sandbox):** em 20 trocas com 3 janelas por área, nenhuma captura mostra janelas das duas áreas juntas nem uma janela fora do lugar; tempo de troca medido e registrado.

### R7 — Bandeja (B7)

1. Todos os itens do menu da nossa bandeja testados pela nossa barra (clique direito, escolher o item): abrir, alternar claro/escuro, restaurar, barra liga/desliga, recarregar, iniciar com o Windows, sair.
2. O menu de outro app (um app de teste com ícone e menu) abre e o comando escolhido chega ao app.

## Ordem

R1 → R2 → R3 → R4 → R5 → R6 → R7. Cada um com testes de unidade, a suíte da Sandbox que o cobre, clang-tidy e o registro em `PROGRESSO.md`; no fim, todas as suítes, o instalador e a v1.7.0 local. O teste real e os dois monitores continuam com o dono.

## Estado (2026-10-06)

Feito, com testes de unidade e as suítes da Sandbox (ADR-0023, ADR-0024):

| | Resultado medido |
|---|---|
| R1 | 7 rices em sequência + "Meu visual" e restaurar: registro igual ao de antes; original com imagem e cópia apagadas: o resto volta e a imagem do Windows entra; `rmw shutdown` com a janela aberta: a janela fecha e nenhum core volta; as imagens recentes do Configurações voltam |
| R2 | janela maximizada fora de todas as barras nos 5 estilos e com barra em cima + dock; o caso "o Windows deixou a faixa" aconteceu na Sandbox e a barra a separou |
| R3 | a janela branca depois de três idas e voltas entre áreas foi reproduzida (diferença 16) e corrigida (0,0) |
| R4 | todas as páginas no tamanho mínimo sem nada para fora da janela; zonas do editor que crescem |
| R5 | barra 2× superamostrada: 18 MB, 0,6% de CPU parada |
| R6 | 5 trocas, uma com arranjo novo: nenhuma janela muda de lugar depois de aparecer |
| R7 | o menu da nossa bandeja restaura o Windows original |

Fica em aberto: no modo Mac (áreas do próprio Windows), em uma de cada duas ou três rodadas da Sandbox a primeira volta para a área 1 não acontece (o Windows ignora o atalho que o *tiler* manda logo depois de uma área nova ser criada). O modo Linux, o do dono, não tem isso. Também resolvido no caminho: uma janela que aparecia por 18 ms fazia as outras se mexerem nas trocas; agora uma janela só entra no *tiling* se continuar lá 120 ms.

## O que o dono faz no PC dele depois da v1.7

- Instalar a v1.7 por cima e usar "Restaurar o Windows original": com R1, volta o que dá e diz o que faltou.
- O "original" guardado dele já tinha um tema nosso; R1.5 troca pelo tema padrão do Windows. Se quiser outro tema, escolher em Personalização > Temas.
