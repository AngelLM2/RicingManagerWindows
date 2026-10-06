# 11 — Pacote inicial (rices oficiais) e conteúdo

> Objetivo: o app já sai da caixa com rices **bonitos** — incluindo estética anime/japonesa — sem nenhum risco jurídico para o projeto. Fontes e detalhes de licença: §7.

## 1. Regras do pacote oficial

1. **Só conteúdo redistribuível e compatível com GPLv3**: CC0, CC BY, CC BY-SA, MIT, ISC, Apache-2.0, GPL/LGPL, fontes OFL — cada asset com seu arquivo de licença e crédito (`CREDITS.md` por rice, exibido na tela "Sobre o rice").
2. **Sem personagens de terceiros** (anime, VTubers, games). Diretrizes de fã (Crypton, hololive, NIJISANJI, Kizuna AI) são **não comerciais** e **não sublicenciáveis** — incompatíveis com um projeto que qualquer um pode redistribuir.
3. **Sem arte gerada por IA no pacote oficial**: não tem proteção autoral (EUA: Copyright Office 2025; Brasil: Lei 9.610/98 exige autor pessoa física), corre risco de reproduzir personagens protegidos e costuma ser mal recebida pela comunidade.
4. **Arte original nossa** (SVG, procedural, shaders) é a base: dá identidade ao projeto e não depende de ninguém.
5. Os rices **não ficam no repositório do código** (vídeos incham o Git): repositório separado `rmw-rices` com fontes + script de build, publicados como *assets* da release.

## 2. Os 6 rices do pacote inicial (v1.0)

> **Na v1.0 (ADR-0015):**
> - **Prontos:** Diva Stage e Tokyo Midnight (com vídeo), Sakura Dawn, Mocha Kawaii e o kit "Your Rice".
> - **Arte:** desenhada por código em `rices/art/RiceArt.cpp` (Direct2D/DirectWrite/Media Foundation), com cursores próprios no lugar do Bibata recolorido.
> - **Onde ficam:** neste repositório, já que nada binário entra no Git. O target `rices` desenha e empacota.
> - **Pendentes:** Spring Spirits e Witch's Workshop esperam o OK do dono para baixar a arte CC BY de terceiros.
> - **Fontes:** as fontes do Windows (Segoe UI Variable, Yu Gothic UI); as da tabela, que precisariam ser baixadas, ficam para quando houver o OK.

| Rice | Clima | Paleta (hex) | Wallpaper | Cursores | Fonte |
|---|---|---|---|---|---|
| **Diva Stage** ★ | Vocaloid-*inspired* (sem personagem): palco, luzes, equalizador | `#0B1026` `#2CD5C4` `#FF4FA3` `#7B5CFF` `#E8FCFF` | **original**: holofotes, barras de equalizador e forma de onda em SVG + loop de vídeo renderizado do nosso shader | Bibata recolorido teal/rosa | M PLUS Rounded 1c + JetBrains Mono |
| **Tokyo Midnight** ★ | cidade japonesa à noite, chuva, neon | Tokyo Night: `#1A1B26` `#C0CAF5` `#7AA2F7` `#BB9AF7` `#7DCFFF` `#F7768E` | **original**: skyline procedural, shader de chuva, kanji em neon (夜, 東京), sem marcas reais | Bibata Modern Classic | Noto Sans JP + Fira Code |
| **Sakura Dawn** | claro, primavera, pétalas | Rosé Pine Dawn: `#FAF4ED` `#575279` `#D7827E` `#B4637A` `#907AA9` `#286983` | **original**: partículas de pétalas (procedural) | Bibata recolorido rosé | Zen Maru Gothic |
| **Spring Spirits** | pintura, anime-ish | Nord: `#2E3440` `#ECEFF4` `#88C0D0` `#A3BE8C` `#EBCB8B` | quadros + loop **sem áudio** do curta *Spring* (Blender Studio, CC BY 4.0) | Bibata Modern Ice | Inter + JetBrains Mono |
| **Witch's Workshop** | ilustração fantasia aconchegante | Gruvbox: `#282828` `#EBDBB2` `#FE8019` `#FABD2F` `#D3869B` `#8EC07C` | ilustrações de **Pepper&Carrot** (David Revoy, CC BY 4.0) | Phinger convertido (CC BY-SA 4.0) | Inter |
| **Mocha Kawaii** | pastel fofo, kaomoji, estrelinhas pixel | Catppuccin Mocha: `#1E1E2E` `#CDD6F4` `#F5C2E7` `#CBA6F7` `#94E2D5` `#FAB387` (Latte no modo claro) | **original**: gradiente pastel procedural com kaomoji e estrelas em pixel art | Bibata recolorido (ou Catppuccin cursors convertido) | M PLUS 2 |

★ = rices-vitrine (screenshots do README e da loja).

Ícones (Fluent UI System Icons — MIT, Phosphor — MIT, Lucide — ISC, Papirus/Tela/Colloid — GPL-3.0) entram nos rices para a **barra própria (M3)**, para **ícones do sistema por usuário (Fase 2)** e para o **Modo avançado** (doc 10).

## 3. E a Hatsune Miku (e outros personagens)?

**Decisão do dono do projeto:** Miku, anime e personagens em geral **não** são conteúdo oficial — são **customizações da comunidade**. O projeto oferece as ferramentas (importar, kit "Seu Rice", galeria com regras); o conteúdo é de quem o publica.

Nuance que orienta o desenho: imagem **pública** (visível na internet) não é o mesmo que imagem **livre para redistribuir** — o direito continua com o artista/empresa. Por isso:
- **Uso pessoal é livre**: cada pessoa monta o próprio rice com as imagens que quiser; nada é distribuído pelo projeto.
- **O app nunca embute nem baixa sozinho** conteúdo da comunidade; o usuário importa o `.rice` que escolheu.
- **Pacotes da comunidade vivem em repositórios dos próprios autores**, não nos do projeto.
- **A galeria online (Fase 4)** só indexa (não hospeda arquivos) e exige: campo de licença/origem de cada asset, créditos, selo **"Comunidade · não oficial"**, aviso de marca quando houver personagem, e um **canal de remoção** (pedido do titular → item sai do índice). Itens sem licença declarada não entram.

Detalhes do que cada caminho permite:

| Caminho | Pode? | Como |
|---|---|---|
| Miku no pacote oficial (GPL) | **Não** | A *Piapro Character License* proíbe uso comercial e sublicenciamento; as ilustrações oficiais (KEI etc.) são CC BY-NC 3.0, que a FSF classifica como não livre. "Hatsune Miku" e "初音ミク" são marcas registradas da Crypton. |
| **Rice "Diva Stage"** (inspirado, sem personagem) | **Sim** | paleta teal/rosa, palco, música — sem o visual característico da personagem (*twin-tails*, alho-poró, "01") |
| **Kit "Seu Rice"** — o usuário usa as próprias imagens | **Sim** | rice-modelo com a paleta Diva Stage e espaços vazios para o usuário arrastar a fan art que ele tem/comprou; uso pessoal, nada é distribuído por nós |
| **Pacote de fã feito pela comunidade** (no repositório do próprio autor) | **Responsabilidade do autor**; nossa galeria só indexa se seguir as regras acima | orientação que daremos aos autores no guia de contribuição: licença citando PCL + CC BY-NC 3.0; só ilustrações oficiais com o crédito exigido ou fan art com **permissão** do artista; gratuito, sem anúncios/doações; aviso de marca e "não oficial". O app **não** baixa sozinho. |

Créditos exigidos (se o Fan Pack existir): *"Hatsune Miku by Crypton Future Media, INC. 2007 is licensed under CC BY-NC 3.0"* para as oficiais; para obras derivadas, a menção à PCL com a URL da licença.

## 4. O que eu (o agente) consigo criar — e o que não

| Consigo | Não consigo |
|---|---|
| Arte **original** em SVG (geométrica, abstrata, paisagens estilizadas, neon, kanji, pixel art, kaomoji) | Ilustrações de **personagens** com qualidade de artista (e não devemos, pelas regras acima) |
| **Shaders** (HLSL) para wallpapers animados e para gerar loops de vídeo | Fotos |
| **Cursores** completos (SVG → `.cur`/`.ani` com o *clickgen*, MIT, via Python — já instalado nesta máquina) e recolorir o Bibata | |
| Paletas com contraste verificado (WCAG AA) para modo claro e escuro | |
| Ícones simples em SVG no estilo do rice | |

**Pipeline de assets** (repositório `rmw-rices`):
1. Fontes editáveis: `.svg`, `.hlsl`, definições de cursor, `rice.toml`.
2. Script de build:
   - rasteriza SVG em PNG nas resoluções comuns (1080p, 1440p, 4K, ultrawide) usando o próprio **Direct2D** do Windows (`ID2D1SvgDocument` — sem dependência extra);
   - renderiza shaders em **loops MP4/H.264** com o codificador de vídeo embutido do Windows (Media Foundation);
   - gera cursores com *clickgen*.
3. Validação de cada `.rice` com o **mesmo validador do app** (doc 06 §4) no CI.
4. `CREDITS.md` e licenças geradas automaticamente a partir de metadados de cada asset.

## 5. Quando

| Item | Marco |
|---|---|
| Diva Stage e Tokyo Midnight (estáticos) — usados nos testes desde o M1 | M1 (rascunho) → M8 (final) |
| Os 6 rices com cursores, vídeo e skin de barra | M8 (v1.0) |
| Kit "Seu Rice" | M8 |
| Versões com shader ao vivo (em vez de vídeo pré-renderizado) | Fase 2 |
| Ícones/fontes na barra própria | M3 (barra) / Fase 2 (ícones e fontes do sistema por usuário) |
| Pacotes de personagens (Miku etc.) | **comunidade** — fora do projeto; a galeria (Fase 4) só indexa com as regras do §3 |

## 6. Pacote da comunidade "Miku" (feito à parte, para o dono postar num fórum)

Pedido do dono do projeto: um pacote de customização com a Miku, **fora do projeto** (sem vínculo), para ele publicar num fórum.

| Item | Como |
|---|---|
| Onde | pasta **fora do repositório** (`C:\dev\treino\rmw-community-miku\`), nunca commitada no projeto |
| Quando | depois do **M3** (precisa do formato final do `rice.toml` **e** da skin da barra); rascunho possível após o M1 |
| Conteúdo feito por mim (original) | `rice.toml`, paleta (turquesa `#39C5BB`, rosa, cinza-azulado), **skin da barra** no estilo, **cursores originais** nas cores, layout de widgets |
| Imagens | (1) **ilustrações oficiais da Crypton** (CC BY-NC 3.0 — distribuição não comercial com o crédito *"Hatsune Miku by Crypton Future Media, INC. 2007, CC BY-NC 3.0"*) — **peço sua confirmação antes de baixar**, com nome, origem e tamanho; (2) fan art de terceiros **só com permissão do artista** — sem permissão, o pacote traz espaços vazios + links para a página do artista |
| Licença do pacote | **não** GPL: aviso "não oficial, não comercial", créditos PCL/CC BY-NC, aviso de marca ("Hatsune Miku" e "初音ミク" são marcas da Crypton) |
| Vínculo | o pacote não usa o nome/logo do projeto como autor; só diz "feito para o Ricing Manager for Windows" |

## 7. Fontes de licença (resumo)

| Asset | Fonte | Licença | Crédito | Pacote oficial? |
|---|---|---|---|---|
| Bibata (+ recolorações) | github.com/ful1e5/Bibata_Cursor | GPL-3.0 | manter LICENSE, crédito a Abdulkaiz Khatri | sim |
| clickgen / cbmp | github.com/ful1e5/clickgen | MIT | aviso de copyright | sim (ferramenta) |
| Phinger cursors | github.com/phisch/phinger-cursors | CC BY-SA 4.0 | "phinger-cursors by phisch, CC BY-SA 4.0" + indicar mudanças | sim |
| Catppuccin cursors | github.com/catppuccin/cursors | GPL-2.0 | manter licença | sim (arquivos separados) |
| Fluent UI System Icons | github.com/microsoft/fluentui-system-icons | MIT | aviso de copyright | sim |
| Phosphor / Lucide | phosphoricons.com / lucide.dev | MIT / ISC | aviso de copyright | sim |
| Papirus / Tela / Colloid | GitHub | GPL-3.0 | manter licença | sim (evitar ícones de marcas) |
| Pepper&Carrot | peppercarrot.com | CC BY 4.0 | "Copyright © David Revoy <ano>, www.peppercarrot.com" | sim |
| *Spring* (quadros e loop sem áudio) | studio.blender.org/films/spring | CC BY 4.0 | "© Blender Foundation \| cloud.blender.org/spring" | sim |
| NASA | images.nasa.gov | domínio público (EUA), com diretrizes | "Credit: NASA"; sem insígnias | sim (conferir créditos) |
| Fontes (JetBrains Mono, Fira Code, Inter, Noto Sans JP, M PLUS, Zen Maru Gothic) | GitHub / Google Fonts | OFL 1.1 | incluir OFL.txt | sim |
| Paletas (Catppuccin, Rosé Pine, Gruvbox, Tokyo Night, Nord) | GitHub | MIT / Apache-2.0 | só se copiarmos arquivos dos ports | sim |
| Unsplash / Pexels / Freepik / Shadertoy (padrão) | — | licenças restritivas (proíbem redistribuir em coleções/plataformas de wallpaper; Shadertoy é NC por padrão) | — | **não** |
| WhiteSur, "W11 Cursor Concept" | — | imita macOS / proprietário | — | **não** |
| Miku (oficial e fan art), hololive, NIJISANJI, Kizuna AI | piapro.net, sites das agências | não comerciais, não sublicenciáveis | — | **não** (ver §3) |

Itens a reconfirmar antes do M8: licença do Breeze, se os Catppuccin cursors são "GPL-2.0 ou posterior", e o padrão de licença do Shadertoy.
