# ADR-0015 — M8: o pacote (importação, rices oficiais, instalador)

- **Status:** aceito
- **Data:** 2026-10-05
- **Contexto:** doc 04 (M8 e a "Definition of MVP"), doc 06 §4 (entrada não confiável), doc 11 (pacote inicial), D5 (assinatura Certum)

## Decisões

1. **Um pacote de fora só é aberto num processo próprio.** `rmw-core --import-worker` roda num Job com teto de 384 MB, sem poder criar processos e morto junto com o core, com 10 minutos de prazo.
   - **O que o core faz:** nunca abre o zip. O worker desempacota numa pasta nova em `<dados>\imports\<aleatório>` (o mesmo volume de `rices\` e `plugins\`) e deixa um relatório JSON. O core lê o relatório, confere o `rice.toml` e põe a pasta no lugar com um *rename*: `rices\<nome do rice>`, com " 2", " 3"... quando o nome já existe.
   - **Plugins:** um `.rmwp` passa pelo mesmo worker e segue para a instalação de sempre, a partir da pasta.
   - **Em caso de falha:** a pasta de staging é apagada.
   - **Disponível como:** `rice.import`, `plugin.install` com um arquivo, `rmw import`, `rmw plugin install x.rmwp`, e na janela os botões "Importar um .rice…" e "Instalar de um arquivo…".

2. **O leitor (`rmw::package`) segue o doc 06 §4:**
   - **Abertura:** libzip, sem as *features* opcionais do vcpkg, com `ZIP_RDONLY | ZIP_CHECKCONS` e nomes decodificados por `ZIP_FL_ENC_STRICT`.
   - **Nomes:** todos são conferidos antes de qualquer gravação: um nome ruim recusa o pacote inteiro.
   - **Entradas recusadas:** links (atributos Unix), entradas criptografadas, métodos que não sejam *stored* ou *deflate*, duplicatas sem diferença de caixa (comparação do Windows) e extensões fora da lista do tipo (rice: TOML, PNG/JPEG/BMP, MP4, `.cur`/`.ani`/`.inf`, textos; plugin: TOML, Luau, Slint, PNG/JPEG/SVG, JSON, textos).
   - **Limites:** por arquivo e por pacote, contados nos bytes **reais**. A razão de 100:1 tem folga de 1 MiB, porque arquivos minúsculos comprimem mais que isso sem perigo.
   - **Conteúdo:** conferido pela assinatura. Imagens: PNG/JPEG/BMP com o tamanho declarado de até 16 384 px por lado e 64 MP, lido no cabeçalho, sem decodificar. Cursores: o validador próprio. Vídeo: caixa `ftyp`. Textos: UTF-8.
   - **Gravação:** caminhos `\\?\`, `CREATE_NEW` + `FILE_FLAG_OPEN_REPARSE_POINT` e o caminho final do arquivo (`GetFinalPathNameByHandle`) comparado com o do staging. Se uma pasta no caminho levava para fora, o arquivo vazio é apagado ao fechar.
   - **Ordem:** arquivos pequenos são conferidos inteiros na memória antes de gravar; vídeos são gravados em fluxo, com o começo conferido antes do primeiro byte.
   - **Desvio 1, "Unicode Path" (0x7075):** a libzip usa esse campo só quando o CRC dele confere com o nome do diretório central. O nome conferido é sempre o nome gravado, então não sobra divergência entre o que se valida e o que se escreve.
   - **Desvio 2, decodificadores:** a importação não decodifica imagem nenhuma (o doc previa decodificadores WIC por CLSID). Ela só lê cabeçalhos. Quem decodifica é quem mostra: o `rmw-wallpaper` e as miniaturas do `rmw-settings`.

3. **Empacotar é o inverso, com as mesmas regras.** É o que fazem `rmw pack`, `rmw plugin pack` (que deixa de ter um escritor de zip próprio) e "Compartilhar…" na janela.
   - **Formato:** zip *stored* com nomes UTF-8 e data fixa (os mesmos arquivos dão os mesmos bytes).
   - **Gravação:** em fluxo num `.partial`, que só substitui o destino quando completo.
   - **Recusas:** o que uma importação recusaria é recusado aqui, onde o autor pode corrigir: links, tipos de fora e conteúdo que não bate com a extensão.
   - **Deixados de fora:** arquivos de editor (`.git`, `.vscode`, `*.d.luau`), `Thumbs.db` e `desktop.ini`.

4. **Rices oficiais são código, não arquivos.**
   - **Fontes:** cada rice tem `rices/<id>/rice.toml` + `CREDITS.md`. O `rices/art/RiceArt.cpp` desenha as imagens, com Direct2D (o *blur* das luzes), DirectWrite (kanji e kaomoji com as fontes do Windows), WIC e o codificador H.264 do Media Foundation.
   - **O que sai:** imagens 4K, *loops* 1080p/30 sem emenda (o quadro *f* desenha a fase *f/N*), 17 cursores por rice (`.cur` de 32 a 96 px; ocupado e trabalhando como `.ani` de 12 quadros) e o ícone do app.
   - **Build:** o target `rices` (fora do `all`) desenha e empacota com `rmw pack`, que valida cada rice como uma importação faria. Os 5 rices levam 14 s numa GPU e 3 min no build release.
   - **Desvio do doc 11:** o doc previa um repositório `rmw-rices` à parte. Como nada binário entra no Git, as fontes ficam aqui.
   - **Cursores:** são desenhos nossos, não o Bibata recolorido, para não baixar nada.
   - **O que existe:** Diva Stage e Tokyo Midnight (com vídeo), Sakura Dawn, Mocha Kawaii e o kit "Your Rice".
   - **Pendentes:** Spring Spirits e Witch's Workshop usam arte CC BY de terceiros (Blender Studio, David Revoy) e esperam o OK do dono para baixar.

5. **Instalador por usuário com Inno Setup 6.7.3 (`installer/RicingManager.iss`).**
   - **Sem admin:** `PrivilegesRequired=lowest`, programas em `%LOCALAPPDATA%\Programs\Ricing Manager`, entrada de desinstalação no HKCU e o runtime do Visual C++ ao lado dos programas (*app-local*).
   - **Rices oficiais:** entram na biblioteca com `onlyifdoesntexist` (as edições do usuário ficam) e não são removidos ao desinstalar, a menos que o usuário peça para apagar os dados.
   - **Atualizar:** `rmw shutdown` antes de trocar os arquivos e `rmw-core --startup` depois, se ele iniciava com o Windows. A migração de dados é do próprio core.
   - **Desinstalar:**
     - pergunta se restaura o Windows original (`rmw restore --original`);
     - remove a entrada `Run`;
     - para o core, e a barra devolve a do Windows ao terminar;
     - pergunta se mantém `%LOCALAPPDATA%\RicingManagerWindows`.
     - Em modo silencioso, `/RESTORE=` e `/KEEPDATA=` respondem.
   - **Onde obter o ISCC:** o instalador do Inno Setup tem um formato que nenhum extrator lê (o innoextract para no 6.1). O `fetch-deps.ps1` confere o SHA-256 e a assinatura (Pyrsys B.V.) e o instala, em modo portátil, **dentro de uma Windows Sandbox descartável**, que copia a pasta para fora. No PC só roda o compilador.

6. **O que uma release leva:** `scripts/package.ps1` monta tudo em `build\package\<versão>\`, sem publicar nada:
   - o instalador;
   - os `.rice` oficiais e os plugins de exemplo em `.rmwp`;
   - `SHA256SUMS.txt`;
   - um SBOM SPDX 2.3 (nossos programas, as *ports* do vcpkg que eles linkam e o Slint);
   - os manifestos do winget, prontos para enviar quando a release existir (modelos em `installer/winget/`).
   - **Assinatura:** o `scripts/sign.ps1` assina com o certificado Certum do dono (SHA-256 + carimbo de tempo RFC 3161) e confere. Fica pronto, mas não é executado: o certificado nunca entra no repositório nem no CI.

7. **O ícone do app** sai do mesmo gerador (`rmw-rice-art --icon apps\resources`). O ícone e o PNG ficam no repositório. Ele aparece no executável (Iniciar, Explorer), na janela e na bandeja do core.

## Adiado

- **Publicar:** a release no GitHub, o winget e a atestação de build do CI dependem do OK do dono para o *push*. A assinatura de verdade depende do certificado do dono.
- **Os dois rices com arte de terceiros:** dependem do OK para baixar.
- **Galeria e índice de rices da comunidade:** Fase 4 (doc 11 §3).

## Números

| Medida | Resultado |
|---|---|
| Testes de unidade | 351 (36 novos: pacotes, processos de uma pasta); todos também com ASan |
| Fuzzing | 13 alvos, 60 s cada (o novo `package`: 694 mil execuções), nada encontrado |
| Regressão na Sandbox | `settings` 28/28, `system` 53/53, `bar` 28/28, `plugins` 43/43, `tray` 43/43, `video` 26/26 |
| Suíte `packages` | 35/35 — todos os pacotes hostis recusados, nada escapa nem sobra; o worker usou no máximo 4 MB |
| Suíte `installer` | 36/36, duas rodadas seguidas |
| Instalador | 33,3 MB; instala em 5,5–6,6 s, atualiza em 3,4 s |
| Aplicar um rice oficial (logo após instalar) | 0,6–1,8 s (Sakura Dawn, Mocha Kawaii, Tokyo Midnight, Diva Stage); meta < 3 s |
| Desenhar e empacotar os 5 rices | 14 s numa GPU (debug), 3 min no build release |
| Pacotes | Diva Stage 15,0 MB, Tokyo Midnight 8,5 MB, Mocha Kawaii 3,2 MB, Sakura Dawn 2,7 MB, Your Rice 1,7 MB |

## O que a Sandbox achou

- **Atualizar e desinstalar:** o `rmw shutdown` voltava antes de o core e os workers saírem. O instalador então encontrava arquivos em uso: atualizar terminava com o código 5 e desinstalar deixava os programas. Agora existe `rmw shutdown --wait`, que espera os processos desta instalação e fecha a janela de configurações.
- **Barra do Windows escondida depois de desinstalar:** isso acontecia ao desinstalar sem restaurar, com uma barra própria na tela. O `TerminateProcess` não espera, e o core mostrava a barra do Windows enquanto a barra própria ainda morria, com o gancho que a esconde de novo. Agora `ChildProcess::terminate` espera o processo acabar (até 2 s).
- **Acento antigo depois de trocar de claro para escuro:** às vezes o Windows aplica o modo e mantém o acento antigo. A nova tentativa só vinha depois de 3,3 s, e o Mocha Kawaii aplicava em 3,8 s. Agora o tema é reaplicado depois de 1,2 s (e depois de 3 s).
