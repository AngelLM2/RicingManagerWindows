# `plugin.toml` — referência

Todo plugin é uma pasta com um `plugin.toml` na raiz. O validador é o mesmo na instalação, no `rmw plugin validate` e no `rmw plugin dev`: chaves desconhecidas são erro (um erro de digitação não passa calado), e cada mensagem diz o caminho (`widget.properties.title`) e o que era esperado.

```toml
schema = 1                                   # obrigatório, sempre 1 nesta versão

[plugin]
id          = "io.github.voce.now-playing"   # reverse-DNS, único, minúsculas
name        = "Now playing"
version     = "1.2.0"                        # semver
api         = "^1.0"                         # versões da API de plugins que o plugin aceita
authors     = ["voce"]                       # até 16
license     = "MIT"                          # SPDX
homepage    = "https://github.com/voce/now-playing"   # só https
description = "O que está tocando, com botões."
kind        = ["bar.widget"]                 # 1 a 5 de: bar.widget, bar.skin, tweak, action, rule
min_app     = "0.4.0"                        # opcional: versão mínima do Ricing Manager

[permissions]          # só para scripts; tudo é mostrado na instalação
providers = ["media"]
actions   = ["media.control"]
network   = []         # domínios (vazio = sem rede)
storage   = false

[entry]
script = "main.luau"   # obrigatório para action e rule; opcional para bar.widget

[widget]               # kind bar.widget
ui                 = "widget.slint"
component          = "NowPlaying"
vertical_component = "NowPlayingColumn"      # opcional: usado numa barra vertical

[widget.properties]
title = "string"

[widget.callbacks]
toggle = []

[config]               # esquema das configurações
show_artist = { type = "bool", default = true, label = "Mostrar o artista" }
```

## `[plugin]`

| Chave | Obrigatória | Regras |
|---|---|---|
| `id` | sim | reverse-DNS (`io.github.voce.nome`): minúsculas, dígitos, `-` e `.`; o mesmo id de um widget numa `[[bar]]` |
| `name` | sim | texto curto, não vazio |
| `version` | sim | semver (`1.2.0`, `0.3.0-beta.1`) |
| `api` | sim | requisito semver (`^1.0`, `>=1.0, <2.0`); se não aceitar a API deste Ricing Manager (**1.0**), o plugin não carrega e a mensagem diz as duas versões |
| `kind` | sim | um texto ou uma lista; cada tipo exige sua tabela (abaixo) |
| `authors`, `license`, `description` | não | textos; `description` aceita várias linhas |
| `homepage` | não | `https://` |
| `min_app` | não | versão mínima do Ricing Manager |

Cada `kind` pede a sua parte — e uma parte sem o `kind` correspondente também é erro:

| `kind` | Exige | Roda script? |
|---|---|---|
| `bar.widget` | `[widget]` | opcional (`[entry] script`) |
| `bar.skin` | `[skin]` | não |
| `tweak` | um ou mais `[[tweak]]` | não |
| `action` | um ou mais `[[action]]` e `[entry] script` | sim |
| `rule` | `[entry] script` | sim |

## `[permissions]`

O que o script pode **ler** (`providers`), **fazer** (`actions`), **alcançar** (`network`) e **guardar** (`storage`). Nada é dado sem estar aqui; o usuário vê a lista na instalação, e uma atualização que pede **mais** volta a pedir aprovação. Os nomes válidos e os dados de cada um estão em [api.md](api.md#provedores-ler).

| Chave | Valores |
|---|---|
| `providers` | `time`, `system.cpu`, `system.memory`, `network.stats`, `battery`, `audio.volume`, `monitors`, `virtual-desktops`, **`media`**, **`windows.list`**, **`tray`** |
| `actions` | `media.control`, `window.activate`, `window.focus`, `window.minimize`, **`window.close`**, `tray.click`, `rice.apply`, `rice.undo`, `wallpaper.next`, `bar.toggle`, `bar.on`, `bar.off`, `appearance.toggle`, `appearance.dark`, `appearance.light` |
| `network` | até 16 domínios públicos: `"api.example.com"` ou `"*.example.com"` (só subdomínios). Endereços IP, `localhost` e nomes locais (`.local`, `.lan`, `.home`, `.internal`, `.test`...) são recusados |
| `storage` | `true` para usar `rmw.storage` (1 MB) |

Em **negrito**: permissões sensíveis, destacadas na instalação (privacidade ou efeito que o usuário sente). Rede é sempre destacada.

## `[entry]`

| Chave | Regras |
|---|---|
| `script` | caminho relativo dentro da pasta, `.luau`, até 256 KB |

## `[widget]`

| Chave | Regras |
|---|---|
| `ui` | o arquivo `.slint` (relativo, dentro da pasta) |
| `component` | o componente exportado que a barra mostra (letra maiúscula, depois letras e dígitos) |
| `vertical_component` | opcional: o componente para barras verticais; sem ele, a barra vertical usa `component` |
| `[widget.structs.<Nome>]` | até 8 structs, cada uma com 1 a 16 campos `campo = "<tipo>"` |
| `[widget.properties]` | até 32: `nome = "<tipo>"` |
| `[widget.callbacks]` | até 16: `nome = ["<tipo>", ...]` (até 4 argumentos; `string`, `int`, `float` ou `bool`) |

Tipos: `string`, `int`, `float`, `bool`, `color`, `image`, `length`, uma struct declarada (`"Entry"`), ou lista de um deles (`"[Entry]"`, `"[string]"`). Nomes de propriedades e callbacks: minúsculas, dígitos e `-`, até 32 caracteres. `foreground` e `vertical` vêm da barra: declare-os só no `.slint` (ver [widgets.md](widgets.md)).

O componente precisa declarar cada propriedade (`in property`) e cada callback do manifesto; a barra confere isso ao carregar.

## `[config]`

Cada chave é uma configuração, com esquema — a interface gráfica (M7) gera o formulário dele. Até 64 configurações; nomes em `snake_case` (minúsculas, dígitos e `_`).

| `type` | Chaves extras | `default` |
|---|---|---|
| `bool` | — | `true`/`false` (padrão `false`) |
| `int` | `min`, `max` | inteiro no intervalo |
| `float` | `min`, `max` | número no intervalo |
| `color` | — | obrigatório: `"#RRGGBB"`, `"#RRGGBBAA"` ou `"palette.<nome>"` |
| `choice` | `choices = [...]` (obrigatório, até 32) | um dos `choices` |
| `string` | `max_length` (padrão e teto: o de textos longos) | texto |

Todas aceitam `label` e `description`. Uma cor `palette.<nome>` é resolvida com a paleta do rice aplicado (`palette.accent` usa o acento do rice, ou o do Windows).

### De onde vêm os valores

Do mais fraco ao mais forte:

1. o `default` do manifesto;
2. a tabela do plugin no rice: `[plugins."io.github.voce.now-playing"]`;
3. a tabela do widget na barra (só para widgets): `right = [{ id = "io.github.voce.now-playing", max_width = 300 }]`.

Um valor que não serve (tipo errado, fora do intervalo, escolha que não existe) vira um aviso e o valor da camada anterior vale. O script lê o resultado em `rmw.config` (ou `w.config`, por widget).

```toml
# rice.toml
[plugins]
requires = ["io.github.voce.now-playing@^1.2"]

[plugins."io.github.voce.now-playing"]
show_artist = false

[[bar]]
right = [{ id = "io.github.voce.now-playing", max_width = 320 }, "clock"]
```

## `[skin]` — `kind = "bar.skin"`

As mesmas chaves de uma `[[bar]]` do rice (`position`, `mode`, `thickness`, `length`, `margin`, `radius`, `background`, `foreground`, `autohide`, `hide_native_taskbar`, `left`, `center`, `right`...). Uma barra do rice com `skin = "<id>"` começa da skin; o que a barra escreve vence. Cores podem ser `palette.<nome>`: são resolvidas com a paleta do rice.

```toml
[skin]
mode       = "float"
thickness  = 40
margin     = [8, 12]
radius     = 12
background = { kind = "acrylic", opacity = 0.6 }
```

```toml
# rice.toml
[plugins]
requires = ["io.github.voce.ilha"]

[[bar]]
skin   = "io.github.voce.ilha"
center = ["clock"]            # sobrepõe o center da skin
```

## `[[tweak]]` — `kind = "tweak"`

Ver [tweaks.md](tweaks.md). Até 64 por plugin.

```toml
[[tweak]]
key   = 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
value = "HideFileExt"
type  = "dword"            # dword | string
data  = 0
restart_explorer = true    # opcional: avisa que o Explorer precisa reiniciar para mostrar tudo
```

## `[[action]]` — `kind = "action"`

Até 32 ações. O script responde com `rmw.onaction("<name>", fn)`; quem chama usa `<id do plugin>:<name>`.

| Chave | Regras |
|---|---|
| `name` | minúsculas, dígitos e `-`, até 32 caracteres; único no plugin |
| `label` | nome para a interface (padrão: `name`) |
| `description` | texto |

```toml
[[action]]
name  = "toggle"
label = "Modo foco"
```

```toml
# settings.toml
[[hotkey]]
keys   = "Ctrl+Alt+F"
action = "io.github.angellm2.focus-mode:toggle"
```

Uma ação de plugin pode ser chamada por atalhos, regras e `rmw action <id>:<ação>` — e só roda se o plugin estiver ligado (num rice aplicado ou com `rmw plugin enable`).

## Arquivos

- Todo caminho é **relativo** e fica **dentro** da pasta do plugin (`..`, caminhos absolutos, `:` e nomes reservados do Windows são recusados; links que saem da pasta também).
- Imagens: `.png`, `.jpg`, `.svg`; até 2 MB e 1024 px de lado.
- Texto em UTF-8 (com ou sem BOM).
