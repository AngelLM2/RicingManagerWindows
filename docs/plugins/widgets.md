# Widgets da barra (`.slint`)

Um widget de plugin é um componente [Slint](https://slint.dev) — a mesma linguagem da barra. A barra **compila** o `.slint` do plugin dentro dela (o código Luau, não: ele fica no `rmw-plugin-host`). Por isso o arquivo passa por um validador antes: o que entra na barra é só **interface**, nunca acesso a arquivos ou janelas.

## O contrato

```toml
# plugin.toml
[widget]
ui        = "widget.slint"
component = "Cpu"

[widget.properties]
text = "string"
load = "float"

[widget.callbacks]
clicked = []
```

```slint
// widget.slint
export component Cpu inherits Rectangle {
    in property <color> foreground;   // da barra: a cor do texto do rice
    in property <bool> vertical;      // da barra: true numa barra vertical
    in property <string> text: "…";   // do script (w:set), com um valor inicial
    in property <float> load;
    callback clicked();
    min-width: 72px;
    Text {
        text: root.text;
        color: root.foreground;
        vertical-alignment: center;
    }
    TouchArea {
        clicked => { root.clicked(); }
    }
}
```

- O componente de `component` precisa ser **exportado** e declarar, como `in property`, cada propriedade do manifesto, e cada callback. A barra confere ao carregar (e `rmw plugin validate` também).
- `foreground` e `vertical` são opcionais: declare-os para recebê-los da barra.
- Também da barra (v1.1): `in property <length> item-radius` (o raio dos destaques: segue os cantos da barra) e `in property <color> accent` (a cor de destaque do Windows, a que o rice aplica). Um destaque de passar o mouse que combina com a barra:
  `background: touch.has-hover ? root.foreground.transparentize(89%) : transparent; border-radius: root.item-radius; animate background { duration: 120ms; }`. Se o seu manifesto já tem uma propriedade `accent` (configurável pelo rice), a barra não a sobrescreve.
- Dê valores iniciais às propriedades (`: "…"`): a barra mostra o widget antes de o script responder.
- O widget recebe a altura da barra (ou a largura, numa vertical); peça largura com `min-width`/`preferred-width`. Para sumir, `visible: false` e largura 0 (veja `now-playing`).
- `vertical_component` (opcional) é usado em barras verticais.

## Tipos

| Manifesto | Slint | No Luau (`w:set`) |
|---|---|---|
| `string` | `string` | texto (até 4 KB) |
| `int` | `int` | inteiro |
| `float` | `float` | número |
| `length` | `length` | número (pixels lógicos) |
| `bool` | `bool` | `true`/`false` |
| `color` | `color` | `"#RRGGBB"` ou `"#RRGGBBAA"` |
| `image` | `image` | `""` (nenhuma), um caminho **na pasta do plugin** (`"icons/sol.svg"`, png/jpg/svg), `"window:<id>"` — o ícone de uma janela de `windows.list` — ou `"tray:<id>"` — um ícone da bandeja de `tray` (cada um exige a permissão do seu provedor) |
| `Nome` (struct) | `export struct Nome { ... }` com os mesmos campos | tabela `{ campo = valor }` |
| `[T]` | `[T]` | lista (até 256 itens) |

As structs do manifesto (`[widget.structs.Nome]`) e as do `.slint` precisam ter os mesmos campos e tipos. Veja `window-list` nos exemplos (`[Entry]` com `image`).

Callbacks levam até 4 argumentos `string`, `int`, `float` ou `bool`, que chegam ao Luau na mesma ordem: `callback pick(int);` ↔ `pick = ["int"]` ↔ `w:on("pick", function(id) ... end)`.

## O que o validador permite

Tudo da linguagem Slint **menos**:

| Recusado | Por quê |
|---|---|
| `import` de arquivos; só `import { Button, ... } from "std-widgets.slint";` | arquivos e fontes ficam fora; os widgets padrão do Slint ficam disponíveis |
| `@image-url` com algo além de um caminho relativo simples, ou fora da pasta | imagens só da pasta do plugin (até 32 por widget); `..`, absolutos e links que saem da pasta são recusados |
| `Timer` | quem marca o ritmo é o host (≥ 250 ms entre atualizações) |
| `Window`, `PopupWindow`, `Dialog`, `ContextMenu`, `ContextMenuArea`, `Menu`, `MenuBar`, `MenuItem`, `MenuSeparator` | a barra é a única janela |
| `iteration-count` | animação infinita faz a barra redesenhar para sempre |
| `for x in N` com `N` > 256 | |
| nomes começando com `Rmw` | são da barra |
| arquivos acima de 64 KB, 20 000 *tokens*, 32 níveis de aninhamento, textos acima de 4 KB | |

Animações com duração (`animate width { duration: 150ms; }`) e estados funcionam. Fontes: as instaladas no Windows, pelo nome (`font-family: "Segoe Fluent Icons"`).

Os nomes de nível superior (`Chip`, `Entry`...) são renomeados internamente: dois plugins podem ter um `Chip` cada um. As mensagens de erro usam os nomes do seu arquivo, com a linha.

## Desenvolvimento

```powershell
rmw plugin dev C:\code\meu-widget
```

Salvar o `.slint`, o `.luau` ou o `plugin.toml` recarrega o widget em menos de 1 s, sem reiniciar a barra. O `rmw plugin dev` mostra o log do plugin e o da barra.

Erros no `.slint` aparecem em duas etapas:

1. **O validador** (também no `rmw plugin validate`): o que é proibido acima, e se o componente declara as propriedades e callbacks do manifesto — `widget.slint:12: 'Timer' is not allowed in a plugin widget`.
2. **O compilador do Slint**, dentro da barra (a CLI não tem o Slint): erros de sintaxe, nomes que não existem, tipos que não batem com o `plugin.toml`. A barra tenta cada widget sozinho, deixa de fora só o que falhou e registra a linha do **seu** arquivo:
   `bar: plugin widget io.github.voce.meu-widget: widget.slint:7:18: <a mensagem do Slint>`. Um tipo que não bate com o manifesto aparece como `does not fit plugin.toml: ...`.

Nos dois casos, o resto da barra continua aparecendo.
