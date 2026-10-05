# ADR-0013 — M6: wallpaper em vídeo

- **Status:** aceito
- **Data:** 2026-10-05
- **Contexto:** doc 04 (M6), ADR-0006 (S1: vídeo fica na v1.0, meta de 90 MB vira tarefa do M6), ADR-0002 (mitigações), doc 09 §1 (encaixe atrás dos ícones)

## Decisões

1. **`IMFSourceReader` + D3D11 Video Processor no lugar do `IMFMediaEngine`** (a tarefa que o ADR-0006 deixou):
   - **Decodificação:** DXVA com o dispositivo D3D11 compartilhado (`MF_SOURCE_READER_D3D_MANAGER`), saída NV12, e o decodificador segura poucos quadros (`MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT = 2`).
   - **Um passo só na GPU:** o *video processor* converte NV12→BGRA e escala direto para o *back buffer* do swap chain (flip, 2 buffers). Não há textura intermediária.
   - **Só apresenta quando um quadro vence** (`Present(1, 0)`), espaçado pelo limite de FPS do rice (`FramePacer`). O laço volta ao início quando o arquivo acaba.
   - **Achado (Sandbox):** o atributo simples `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` recusa um gerenciador D3D (`E_INVALIDARG` ao abrir o arquivo). O avançado (`…_ADVANCED_VIDEO_PROCESSING`) aceita e converte na GPU.
   - **Sem DXVA:** quadros em memória chegam como RGB32 e sobem por uma textura. Cada quadro é examinado, e o jeito como chegou decide o caminho.

2. **DirectComposition como caminho principal.** Quando não dá para mirar a janela, ela ganha um swap chain próprio (`CreateSwapChainForHwnd`), que é o *fallback* do doc 04.

3. **Codec que falta é explicado.** MP4 com HEVC, AV1 ou VP9 sem a extensão instalada não toca. O erro diz qual extensão da Microsoft Store resolve, ou sugere salvar em H.264. O erro aparece no `rmw status` e no log, e a imagem de reserva do rice fica na tela.

4. **Um processo supervisionado, e o Windows sempre tem um wallpaper por baixo:**
   - **O motor aplica a imagem de reserva** (`fallback_image`) como wallpaper do Windows. O `rmw-wallpaper` só desenha por cima. Se ele cair, travar ou ficar sem GPU, o desktop nunca fica vazio. O parser avisa quando um vídeo vem sem imagem de reserva.
   - **O plano é um arquivo.** O core grava `<data>\wallpaper\video.json` com todas as regras de monitor do rice (as que não são vídeo também: um monitor que outra regra pegou não é do vídeo) e a política. Os vídeos são as cópias do Store, com nome pelo hash. Como a barra, o `rmw-wallpaper` resolve os monitores sozinho e acompanha o plano: arquivo regravado faz ele reconstruir, arquivo apagado faz ele sair.
   - **Supervisor:** Job com 768 MB, reinício com *backoff*, desligado após 3 falhas em 5 min. A trava é detectada com **1,5 s** de silêncio (doc 04: "< 2 s"; medido: 1,8 s até o core ver).
   - **Mitigações reduzidas** (`applyMediaMitigations`): drivers de GPU e decodificadores carregam DLLs que não são da Microsoft e podem gerar código. Por isso ficam de fora as mitigações estritas do core (ADR-0002: ACG e *MicrosoftSignedOnly*). Ficam: sem *extension points*, sem imagens remotas ou de baixa integridade, DLLs do sistema primeiro.

5. **A thread de janela não espera pela GPU.** Ela só cuida de janelas e mensagens e bate o coração a cada 250 ms. Ficam em outras threads:
   - **o dispositivo D3D11, o gerenciador do MF e o DirectComposition:** são criados numa thread auxiliar, que avisa por mensagem quando terminam;
   - **abrir o arquivo, ligar o decodificador e decodificar o primeiro quadro:** ficam na thread de cada renderer.

   A janela de cada monitor fica escondida até o primeiro quadro; até lá aparece a imagem de reserva. Um renderer pausado desde o início não mostra nada até tocar.
   - **Achado (Sandbox, intermitente):** com tudo isso na thread de janela, uns 4 s depois de um app em tela cheia abrir a thread passou dos 1,5 s e o supervisor reiniciou o worker. Os reinícios ficaram presos esperando a GPU (GPU-PV): um não criou o dispositivo em 10 s, outro levou 4,6 s, e o worker foi desligado. Não deu para saber em qual chamada a primeira trava aconteceu (daí o vigia abaixo), mas a GPU ocupada fica fora da thread de janela de qualquer jeito. Um jogo abrindo seria o mesmo caso. Depois da mudança, nenhum reinício espontâneo em duas rodadas da suíte.
   - **Vigia:** uma thread do próprio worker registra no log o passo em que a thread de janela está presa há mais de 1 s (ex.: `busy for 1029 ms: the test hook`). Uma trava que o core encerra chega com nome.

6. **Encaixe atrás dos ícones:**
   - **Layout novo (24H2+, "raised"):** janela filha *layered* do `Progman`, logo abaixo do `SHELLDLL_DefView`.
   - **Layout antigo:** atrás, no `WorkerW` que a mensagem `0x052C` cria.
   - **Conferência a cada segundo:** checa se ainda está lá. Janelas nossas entre a nossa e os ícones não contam como desencaixe.
   - **Reconstrução:** `TaskbarCreated` (o Explorer reiniciou) e `WM_DISPLAYCHANGE` reconstroem depois de 500 ms. GPU perdida (TDR, troca de driver) e volta da suspensão recriam o dispositivo.

7. **Pausas, em ordem de importância** (a primeira que vale é a mostrada):
   1. suspensão;
   2. tela desligada;
   3. sessão bloqueada;
   4. app em tela cheia (janela da frente que cobre o monitor e não é do shell nem nossa, ou `QUNS_RUNNING_D3D_FULL_SCREEN` / modo apresentação);
   5. bateria ou economia de energia;
   6. desktop coberto (uma janela maximizada visível, não *cloaked*, com o centro no monitor).

   **Como pausa:** a thread de renderização espera numa *condition variable* (0% de CPU) e o último quadro fica na tela. **Liberação:** pausado por 2 min, o decodificador, o swap chain e a janela daquele monitor são liberados, e a imagem de reserva aparece. Quando volta a tocar, tudo é montado de novo.

8. **Estado visível.** O `rmw-wallpaper` grava `<data>\wallpaper\status.json` (só quando muda). O core o junta ao `status`, e o `rmw status` mostra, por monitor:
   - tocando, pausado (e por quê), liberado ou com erro;
   - o vídeo (tamanho, fps, decodificação por hardware ou software);
   - a GPU.

9. **Gancho de teste.** O arquivo `<data>\wallpaper\test-hang` faz a thread de janela travar 10 s, uma vez: é como a suíte de sistema prova a detecção. Quem pode escrever na pasta de dados do usuário já pode fazer coisa pior, e o efeito é só o reinício do worker.

## Adiado

- **Áudio:** sempre mudo, não há caminho de áudio. O `mute = false` do rice é aceito e ignorado.
- **HDR, 4K/60 e o encaixe no layout antigo:** sem ambiente na Sandbox. 4K/60 e a sua GPU ficam para o teste real.
- **Dois monitores e DPI misto:** no teste real final, com o monitor secundário conectado.

## Números (Sandbox, build debug, RX 6600 por GPU-PV, vídeo 1080p30 H.264 numa tela 1520×823)

| Medida | Resultado |
|---|---|
| Suíte `video` | 25/25, duas rodadas seguidas |
| Tocando: memória (*Private Bytes*) | **45,3–46,6 MB** (S1 com o `IMFMediaEngine`, na Sandbox: 54 MB; no host: 106 MB — a meta de 90 MB no seu PC se confere no teste real) |
| Tocando: CPU | 0,30–0,49% (0,42% na segunda rodada) (o critério de "não seguir" é 5%; o doc 01 pede ≤ 3%) |
| Pausado (tela cheia na frente) | 0,00% de CPU |
| Liberado após 2 min pausado | 37,7 MB (o dispositivo D3D11 fica; decodificador, swap chain e janela saem) |
| `rmw-wallpaper` morto | de volta em 1,1 s (limite: 3 s) |
| Thread de janela travada | detectada em 1,8 s (limite: 2 s) |
| Explorer reiniciado | vídeo de volta atrás dos ícones, sozinho |
| `restore --original` | worker sai, wallpaper do Windows de antes |
