# ADR-0017 — M12: wallpapers vivos e Wallpaper Engine

- **Status:** aceito
- **Data:** 2026-10-06
- **Contexto:** doc 14 §M12; os pedidos do dono depois da v1.0: "efeitos e config de wallpaper igual ao do Wallpaper Engine", só ler os wallpapers do Wallpaper Engine que a pessoa já tem, e rices pessoais com brilhos parecidos com o anime.

## Decisões

1. **Efeitos e partículas no formato do rice.** Uma regra `image`, `video` ou `color` de `[[wallpaper.monitor]]` ganha:
   - **`effects = { ... }`:**
     - `brightness`, `contrast`, `saturation`, `hue`: as cores;
     - `speed`: a velocidade de um vídeo;
     - `blur`;
     - `parallax`: a imagem se inclina para longe do mouse;
     - `zoom`: um zoom e um deslize lentos, de ida e volta em 40 s;
     - `vignette`.
   - **`particles = [ ... ]`:** até 8 camadas de `sparkles`, `stars`, `snow`, `rain`, `petals`, `fireflies`, `magic`, `notes`, `hearts` ou `bubbles`. Cada camada tem cor (também `palette.*`), quantidade, tamanho, velocidade, direção e brilho (`glow`).
   - **Valores omitidos:** o que o rice não diz fica como está e não é escrito de volta. Faixas fora do lugar são erro; um efeito que não se aplica ao tipo (velocidade numa imagem, desfoque numa cor, qualquer coisa num slideshow) é aviso e é ignorado.

2. **Quem desenha é o `rmw-wallpaper`, com Direct2D no mesmo dispositivo do vídeo.**
   - **Por baixo:** o wallpaper do Windows continua sendo a imagem do rice, sem efeitos. Se o worker parar, é ela que aparece.
   - **Plano v2:** o `video.json` passou a ser o plano dos wallpapers vivos. Cada entrada tem uma fonte (`video`, `image`, `color` ou `none`), os efeitos com os valores preenchidos e as partículas com as cores já resolvidas, então o worker nunca lê o rice. A versão 1 (só vídeos), guardada por um core mais antigo, ainda é lida.
   - **Uma imagem:**
     - é decodificada pelo WIC no tamanho em que é mostrada, com a orientação EXIF;
     - é posicionada, desfocada e recolorida **uma vez**, numa margem um pouco maior que o monitor (o que o parallax e o zoom precisam);
     - depois disso só se move.
   - **Um vídeo com efeitos:** o processador de vídeo escreve numa textura intermediária, e os efeitos de Direct2D (desfoque, matriz de cor) correm a cada quadro. Sem efeitos nem partículas, o caminho é o de antes: direto para a swap chain.
   - **Uma cor:** a matriz de cor é aplicada uma vez, no core.
   - **Custo:**
     - o que não se mexe é desenhado uma vez e custa 0% de CPU;
     - o parallax só redesenha enquanto a imagem alcança o mouse;
     - partículas e zoom seguem o `fps_limit`;
     - tudo pausa como um vídeo.
     - Uma imagem parada não é liberada depois de 2 minutos pausada, porque não custa nada e, ao voltar, mostraria a imagem sem efeitos por um instante.

3. **A física das partículas e a matriz de cor ficam em `libs/video`, sem Windows**, e são testadas e fuzzadas.
   - **Física:** quem sai da tela volta do lado de onde vem, e quem tem vida (brilhos, poeira mágica, notas, corações) nasce e some suavemente. O tamanho segue a altura do monitor.
   - **Matriz:** é a do `feColorMatrix` do SVG, no formato do Direct2D.

4. **Wallpaper Engine: só leitura, e só do que a pessoa já tem.**
   - **Onde procurar:** o caminho do Steam vem do registro e as bibliotecas do `libraryfolders.vdf`. Lemos o `project.json` de cada item do workshop (app 431960) e dos projetos do próprio Wallpaper Engine, além de qualquer pasta que a pessoa aponte.
   - **Arquivos:** um nome que leve para fora da pasta do item (`..`, um drive) é ignorado.
   - **Vídeo:** toca como está. Agora aceitamos também WebM.
   - **Cena, página web ou aplicativo:** só tocam dentro do Wallpaper Engine. Usamos a imagem de prévia (um GIF vira PNG), com um zoom lento.
   - **Sem workshop:** não baixamos nada nem falamos com a Steam.

5. **Arquivos de terceiros não saem do computador.**
   - **Marca:** um rice que usa um wallpaper do Wallpaper Engine recebe `shareable = false`.
   - **Efeito:** o `package::pack` recusa empacotá-lo, o que vale tanto para `rmw pack` quanto para o botão de compartilhar. A galeria nem mostra "Compartilhar" para ele.
   - **Rices pessoais:** os do dono (Little Witch Academia, Miku) usam a mesma marca e ficam fora do repositório.

## Números (Sandbox, GPU-PV)

| Medida | Resultado |
|---|---|
| Imagem com saturação 0 | cinza na tela (`#4A4A4A` onde o Windows mostra `#C82828`) |
| Imagem parada com efeitos | 0,00% de CPU depois de desenhada |
| Cor + vinheta + 360 partículas (neve e brilhos) a 30 fps | 0,23% de CPU, 39 MB privados |
| Parallax | a imagem acompanha o mouse; 0,06% de CPU com o mouse parado |
| Suíte `video` | 41/41 (o vídeo de antes e os wallpapers vivos) |
| Fuzzing | `plans` (com as partículas) e `steam_vdf`: nada encontrado |
