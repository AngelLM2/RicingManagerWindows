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

Quatro revisões adversariais (barra, *tiler*, core e motor, configurações e plugins) acharam 87 problemas; cada um foi conferido no código antes de corrigir. Os principais:

| # | Área | O que quebrava | Correção | Teste |
|---|---|---|---|---|
| Q1 | *Tiler* | Laço de pedidos de administrador (C5) | `wantsAdmin` igual ao `applyPlan`; uma cópia que sai na hora não é pedida de novo | `tiler` §15 |
| Q2 | Barra | **Apps abertos pelo Iniciar e pelo dock entravam no *job* da barra**: herdavam o teto de 96 MB, contavam no limite dela e morriam com ela — provável causa real do "crash" em Todos os apps | `JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK` no *job* da barra; abrir apps numa *thread* própria (um app que pede administrador segurava a barra até ela ser morta) | `bar` §4d |
| Q3 | Barra | Com o core morto, a barra girava a 100% de CPU e a barra do Windows nunca voltava | o laço lê as mensagens mesmo quando um *handle* fica sinalizado; o core é observado uma vez | revisão do código |
| Q4 | Barra | Texto do rice com caracteres japoneses ou acentos cortado no meio de um caractere: o Slint aborta e a barra cai em todo início | `utf8Prefix` corta entre caracteres | `UtfTests` |
| Q5 | Barra | Clicar num número do widget de áreas do Windows não fazia nada (callback com outro nome) | `-select`, e um aviso no log se faltar | revisão do código |
| Q6 | Barra | Plugins podiam criar milhões de elementos (`for x in 200*200*200`, laços aninhados, um número vindo do script) | repetidor só com número escrito (≤ 256) ou lista; ≤ 4096 elementos somando os aninhados | `WidgetSourceTests` |
| Q7 | Barra | Com o Explorer lento, posicionar a barra esperava até 4,5 s e ela era morta como travada | chamadas de app bar esperam 500 ms | — |
| Q8 | Barra | Ícone animado recusado pelo Explorer acumulava um GDI *handle* por quadro por 2 min (visto no PC do dono) | uma atualização substitui a que espera; até 8 por ícone | — |
| Q9 | Barra | Iniciar: lista e ícones no *thread* da barra (recriar a barra ou digitar podia passar do *heartbeat*); 25 MB de *buffers* a 200% mesmo fechado | carregamento numa *thread* própria em lotes; *buffers* só com o menu aberto | `bar` §4d |
| Q10 | *Tiler* | Janela em tela cheia (F11, vídeo, jogo) puxada de volta para o *tile* | deixada em paz como uma maximizada | `tiler` §12 |
| Q11 | *Tiler* | Discord/Spotify fechados na bandeja reapareciam maximizados ao desligar o *tiling* | janela que o app escondeu fica escondida | `tiler` §13 |
| Q12 | *Tiler* | Redimensionar um *tile* trocava a janela com a vizinha | um arrasto que muda o tamanho é redimensionar | `tiler` §14 |
| Q13 | *Tiler* | Troca de área podia voltar para a anterior (o eco do foco da janela escondida) | 400 ms de calma depois da nossa troca | `tiler` §3 |
| Q14 | *Tiler* | Janelas de app de administrador escondidas para sempre depois de uma queda da cópia elevada | ficam no `windows.json` até um *tiler* que possa movê-las | revisão do código |
| Q15 | *Tiler* | Monitor que muda de nome (DisplayPort acordando) embaralhava as áreas; monitor desligado empilhava tudo na área visível | mesmo lugar = mesmo monitor; áreas juntam por número | `TilingTests` |
| Q16 | *Tiler* | Ao soltar Alt de um atalho Ctrl+Alt, o menu do app abria; segurar a tecla de Toggle ligava e desligava 30 vezes por segundo | tecla neutra depois de Alt; repetição só para foco, mover e próxima área | revisão do código |
| Q17 | *Tiler* | Janela volta para fora de todas as telas quando o monitor dela sumiu | volta para a área de trabalho do monitor mais próximo | revisão do código |
| Q18 | Ambos | Tooltips e listas suspensas voltavam como fantasmas; arquivo do plano perdido durante leitura; salvar a cada troca esperava o disco | ver o commit do *tiler* | — |

**Core e motor** (17 achados; 16 corrigidos, 1 em parte, 1 deixado):

| # | O que quebrava | Correção |
|---|---|---|
| Q19 | Sair pela bandeja ou desinstalar matava o *tiler* sem devolver as janelas escondidas nas outras áreas | o core pede ao *tiler* que saia e espera até 3 s; se não sair, `rmw-tiler --restore` devolve pela lista |
| Q20 | Um desfazer que sempre falha travava **toda** operação, inclusive restaurar | `recover()` faz o que dá e fecha o diário; restaurar segue; `rmw recover [--discard]` |
| Q21 | Restaurar falhava inteiro se um módulo não conseguisse ler o estado atual | o módulo entra em "não voltou" e o resto continua |
| Q22 | O banco era escrito por várias *threads* fora da transação (escritas perdidas no `ROLLBACK`, histórico ligado ao *snapshot* errado) | uma trava por chamada e por transação |
| Q23 | `state.db` corrompido impedia o core de iniciar para sempre; um `state.db` vazio perdia o "Windows original" | o corrompido vai para o lado (`state.db.damaged-…`); o original também fica em `original.json` |
| Q24 | Uma exceção num cliente encerrava o core; o laço de conexões podia girar a 100% | cada pedido e cada tarefa têm seu `catch`; a instância do *pipe* é refeita e espera |
| Q25 | Pasta observada apagada (ou unidade de rede perdida): o *thread* da janela girava a 100% | a observação é refeita ou largada; as mensagens são lidas a cada volta |
| Q26 | Restaurar interrompido era desfeito (o rice voltava); o plano da apresentação sobrevivia a um apply desfeito; um monitor desconectado ficava com o papel do rice | restaurar interrompido termina; plano pendente até o fim do apply; campo de monitor ausente continua gerenciado |
| Q27 | Voltar a um ponto do histórico desmontava barras, vídeo e agenda; voltar de "Windows original" não fazia nada | o *snapshot* guarda o que a operação mudou e o estado do core |
| Q28 | Mudança de fuso ou horário de verão perdia a troca claro/escuro | a agenda compara o período local antes e depois do salto |
| Q29 | Valores do registro de tipos inesperados (QWORD, MULTI_SZ) voltavam como binário ou eram apagados | o tipo é guardado (o módulo de ícones ainda converte as strings como antes) |
| Q30 | O ajudante elevado (ícones de pasta e unidade para o PC todo) podia ser enganado por uma *junction* trocada no meio; um usuário apagava os ícones de outro | pasta aberta sem seguir *links* e protegida pelo mesmo *handle*; diário e ícones por usuário |
| — | Deixado: o tema "legado" pode aplicar o papel de parede e os sons do `.theme` | só se confere aplicando um tema num Windows de verdade |

**Configurações e plugins** (24 achados; 20 corrigidos, 4 em parte):

| # | O que quebrava | Correção |
|---|---|---|
| Q31 | **Arquivo com nome longo (mais de ~56 letras) deixava o rice ilegível** (o corte comia a extensão) | o corte fica no nome, a extensão fica; um rice que não relê não é salvo |
| Q32 | Cursores importados perdiam os arquivos (`install.inf` apontava para nomes trocados) | os nomes originais ficam quando o Windows aceita |
| Q33 | Escolher um vídeo de 2 GB ou uma pasta de 500 imagens travava a janela ("Não está respondendo") | cópias no *thread* de trabalho, com aviso de "copiando" |
| Q34 | Fechar a janela durante uma tarefa longa deixava um processo invisível que impedia abrir de novo por até 11 min | o processo sai 3 s depois da janela |
| Q35 | Arrastar um controle e clicar em Aplicar aplicava o valor antigo (salvamento a 400 ms nunca era forçado) | o que espera é salvo antes de aplicar, compartilhar, trocar de rice e fechar |
| Q36 | Controles mostravam valores velhos depois de serem mexidos (ligações de mão única do Slint) | controles que retomam o valor a cada mudança |
| Q37 | **Rolar a página com a roda do mouse mudava espessura, margens, raio e FPS da barra** | caixa de número nova: a roda só age com o foco |
| Q38 | Um atalho podia travar a página de *tiling*; atalhos repetidos aceitos | conferidos antes de salvar |
| Q39 | Aprovar um plugin podia conceder permissões nunca mostradas | a aprovação leva as permissões vistas; o core recusa se o plugin pede mais |
| Q40 | Desinstalar um plugin passava as edições para outro (seleção por posição) | seleção por id |
| Q41 | Renomear rices não funcionava | ligado, com os nomes que o Windows recusa conferidos |
| Q42 | Cartões e linhas que vazavam em português; janela mais alta que uma tela 1080p a 200% | linhas que quebram; altura mínima 420 |
| — | Em parte: cópias longas não podem ser canceladas no meio; a lista do seletor de apps não recebe foco pelo teclado; mensagens que vêm do core seguem em inglês; **plugins não usam mais proxy** (o endereço de destino é conferido antes de enviar; numa rede que só sai por proxy, o recurso de rede dos plugins para, com uma mensagem que diz o motivo) | |

**A suíte `break` e as suítes de sempre na Sandbox** (o que só apareceu rodando):

| # | O que quebrava | Correção | Teste |
|---|---|---|---|
| Q43 | Com o Iniciar próprio aberto, a barra usa ~55 MB; o teto de 96 MB deixava pouca folga | teto de 128 MB (aviso a 80%) | `bar` §4d |
| Q44 | Esc no Iniciar e um clique logo depois no botão Iniciar não abria nada (a proteção de 300 ms valia para qualquer fechamento) | só um menu que perdeu o foco ignora o próximo clique | `bar` §4d |
| Q45 | Depois de a barra cair várias vezes e o supervisor desistir, `rmw bar on` não fazia nada até reiniciar o core | `bar on` com a barra parada a inicia de novo | `break` bar-killed |
| Q46 | Trocar o fuso horário (ou o serviço automático trocá-lo) não mudava a agenda claro/escuro: o C *runtime* guarda o fuso que leu primeiro, e uma atualização da agenda escondia o salto do relógio | `_tzset` antes de ler a hora local (agenda, relógio dos plugins, datas da janela); a agenda anota o relógio só quando olha para ele e confere a hora a cada minuto | `break` time-zone |
| Q47 | Um executável com nome `.png` era aceito como papel de parede | uma imagem é conferida pelos bytes (JPEG, PNG, BMP, GIF) | `break` huge-picture |
| Q48 | A caixa de número mudava de largura com o valor; um `settings.toml` que não carrega não deixava rastro | largura fixa; aviso no log | `settings` |
| Q49 | O gancho da tecla Win podia ser parado antes de saber o próprio *thread* (a barra esperava para sempre ao sair) | o *thread* é conhecido antes de qualquer um poder pará-lo | revisão do código |
| Q50 | **Modo Mac: Ctrl+Win+número tocado rápido às vezes não trocava de área** (a falha "intermitente" desde a v1.4): o *tiler* olhava se Ctrl e Win ainda estavam apertados e mandava só a seta, mas quando ela chegava a pessoa já os tinha soltado (o Windows via uma seta sozinha). O mesmo com a tecla neutra depois de um atalho com Win ou Alt: chegando depois do Win solto, o Iniciar abria | o gancho segura as teclas modificadoras soltas até o comando responder e o que ele mandou passar pelo gancho, e as solta depois, na ordem (no máximo 250 ms) | `tiler` §9 |

A suíte `break` (56 verificações, todas passando) faz: instalação e dados em pastas com apóstrofo e acento (como o perfil `C:\Users\user'` do dono); `settings.toml` vazio, lixo, JSON, com NUL, com 5000 níveis, de 6 MB, com tipos, atalhos e regras errados, só leitura e apagado; cinco aplicações e uma restauração de uma vez (as que chegam com outra em curso recebem "ocupado", o core é o mesmo no fim); apagar o rice aplicado e desfazer/restaurar; matar a barra até o supervisor desistir e pedir `bar on`; 39 chamadas da CLI com argumentos faltando, errados, de 32 000 letras, com emoji e japonês, arquivos aleatórios e vazios; imagem de 7000×7000 e um executável disfarçado de imagem; quatro barras absurdas (espessura 512 com margens de 1024, espessura 4 com raio 1000, comprimento 0, espessura 300 à direita); troca de fuso com a agenda ligada; `state.db` corrompido e vazio. No fim, a barra do Windows está de volta e nenhum `fatal` aparece nos logs.

## Ordem

R1 → R2 → R3 → R4, depois a campanha (achados corrigidos à medida que aparecem). Cada um com testes de unidade, a suíte da Sandbox que o cobre, clang-tidy e o registro em `PROGRESSO.md`; no fim, todas as suítes, o instalador e a v1.8.0 local. O teste real e os dois monitores continuam com o dono.
