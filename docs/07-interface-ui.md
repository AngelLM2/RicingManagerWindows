# 07 — Interface (UI)

## Dá para fazer uma UI bonita em C++?

**Sim.** Em 2026 existem pelo menos duas opções boas de verdade para C++ nativo no Windows: o **Slint** (UI declarativa com estilo Fluent parecido com o do Windows 11) e o **WinUI 3** (o próprio framework do Windows 11). O que **não** queremos é o caminho do Electron/WebView (centenas de MB de RAM para uma janela de configurações).

A arquitetura (doc 03) já reduz o risco dessa escolha: **a UI é um processo separado que só conversa com o core por IPC**. Se o framework escolhido decepcionar, trocamos a UI sem tocar no motor.

## Duas UIs diferentes, dois requisitos diferentes

| | (A) Configurações / galeria — MVP (M7) | (B) Barra própria e widgets residentes — MVP (M3) |
|---|---|---|
| Quando existe | só enquanto o usuário está configurando | o tempo todo |
| Prioridade | **beleza**, usabilidade, acessibilidade | **RAM mínima**, redesenho barato, temável pelo usuário |
| Orçamento | ≤ 120 MB aberta, 0 MB fechada | ≤ 25 MB por barra |

## Comparação (resumo da pesquisa, set/2026)

| Opção | Visual | RAM ociosa (medida/relatada) | Build com CMake | Acessibilidade | Licença | Veredito |
|---|---|---|---|---|---|---|
| **Slint 1.18** (API C++) | estilo **Fluent** padrão, convincente | **~11–20 MB** com renderizador por CPU (relatos de até ~50 MB em algumas máquinas) · ~155–200 MB com o padrão novo por GPU | **nativo** (SDK pré-compilado MSVC, sem Rust) | sim (UI Automation via AccessKit) | GPLv3 · *royalty-free* com atribuição · comercial | **Recomendado para (A)** |
| **WinUI 3** (Windows App SDK 2.5, C++/WinRT) | Fluent **real**, Mica/Acrylic nativos | dezenas de MB (sem número oficial; a Microsoft admitiu em 2026 que é alto) | **só MSBuild** para XAML (projeto `.vcxproj` separado) | a melhor | MIT | 2º lugar para (A) |
| Qt 6.12 (Quick, estilo FluentWinUI3) | bom | 60 MB+ | ok, mas compilação no vcpkg leva horas | boa | LGPLv3 (obrigações de distribuição) | pesado demais para a meta |
| Dear ImGui | "cara de ferramenta" | muito baixa | trivial | **nenhuma** | MIT | só para overlay de debug |
| Win32 + Direct2D puro | o que fizermos | a menor | trivial | temos que implementar tudo | — | meses de trabalho para (A); **base** do host em (B) |
| RmlUi 6.3 (HTML/CSS-like) | temável com "CSS" | baixa | ok | nenhuma | MIT | candidato para (B) |
| WebView2 / Tauri / Electron | ótimo | **100s de MB**, vários processos | — | boa | — | **descartado** (contra o princípio "leve") |

## Recomendação

### (A) Configurações e galeria → **Slint**, com estas regras

1. **Processo próprio** `rmw-settings.exe`, aberto sob demanda e encerrado ao fechar.
2. **Versão fixada** (SDK pré-compilado num caminho fixo / *overlay port* do vcpkg — não há port oficial).
3. **Renderizador escolhido explicitamente** (Skia por software ou software puro). **Nunca** confiar no padrão: entre versões o padrão mudou e a RAM saltou de ~15 MB para 150 MB+.
4. **Teste de orçamento no CI**: abrir a UI, esperar, medir *private bytes*, falhar se passar do limite.
5. Componentes que o Slint não traz e que vamos construir: barra lateral de navegação, seletor de cores, **grade virtualizada** de miniaturas (ListView de linhas com N itens), **mapa de monitores** (arrastar wallpaper para um monitor), *InfoBar*/notificações.
6. **Mica**: provavelmente **não** aparece com o renderizador por software (ele pinta pixels opacos por cima do *backdrop*). O S2 testa; o plano padrão é fundo sólido com a cor do rice ativo, que combina igualmente bem.
7. **Miniaturas** geradas no próprio processo da UI (que é descartável) usando `IShellItemImageFactory` — o mesmo cache de miniaturas do Explorer, que já sabe gerar prévia de **vídeo** — com fallback para WIC; cache LRU em disco em JPEG/PNG (o WIC não tem codificador WebP embutido). A UI é instância única e dona do `library.db`; pastas da biblioteca são reescaneadas ao abrir e monitoradas (`ReadDirectoryChangesW`) só enquanto ela está aberta. O core **nunca** decodifica imagens.
8. **i18n** com `@tr()` do Slint + arquivos gettext `.po` (pt-BR e en), desde o M7.
9. **Acessibilidade** dos componentes próprios: papéis e nomes acessíveis (AccessKit), alternativa por teclado para tudo que é arrastar (ex.: "Aplicar ao monitor ▸"), checagem com o Narrador no S2.

**Por que não WinUI 3 como primeira opção?** Visual e acessibilidade são os melhores, mas: a compilação de XAML ainda exige MSBuild (quebra a nossa toolchain CMake/Ninja), C++/WinRT com XAML é lento de desenvolver (IDL, código gerado, builds longos), runtime extra de ~30–44 MB, e o consumo de RAM é reconhecidamente alto. **Se você priorizar fidelidade total ao Windows 11 acima de leveza e simplicidade de build, WinUI 3 é a escolha** — a arquitetura suporta.

### (B) Barra própria e widgets → MVP (marco M3), motor decidido no spike S7

Desenho: um **host Win32 próprio** (`rmw-bar`: AppBar, janelas por monitor, DPI, fundo acrylic/mica na **nossa** janela, DirectComposition) + um **motor de widgets** que **não executa código de plugin** — os plugins Luau rodam no `rmw-plugin-host` e mandam só valores/árvores por IPC (doc 12). Duas opções, decididas pelos critérios do S7 (doc 04):

- **(A) Slint** (via *Platform API*, renderizador por CPU, interpretador de `.slint` com recarga automática): plugins descrevem a UI em `.slint` — mesma tecnologia da UI de configurações, menos código nosso. Requisito de segurança: conseguir restringir imagens/fontes à pasta do plugin.
- **(B) Árvore declarativa própria** em Direct2D/DirectWrite (linha, coluna, texto, ícone, imagem, gráfico, barra de progresso, com estilos): mais controle e menos RAM, mais código nosso.

Os **tokens de tema** (cores, fontes, espaçamentos, raio) ficam no `rice.toml`, neutros de framework, e são convertidos para o motor. Assim rices e plugins sobrevivem a uma troca de tecnologia.

## Licença do projeto: GPLv3 (decidido — D1)

| Licença do projeto | Slint para (A) | Slint para (B) com temas `.slint` de usuários |
|---|---|---|
| **GPLv3** | ok | ok |
| **MIT** (ou Apache-2.0) | ok com a licença *royalty-free* do Slint: exige o widget "Sobre o Slint" na tela Sobre **ou** o selo na página de download | **problema provável**: a licença *royalty-free* proíbe "expor as APIs" do Slint; deixar usuários escreverem `.slint` pode se enquadrar → usar RmlUi em (B) |

**Interação com a assinatura de código gratuita:** a SignPath Foundation exige licença OSI **sem componentes proprietários**. Usar o Slint sob a licença *royalty-free* (que não é OSI) provavelmente desqualifica o projeto; usar o Slint sob **GPLv3** não. Ou seja: **Slint + GPLv3 é a combinação mais coerente** (UI moderna, barra temável no futuro e assinatura gratuita). Com MIT, a assinatura iria para Certum (pago) ou Store.

Nota técnica: o SDK pré-compilado do Slint é uma DLL (`slint_cpp.dll`, feita em Rust) que fica ao lado do `rmw-settings.exe` e provavelmente importa o `vcruntime140.dll` (distribuído app-local). Só o `rmw-settings` libera a pasta do app na busca de DLLs (doc 06 §5); a convivência com o nosso CRT estático é verificada no spike S6.

## Diretrizes de design da UI

- Visual Fluent (cantos arredondados, tipografia Segoe UI Variable, ícones Fluent), modo claro/escuro seguindo o sistema **e** a paleta do rice ativo.
- **Prévia antes de aplicar** sempre que possível; aplicar mostra o que vai mudar ("3 alterações: wallpaper do monitor 2, modo escuro, cursor").
- Ações destrutivas/arriscadas com confirmação e **desfazer** imediato (barra "Aplicado · Desfazer").
- Navegação completa por teclado, foco visível, nomes acessíveis, respeito a alto contraste e escala de texto.
- Textos em pt-BR e en desde o início (nada de string fixa no código).
- Animações curtas e desligáveis ("reduzir movimento").

## Telas do MVP

Ver doc 04, marco M7 (inclui editor visual da barra e gerenciador de plugins).
