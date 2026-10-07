# ADR-0025 — O terceiro teste real e a campanha de quebra (v1.8)

- **Data:** 2026-10-06
- **Estado:** aceita
- **Contexto:** o teste do dono com a v1.7.0 (doc 16) e o pedido de "tentar quebrar o software inteiro".

## Decisões

1. **Os apps que a barra abre saem do *job* dela** (`JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK`, só no *job* da barra). No *job*, um navegador aberto pelo Iniciar ou pelo dock herdava o teto de memória de 96 MB por processo, contava no limite do *job* inteiro e morria quando o supervisor encerrava a barra. Os outros *workers* continuam sem poder criar processos fora do *job*: só a barra abre apps da pessoa.
2. **Um app é aberto numa *thread* própria** (`launchAppInBackground`). `ShellExecuteEx` de um app que pede administrador só volta quando a pessoa responde ao UAC, e o *thread* da barra parado passa do *heartbeat*.
3. **Ícones no tamanho em que aparecem.** `appIconPixels` reduz (média da área, pré-multiplicada) o que o Shell devolve com `SIIGBF_BIGGERSIZEOK`, que pode ser 256 px. O Iniciar carrega a lista e os ícones numa *thread* própria e recebe em lotes; "Todos os apps" é uma `ListView` (só as linhas à vista existem).
4. **O *tiler* aprende o tamanho mínimo de cada janela** em vez de perguntar (`WM_GETMINMAXINFO` entre processos não é seguro: o ponteiro seria do nosso processo). Uma janela no lugar certo mas maior que o espaço recusou encolher; o mínimo é guardado e as divisões do *layout* o respeitam quando cabe. Quando não cabe, a janela fica inteira dentro da área do monitor (`tiling::fitted`). Limite conhecido: com mais mínimos do que a tela comporta, janelas podem ficar umas sobre as outras (nunca fora da tela).
5. **Animações do *tiler* só de posição**, numa *thread* própria no ritmo da tela (o período de composição do DWM, num *timer* de alta resolução), com `SWP_ASYNCWINDOWPOS`. O tamanho muda no máximo duas vezes (o menor dos dois no caminho): apps baseados em Chromium piscam a cada redimensionamento e nenhum app é obrigado a redesenhar a cada quadro. Nada é injetado nem tem a transparência mudada (`WS_EX_LAYERED` em janela alheia quebra apps com *flip model*). Desligadas quando as animações do Windows estão desligadas, e em *Tiling → Animações*.
6. **Barras fluidas sem pedir espaço de novo.** A faixa reservada não depende do comprimento da barra: uma barra `auto` muda de comprimento quadro a quadro só com `UpdateLayeredWindow` (o DIB só cresce). Os itens que entram crescem (`init` + `animate`), e as listas da barra (janelas, dock, bandeja) mudam linha a linha (`syncRows`), para que um título que muda não refaça os botões.
7. **Repetidores de *widgets* de plugin:** só um número escrito (≤ 256) ou uma propriedade de lista; ≤ 4096 elementos multiplicando os aninhados.
8. **A campanha de quebra** virou uma suíte da Sandbox (`break`) e se repete a cada versão: caminhos com apóstrofo e acento (o perfil do dono é `C:\Users\user'`), configurações e banco danificados, rajadas de operações, CLI com lixo, imagens enormes, barras absurdas, mudança de fuso durante uma agenda.
9. **A hora local é lida com o fuso de agora.** O C *runtime* guarda o fuso que leu na primeira vez (`localtime_s`), e um core residente passaria dias com o fuso velho; `_tzset` vem antes de cada leitura (agenda, relógio dos plugins, datas da janela). O serviço de fuso automático do Windows não avisa nenhum app: a agenda confere a hora a cada minuto, além de `WM_TIMECHANGE`.
10. **Uma imagem de um rice é uma imagem pelos bytes** (`sniffFileType`: JPEG, PNG, BMP, GIF), não só pela extensão, ao entrar no armazém. O `.rice` importado já era conferido assim; uma pasta aplicada direto não era.
11. **O teto de memória da barra é 128 MB.** Com o Iniciar próprio aberto em "Todos os apps", a barra medida na Sandbox (438 apps) fica perto de 55 MB; os apps que ela abre não contam mais (decisão 1).

## Consequências

- Um app aberto pela barra não é mais encerrado com ela, mas também não é mais limitado por ela (é o comportamento esperado de um *launcher*).
- Mover janelas a 60–144 quadros por segundo custa pouco ao DWM e quase nada aos apps (só posição), mas um app travado recebe os pedidos acumulados quando volta: termina no lugar certo.
- O *tiler* precisa de uma rodada (≈ 200 ms) para aprender o mínimo de uma janela nova: ela aparece um instante maior que o espaço antes de as outras abrirem lugar.
