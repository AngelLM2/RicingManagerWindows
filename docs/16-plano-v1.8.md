# 16 — Plano v1.8: o terceiro teste real e a campanha de quebra

> Origem: o teste do dono no PC real com a v1.7.0 (2026-10-06): janelas sobrepostas ao abrir, nenhuma animação ao voltar para uma área com janelas, barras que crescem aos saltos quando a bandeja ("segundo plano") se abre, e o Iniciar próprio que "crasha" em **Todos os apps**. O dono pediu, além disso, uma postura crítica: **tentar quebrar o software inteiro, testando na prática todas as funcionalidades, subfuncionalidades e possibilidades, e analisando o resultado.** As regras dos docs 14 e 15 continuam valendo: nada injetado em processos do Windows, tudo reversível, tudo testado na Windows Sandbox antes do teste real.

## O que o teste achou e a causa encontrada

As causas vêm dos logs do PC do dono (lidos sem alterar nada) e do código.

| # | Problema | Causa encontrada |
|---|---|---|
| C1 | O Iniciar "crasha" em **Todos os apps** | `core.log`: `worker bar passed its memory limit` duas vezes em 4 s. O teto da barra é 96 MB (aviso a 80%). O Iniciar carrega o ícone de cada um dos 239 apps com `SIIGBF_BIGGERSIZEOK`, que devolve até 256×256 (256 KB por app, ~60 MB só nos ícones guardados, mais o que o Shell guarda), e a lista instancia as 239 linhas de uma vez. |
| C2 | Janelas sobrepostas ao abrir | Com 5 janelas, o *dwindle* dá ~465×245 às duas últimas; Discord e a nossa janela de configurações têm tamanho mínimo maior e passam do espaço delas (o Discord sai pela borda de baixo). O *layout* não conhece o tamanho mínimo dos apps. As janelas novas também ficam 120 ms no lugar delas antes de entrar no *tiling* (v1.7). |
| C3 | Nenhuma animação ao voltar para uma área com janelas | Esconder (`SW_HIDE`) tem o *fade* do próprio Windows; mostrar no lugar (`SetWindowPos` com `SWP_SHOWWINDOW`, v1.7) não tem nada. |
| C4 | Barras crescem aos saltos ("abertura de coisa em segundo plano") | A seta da bandeja troca a lista de ícones de uma vez; uma barra `auto` (dock, ilha) muda de comprimento num quadro só. Nenhum item que entra ou sai é animado. |
| C5 | (achado nos logs) 6 pedidos de administrador seguidos às 17:38 | Com o modo avançado ligado e o *tiling* desligado, `wantsAdmin()` olha só `runAsAdmin`; a cópia elevada vê `enabled = false` e sai na hora (código 3); a comum pede de novo. Laço de UAC até o dono dizer não. |

## As correções

### R1 — O Iniciar aguenta qualquer número de apps (C1) — prioridade 1

1. **Ícones no tamanho em que aparecem:** `appIconPixels` reduz (área média) o que o Shell devolve ao tamanho pedido; nada maior que o pedido fica guardado.
2. **Lista virtual:** "Todos os apps" usa uma lista que só cria as linhas visíveis; os ícones são carregados para as linhas que aparecem, aos poucos.
3. **Teto medido:** o Iniciar com 400 apps (atalhos de teste) aberto, em "Todos os apps", rolado até o fim e buscando: a barra fica abaixo do orçamento.
- **Aceite (Sandbox):** 400 atalhos no menu Iniciar; abrir, "Todos os apps", rolar até o fim, voltar, buscar; nenhuma reinicialização da barra no `core.log`; memória da barra medida antes e depois.

### R2 — Janelas que cabem (C2, C5)

1. **Tamanho mínimo aprendido:** depois de pôr uma janela no lugar, se ela ficou maior que o espaço (o app recusou encolher), o *tiler* guarda esse mínimo e refaz o *layout* respeitando-o: as divisões do *dwindle*, da pilha e das colunas dão a cada janela pelo menos o mínimo dela quando cabe.
2. **Quando não cabe:** a janela fica inteira dentro da área do monitor (nunca para fora da tela, nunca por baixo das barras).
3. **Janelas novas** entram já no lugar delas, sem a espera de 120 ms para as que são claramente janelas de app (com botão na barra de tarefas); a espera fica para as que aparecem sem isso.
4. **C5:** o modo avançado só pede administrador quando o *tiling* está ligado; uma cópia elevada que sai por "não precisa" não é pedida de novo até o plano mudar.
- **Aceite (Sandbox):** 5 janelas, duas com mínimo de 900×500 (app de teste): nenhuma janela para fora da área nem por cima de outra quando cabe; `tiler.log` com um só pedido de administrador ao desligar o *tiling* com o modo avançado ligado.

### R3 — Áreas de trabalho com animação (C3)

1. **Ao trocar de área (modo Linux),** as janelas da área nova entram deslizando do lado para onde se vai (só posição, sem mudar o tamanho, ~200 ms, desaceleração); as da área antiga somem como hoje.
2. **Ao abrir, fechar ou trocar janelas de lugar,** as outras deslizam até o lugar novo.
3. **Configurável:** *Tiling* → Animações (ligadas por padrão), e desligadas sozinhas quando o Windows está com as animações desligadas (Acessibilidade → Efeitos de animação).
- **Aceite (Sandbox):** posições intermediárias medidas durante a troca (a janela passa por pontos entre o início e o fim) e a posição final exata; nenhuma janela parada fora do lugar depois da animação.

### R4 — Barras fluidas (C4)

1. **A bandeja abre e fecha deslizando:** os ícones escondidos entram e saem com largura e opacidade animadas.
2. **Itens que entram e saem** (bandeja, janelas, dock) crescem e encolhem em vez de aparecer de uma vez.
3. **Uma barra `auto` (dock, ilha) muda de comprimento animada,** sem pedir espaço de novo ao Windows a cada quadro.
- **Aceite (Sandbox):** comprimentos intermediários medidos durante a abertura da bandeja e a chegada de um app no dock; CPU parada continua ~0%.

## A campanha de quebra

O dono pediu para tentar quebrar tudo. O método:

1. **Revisão adversarial do código** por área (barra, *tiler*, core e motor, configurações e plugins): cada achado é conferido no código, reproduzido na Sandbox quando dá, corrigido com teste.
2. **Uma suíte nova da Sandbox, `break`,** que faz o que um usuário impaciente ou azarado faria, e confere o resultado (log, registro, capturas de tela, memória):
   - **Ambiente do dono:** dados e instalação numa pasta com apóstrofo e acento (o perfil do dono é `C:\Users\user'`).
   - **Rices:** aplicar um atrás do outro sem esperar; aplicar durante uma restauração; apagar o rice aplicado; renomear e duplicar com nomes que o Windows recusa; importar `.rice` vazios, truncados, enormes; restaurar com o armazém apagado.
   - **Arquivos do app:** `settings.toml` vazio, lixo, enorme, só leitura; `state.db` corrompido; pasta de dados apagada com tudo rodando.
   - **Processos:** matar cada *worker* e o core em momentos ruins (no meio de aplicar); Explorer reiniciado com barra, bandeja e *tiling* ligados.
   - **Barra:** todos os estilos × bordas × tamanhos extremos × `auto`; 60 ícones na bandeja; apps que abrem e fecham sem parar; Iniciar com 400 apps.
   - **Tiling:** apps com tamanho mínimo, que se escondem na bandeja ao fechar, que recriam a janela; trocar de área 50 vezes rápido; todas as teclas com e sem janelas; regras.
   - **Configurações:** todas as páginas, todos os controles, valores extremos, troca de idioma com a janela aberta, janela no tamanho mínimo e em 200%.
   - **CLI:** cada comando com argumentos errados, faltando, enormes.
3. **Cada achado** entra na tabela abaixo com a causa, a correção e o teste que o cobre.

### Achados

| # | Área | O que quebrou | Causa | Correção | Teste |
|---|---|---|---|---|---|
| Q1 | *Tiler* | Laço de pedidos de administrador | C5 | R2.4 | `tiler` |

## Ordem

R1 → R2 → R3 → R4, depois a campanha (achados corrigidos à medida que aparecem). Cada um com testes de unidade, a suíte da Sandbox que o cobre, clang-tidy e o registro em `PROGRESSO.md`; no fim, todas as suítes, o instalador e a v1.8.0 local. O teste real e os dois monitores continuam com o dono.
