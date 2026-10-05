# A API `rmw` (Luau) — versão 1.0

Scripts são [Luau](https://luau.org) (o Lua tipado do Roblox) rodando no `rmw-plugin-host`. Tudo o que o plugin faz passa pela tabela global `rmw`.

**Tipos.** As definições estão em [`plugins/template/rmw.d.luau`](../../plugins/template/rmw.d.luau); `rmw plugin new` as copia para a pasta do plugin junto com `.luaurc` (modo estrito) e `.vscode/settings.json` para o [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp). Nomes de provedores e ações são conferidos (um erro de digitação vira erro no editor). Os valores dos provedores chegam como `any`: dê o tipo onde você os recebe — `function(cpu: RmwCpu)`, `local battery: RmwBattery? = rmw.provider("battery")`, `w: RmwWidget`. Os testes do projeto conferem todos os exemplos contra esse arquivo, em modo estrito.

## O ambiente

- Bibliotecas do Luau: `string`, `table`, `math`, `utf8`, `bit32`, `buffer`, `coroutine`, `os` (`os.time`, `os.date`, `os.clock`, `os.difftime`). Todas **somente leitura** (`luaL_sandbox`): um plugin não altera `string` nem `table` de ninguém.
- Fora: `loadstring`, `load`, `dofile`, `require`, `getfenv`, `setfenv`, `debug`. Não há `io`, nem arquivos, processos, registro ou rede direta.
- `print(...)` escreve no log do plugin, como `rmw.log`.
- Cada plugin tem a sua VM: globais de um não são vistas por outro.
- O script inteiro roda uma vez ao carregar (até 200 ms); depois, só os callbacks que ele registrou — cada um com **50 ms** no máximo. Um callback que passa disso é interrompido (inclusive dentro de `string.find` com padrões patológicos) e conta como falha.
- **Falhas:** um erro num callback é registrado no log com a linha; **3 falhas em 5 minutos** param o plugin (os outros seguem). `rmw plugin status` diz por quê; `rmw plugin enable <id>` religa.
- Memória: 16 MB por VM. Passar disso é um erro "not enough memory" no script.

## Valores que saem do plugin

O que vai para a barra, para `rmw.storage`, para ações ou para `rmw.json.encode` precisa ser **dado**:

- `nil`, `boolean`, `number` (sem `nan`/`inf`), `string` (UTF-8 válido, até 64 KB);
- tabelas que são **lista** (`{1, 2, 3}`, sem buracos) **ou registro** (`{a = 1}`, chaves de texto) — não as duas coisas;
- até 16 níveis e 10 000 elementos.

Funções, threads, buffers e userdata não saem (é um erro).

## Referência

### Informação

| | |
|---|---|
| `rmw.id: string` | o id do plugin |
| `rmw.version: string` | a versão da API (`"1.0"`) |
| `rmw.config: {[string]: any}` | as configurações resolvidas: padrão do manifesto ← tabela do plugin no rice (ver [manifesto.md](manifesto.md#de-onde-vêm-os-valores)). Widgets têm a sua em `w.config` |

### Log

| | |
|---|---|
| `rmw.log(...)` | uma linha de informação no log do plugin (`logs\plugins.log`; `rmw plugin dev` mostra ao vivo) |
| `rmw.warn(...)` | uma linha de aviso |

Até 1 KB por linha e 50 linhas a cada 10 s por plugin (o excesso é descartado).

### Tempo

| | |
|---|---|
| `rmw.every(ms, fn): number` | chama `fn()` a cada `ms` (≥ 250 ms); devolve o id do timer |
| `rmw.after(ms, fn): number` | chama `fn()` uma vez depois de `ms` (≥ 50 ms) |
| `rmw.cancel(id)` | cancela um timer |

Até 64 timers por plugin; no máximo 24 h. Para algo por widget, use `w:every`/`w:after` (são cancelados quando o widget sai da barra).

### Provedores (ler)

| | |
|---|---|
| `rmw.provider(name): any?` | o valor atual (ou `nil` antes da primeira leitura) |
| `rmw.on(name, fn(value))` | chama `fn` quando o valor **muda** |

Cada nome precisa estar em `[permissions] providers`. O host só lê o que algum plugin observa (e para quando ninguém mais observa).

| Provedor | Tipo | Valor | Atualiza |
|---|---|---|---|
| `time` | `RmwTime` | `{ unix: number, offset: number }` (minutos em relação ao UTC) | 1 s |
| `system.cpu` | `RmwCpu` | `{ usage: number }` (0–100) | 1 s |
| `system.memory` | `RmwMemory` | `{ usage: number, total: number, available: number }` (% e bytes) | 2 s |
| `network.stats` | `RmwNetwork` | `{ down: number, up: number }` (bytes/s, todas as placas) | 1 s |
| `battery` | `RmwBattery` | `{ present: boolean, percent: number, plugged_in: boolean, charging: boolean }` | 10 s |
| `audio.volume` | `RmwVolume` | `{ device: string, level: number, muted: boolean }` (0–100) | quando muda |
| `monitors` | `{ RmwMonitor }` | lista de `{ name, x, y, width, height, dpi, primary }` | 5 s |
| `virtual-desktops` | `RmwDesktops` | `{ count: number, current: number }` (`current` começa em 1) | 1 s |
| `media` ⚠ | `RmwMedia` | `{ session: boolean, playing: boolean, title, artist, album, app }` | quando muda |
| `windows.list` ⚠ | `{ RmwWindow }` | lista de `{ id: number, title, app, active: boolean, minimized: boolean }`, do topo para o fundo | quando muda (e 1 s) |
| `tray` ⚠ | `{ RmwTrayIcon }` | os ícones da bandeja do sistema: lista de `{ id: number, tip, app, promoted: boolean }` (`promoted`: o Windows o mostra na barra; os outros ficam no excesso); a imagem de um ícone é `"tray:<id>"` | quando muda (vem da barra) |

⚠ = sensível: destacado na instalação.

### Ações (fazer)

| | |
|---|---|
| `rmw.action(name, args?)` | pede uma ação; não espera nem devolve nada (o resultado aparece no log) |

Cada nome precisa estar em `[permissions] actions`.

| Ação | Argumentos | Faz |
|---|---|---|
| `media.control` | `{ command = "toggle" \| "play" \| "pause" \| "next" \| "previous" }` | controla o que está tocando |
| `window.activate` | `{ id = <id> }` | como um botão da barra de tarefas: traz a janela para a frente, ou minimiza se ela já está na frente |
| `window.focus` | `{ id = <id> }` | traz para a frente |
| `window.minimize` | `{ id = <id> }` | minimiza |
| `window.close` ⚠ | `{ id = <id> }` | pede para a janela fechar (como o X: o app pode perguntar) |
| `tray.click` | `{ id = <id>, button = "left" \| "right" \| "double" \| "middle" }` | clica num ícone da bandeja (o app recebe como se fosse na barra; `right` abre o menu dele) |
| `rice.apply` | `{ rice = "<pasta>" }` | aplica um rice **que vem com o plugin** (uma pasta com `rice.toml` dentro da pasta do plugin) |
| `rice.undo` | — | volta ao rice anterior (a última troca) |
| `wallpaper.next` | — | próxima imagem do slideshow |
| `bar.toggle`, `bar.on`, `bar.off` | — | mostra/esconde a barra |
| `appearance.toggle`, `appearance.dark`, `appearance.light` | — | modo escuro/claro |

Ações de janela usam o `id` de `windows.list`, e `tray.click` o de `tray` (as duas precisam da barra rodando). Ações pedidas durante um clique no widget ou uma ação chamada pelo usuário contam como **do usuário**; as que vêm de timers e provedores contam como **automáticas** (as mesmas regras de prioridade das regras do `settings.toml`).

### Ações próprias (`kind = "action"`)

| | |
|---|---|
| `rmw.onaction(name, fn(args))` | responde à ação `name`, declarada em `[[action]]`; `args` é a tabela de quem chamou (ou vazia) |

Quem chama usa `<id>:<name>`: `rmw action io.github.voce.foco:toggle`, um `[[hotkey]]` ou uma regra.

### Widgets (`kind = "bar.widget"`)

| | |
|---|---|
| `rmw.widget(fn(w))` | `fn` é chamada para **cada lugar** onde o widget aparece (duas barras = duas chamadas) |
| `w.id: string` | identifica este lugar |
| `w.config` | as configurações deste lugar (padrão ← rice ← a tabela do item na `[[bar]]`) |
| `w:set({ prop = valor, ... })` ou `w:set("prop", valor)` | muda propriedades declaradas em `[widget.properties]`; o tipo é conferido |
| `w:on("callback", fn(...))` | responde a um callback de `[widget.callbacks]`, com os argumentos declarados |
| `w:every(ms, fn)`, `w:after(ms, fn)` | timers deste lugar |
| `w:onclose(fn)` | chamado quando o lugar some (rice trocado, barra desligada) |

As mudanças são enviadas à barra **em lote**, no máximo a cada 250 ms por lugar; a resposta a um clique vai na hora. A barra guarda os últimos valores: se o host reinicia (ou o plugin é recarregado), o widget continua mostrando-os até o script responder. Detalhes do `.slint` em [widgets.md](widgets.md).

### Armazenamento (`storage = true`)

| | |
|---|---|
| `rmw.storage.get(key): any?` | |
| `rmw.storage.set(key, value)` | `nil` apaga |
| `rmw.storage.remove(key)` | |

Chaves de 1 a 128 bytes; até **1 MB** no total (o `set` que passaria disso é um erro). Gravado em disco pelo host a cada 5 s (se mudou) e ao sair, em `plugin-data\<id>.json`. Apagado junto com o plugin (`rmw plugin remove`).

### Rede (`network = [...]`)

| | |
|---|---|
| `rmw.http.get(url, options?, fn(response))` | |
| `rmw.http.post(url, body, options?, fn(response))` | `body`: texto até 64 KB |
| `options.headers` | `{ ["Nome"] = "valor" }`, até 16 |
| `response` (`RmwHttpResponse`) | `{ status: number, ok: boolean, body: string, type: string, error: string? }` |

- Só `https://`, porta 443, para os domínios declarados (`*.example.com` vale para subdomínios). Endereços IP e nomes locais nunca.
- Quem faz a requisição é o host (WinHTTP): **sem redirecionamentos**, sem cookies, sem credenciais do Windows; resposta até **1 MB** (o resto é cortado com erro).
- **Uma requisição a cada 10 s** por plugin; a que vier antes é respondida com `error = "too many requests..."` (não é falha do plugin). Até 4 aguardando ao mesmo tempo.
- Cabeçalhos que o host controla (`Host`, `Cookie`, `Content-Length`, `Connection`, `Proxy-*`, `Sec-*`...) não podem ser definidos.

### JSON

| | |
|---|---|
| `rmw.json.encode(value): string` | |
| `rmw.json.decode(text): (any?, string?)` | `nil` e a mensagem quando o texto não é JSON (até 1 MB, 16 níveis, 10 000 elementos) |

## Um widget completo

```lua
--!strict
local widgets: { [string]: RmwWidget } = {}

local function show(w: RmwWidget, cpu: RmwCpu?)
    if cpu then
        w:set({ text = string.format("CPU %d%%", math.floor(cpu.usage + 0.5)) })
    end
end

rmw.widget(function(w)
    widgets[w.id] = w
    show(w, rmw.provider("system.cpu"))
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("system.cpu", function(cpu: RmwCpu)
    for _, w in widgets do
        show(w, cpu)
    end
end)
```

Mais em [`plugins/examples`](../../plugins/examples): tocando agora, clima (rede + storage), gráfico de CPU, lista de janelas (structs, ícones de janela, barra vertical), modo foco (ação + rice dentro do plugin), relógio e bateria.

## Compatibilidade

- `api = "^1.0"` aceita qualquer 1.x. Até o Ricing Manager 1.0 a API é *prévia*: mudanças incompatíveis sobem a versão e aparecem no `CHANGELOG`.
- Depois da 1.0, o host aceita a versão atual e a anterior por um ciclo, com avisos de depreciação no log.
