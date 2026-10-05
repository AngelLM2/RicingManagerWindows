# ADR-0008 — Spike S7: barra própria e plugins da comunidade

- **Status:** aceito — **opção (A): widgets descritos em `.slint` e compilados pelo interpretador do Slint**; plugins em **Luau** num processo host separado
- **Data:** 2026-10-05
- **Spike:** `spikes/s7-bar-plugins/` (`bar` e `host` no mesmo executável) — Windows Sandbox e host

## Perguntas (doc 04 / doc 12 §10)

1. UI dos widgets: (A) `.slint` interpretado ou (B) árvore própria em Direct2D? Critérios: barra ≤ 25 MB com 10 widgets, atualização ≤ 4 ms, lote IPC ≤ 2 ms **e** imagens/fontes restritas à pasta do plugin. Se as duas cumprirem → (A).
2. Custo do Luau no `rmw-plugin-host` + IPC; contenção de scripts hostis.

## Montagem testada

- 10 "plugins" em pastas próprias, cada um com um `widget.slint` (espaço de trabalho, título, mídia, relógio, CPU com barra, RAM, rede, volume, bateria, notificações).
- A barra **gera** um `.slint` raiz que importa os widgets (layout esquerda/centro/direita) e o compila com `slint::interpreter::ComponentCompiler` (renderizador por software).
- `rmw-plugin-host` (processo separado): 10 VMs Luau 0.729 (`luaL_sandbox` + `luaL_sandboxthread`, `loadstring` removido, alocador com teto de 16 MB, *interrupt* com orçamento de 50 ms) chamam `update()` a cada 250 ms e enviam um lote JSON por named pipe de **nome aleatório** (`FILE_FLAG_FIRST_PIPE_INSTANCE`, `PIPE_REJECT_REMOTE_CLIENTS`, cliente com `SECURITY_IDENTIFICATION`).
- A barra aplica o lote com `set_property` na thread da UI.

## Resultados

| Métrica | Sandbox | Host (RX 6600) | Critério |
|---|---|---|---|
| Private Bytes da barra (10 widgets) | 9,7 MB | **7,8 MB** | ≤ 25 MB ✅ |
| CPU da barra, 10 widgets a cada 250 ms | 0,05% | **0,014%** | — |
| Aplicar um lote (p95) | 0,13 ms | **0,06 ms** | ≤ 4 ms ✅ |
| Latência host → barra (p95) | 0,49 ms | **0,22 ms** | ≤ 2 ms ✅ |
| Compilar a barra (interpretador) | 9,7 ms | — | — |
| Host com 10 VMs Luau | 6,0 MB (≈ 343 KB de heap por VM) | — | — |
| `update()` dos 10 plugins (p95) | 0,06 ms | — | — |

Segurança:

| Teste | Resultado |
|---|---|
| Validação do `.slint` de plugins (`import`/`from`/`@image-url` com caminho absoluto, `..`, UNC, `:`) | 10/10 legítimos aceitos · **4/4 hostis rejeitados** |
| Loop infinito | interrompido em **50 ms** ("callback exceeded its time budget"); host e barra seguem |
| Bomba de memória | **teto atingido** ("not enough memory", pico 12,4 MB); host e barra seguem |
| `loadstring`, `io`, `os.execute` | inexistentes (`nil`) |
| Modificar funções embutidas | bloqueado ("attempt to modify a readonly table") |

Visual (captura conferida): todos os widgets renderizados corretamente na paleta do rice; emojis saem monocromáticos no renderizador por software (aceitável; ícones próprios em SVG/PNG resolvem).

## Decisões

1. **Widgets = componentes `.slint`** fornecidos pelos plugins, compostos num `.slint` raiz gerado a partir do layout do rice e compilados pelo interpretador (recarga automática = recompilar, ~10 ms). A opção (B) não é necessária.
2. A API C++ do interpretador **não** tem gancho de carregamento de arquivos → o **validador de código-fonte `.slint`** é obrigatório antes de compilar. No produto: tokenizador próprio (não regex), lista de construções permitidas, e **alvo de fuzzing** (somado à lista do doc 06 §4).
3. **Um `rmw-plugin-host` compartilhado** para os plugins Luau (≈ 0,35 MB por plugin), com teto por VM, orçamento de tempo por *callback* e Job Object no processo; opção de isolar um plugin suspeito em host próprio.
4. **Protocolo barra ↔ host**: lote por ciclo (JSON-RPC no produto), named pipe aleatório criado pelo core e passado por herança de handle (doc 03 §8).
5. **A janela da barra precisa de host Win32 próprio** (M3): o winit reposicionou a janela por conta própria, então AppBar/posição/DPI/clique-através ficam num host nosso, com o Slint desenhando via *Platform API* (custom window adapter) — como já previsto no doc 07.

## A confirmar no M3/M4

- Slint Platform API com janela própria (AppBar) mantendo os números acima.
- Recarga automática de plugins com estado.
- Bomba de CPU dentro de função C longa (ex.: padrões de string): o watchdog de processo do host cobre (doc 06 §6).
