# ADR-0022 — Restaurar de verdade: o Windows como era, não "parecido"

- **Status:** aceito (v1.6)
- **Data:** 2026-10-06
- **Contexto:** no teste real, o dono viu que restaurar não voltava 100%: "partes do mouse não resetam, cores de temas". As suítes da Sandbox comparavam só alguns valores (alguns cursores, modo, destaque) e passavam.

## O que foi medido

Uma foto do registro (`reg query /s`) das áreas que o Windows usa para ponteiros, cores, temas, sons e ícones, tirada **antes de o Ricing Manager rodar pela primeira vez** e comparada depois de aplicar e restaurar (`system-tests.ps1`: `restore:registry-as-pristine`, e por rice em `extra:*:registry-as-before`, com os rices oficiais e os pessoais). Antes, a Sandbox recebe um estilo de ponteiro "do usuário" como o Configurações faz (arquivos próprios em `%LOCALAPPDATA%\Microsoft\Windows\Cursors`, tamanho 48, tipo e cor de Acessibilidade).

O que sobrava:

1. **O tema selecionado** (`Themes\CurrentTheme`) ficava `RicingManager.theme`: o desfazer aplicava um tema nosso com o modo e o destaque de antes, não o tema do usuário. Com ele ficavam a paleta de destaque, "destaque automático pelo papel de parede" e o resto do que um tema define.
2. **As cores recentes** (`Themes\History\Colors\ColorHistory0..5`) ganhavam o destaque de cada rice aplicado.
3. **Os ponteiros:** o gerenciador de temas antigo (usado quando o novo não acha o tema, e sempre na terceira tentativa) aplica o arquivo de tema inteiro, com a seção de cursores dele — que costuma ser mais velha que os ponteiros em uso. Isso trocava os ponteiros por trás do módulo de cursores; o desfazer então os via como "mudados depois" e os pulava.
4. **Os arquivos do usuário:** um cursor, som ou ícone que era um arquivo do usuário (por exemplo, os ponteiros coloridos que o Configurações gera) voltava apontando para a **cópia no armazém**, não para o arquivo dele. Depois de desinstalar com "apagar os dados", ficava quebrado.
5. **`Control Panel\Desktop\WallPaper`** ficava `TranscodedWallpaper` em vez do caminho original (a mesma imagem, outro valor).

## Decisões

1. **Um tema aplicado por nós nunca muda o que não é a aparência.** `Win32ThemeApi` guarda, antes de cada aplicação (nossa ou do tema original), todos os valores de `Control Panel\Cursors`, o estilo de ponteiro de `Accessibility` (`CursorSize`, `CursorType`, `CursorColor`), as cores recentes (`History\Colors`, `History\AutoColor`) e o tema para onde o Windows volta depois do alto contraste (`Pre-High Contrast Scheme`: um Windows novo nem o tem; o Windows o cria na primeira aplicação de tema e o escreve atrasado), e devolve depois o que mudou — logo em seguida e de novo 0,4 s e 1,6 s depois, porque o Explorer escreve alguns desses valores atrasado. Os ponteiros são recarregados (`SPI_SETCURSORS`, `SPI_SETCURSORBASESIZE`).
2. **O tema do usuário volta.** O módulo `appearance` ganhou o campo `theme` (`CurrentTheme`); um rice que muda a aparência o marca como `ricing-manager`. Ao restaurar, o arquivo de tema original é aplicado de novo (só a aparência, pelo `IThemeManager2`, achando-o pelo nome que o Configurações mostra; senão pelo gerenciador antigo, com os ponteiros guardados). O tema do usuário traz a aparência que ele tinha (o Windows atualiza o arquivo do tema atual quando a pessoa muda o modo ou o destaque), então normalmente não se aplica mais nada por cima: só se espera o modo e o destaque aparecerem (ver 5). No fim, o tema do usuário é nomeado o atual. Se o arquivo sumiu, o modo e o destaque voltam pelo tema nosso, como antes.
3. **Os arquivos do usuário voltam pelo caminho dele.** Cursores, sons e ícones guardam o caminho original e, ao lado, `copy` (o mesmo arquivo no armazém). O desfazer escreve o original enquanto ele existir; a cópia só entra se ele sumiu. `copy` nunca conta para "mudou depois".
4. **`WallPaper` volta:** o módulo `wallpaper` guarda o valor (`wallPaperValue`). Pôr a imagem por monitor (`IDesktopWallpaper`) faz o Windows escrever ali a cópia dele (`TranscodedWallpaper`), e escrever o valor por cima não adianta (o Explorer escreve de novo depois). Ao restaurar uma imagem que estava em todos os monitores, ela é posta mais uma vez como o Configurações faz (`SPI_SETDESKWALLPAPER`), e o Windows volta a nomear o arquivo original. O campo segue a imagem: nunca conta como "mudou depois".
5. **Tempo:** o tema do usuário se nomeia antes de o modo e o destaque dele chegarem ao registro; o desfazer espera até 10 s por eles (no primeiro tema de um Windows novo, o Sandbox mostrou que pode levar mais). Se não chegarem, a aparência de antes é posta pelo tema nosso e o tema do usuário é nomeado de novo por cima. Para isso (e para a tela de bloqueio, que pode levar dezenas de segundos), o `--apply-helper` dá 45 s a esses dois módulos, e 15 s aos outros.
6. **`Pre-High Contrast Scheme` é do Windows,** mas nunca um tema nosso: o Windows o reescreve no tempo dele (às vezes segundos depois de um tema aplicado), com o tema que acabou de aplicar ou com o anterior. Ao nomear o tema do usuário, se ali estiver um tema nosso, ele passa a ser o do usuário. O teste não o compara com a foto; confere que nunca aponta para um tema nosso (desligar o alto contraste traria a aparência de um rice de volta).
7. **O teste é o critério:** a foto do registro de antes do primeiro uso tem de ser igual à de depois de restaurar, com os rices oficiais e os pessoais.

## Consequências

- Quem já usou uma versão anterior tem o "Windows original" guardado sem o tema e com os ponteiros apontando para o armazém: restaurar volta tudo o que mudamos, mas o tema escolhido antes e os arquivos de ponteiro do Configurações não têm como voltar sozinhos. Basta escolher o tema (Personalização > Temas) e o estilo de ponteiro (Acessibilidade > Ponteiro do mouse) uma vez.
- As cores recentes que versões anteriores já empurraram ficam até o Windows trocá-las.
