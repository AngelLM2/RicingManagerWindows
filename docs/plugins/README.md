# Plugins — guia de quem escreve

> API de plugins **1.0 (prévia até o Ricing Manager 1.0)**. A regra do projeto: *tudo é customizável pela comunidade* ([doc 12](../12-plugins.md)). Os widgets embutidos da barra e os plugins usam os mesmos dados e as mesmas ações — o relógio e a bateria de [`plugins/examples`](../../plugins/examples) são widgets embutidos reescritos como plugins, para provar que a API basta.

## O que um plugin pode ser

| Tipo (`kind`) | O que faz | Roda código? |
|---|---|---|
| `bar.widget` | um widget para a barra: um componente `.slint` + (opcional) um script Luau que o alimenta | sim, no `rmw-plugin-host` |
| `bar.skin` | um "ponto de partida" para uma `[[bar]]`: tamanho, fundo, cantos, widgets | não (só dados) |
| `tweak` | valores do registro (HKCU, de uma lista permitida) aplicados e desfeitos como qualquer mudança | não (só dados) |
| `action` | ações com nome (`<id>:<ação>`) para atalhos, regras e a CLI | sim |
| `rule` | um script que observa provedores e chama ações sozinho | sim |

Um plugin pode ter mais de um tipo (um widget que também oferece ações, por exemplo).

## Onde o código roda (e onde não roda)

- **A barra e o core nunca executam código de plugin.** Scripts rodam no `rmw-plugin-host.exe`, num sandbox Luau por plugin: sem arquivos, sem rede direta, sem processos, sem `loadstring`, bibliotecas somente leitura.
- **Três travas** em cada plugin: 16 MB de memória por VM, 50 ms por callback (o Luau interrompe até padrões de string patológicos) e o vigia do processo (o host inteiro é reiniciado se travar; o plugin culpado é parado depois de derrubar o host duas vezes).
- Um plugin que falha 3 vezes em 5 minutos é **parado** até você ligá-lo de novo (`rmw plugin enable <id>`). Os outros plugins e a barra seguem.
- O widget `.slint` passa por um **validador** antes de ser compilado na barra: nada de `import` (só nomes de `std-widgets.slint`), imagens só da pasta do plugin, sem `Timer`, sem janelas, sem animações infinitas.

Detalhes em [seguranca.md](seguranca.md).

## Começar em 1 minuto

```powershell
rmw plugin new widget C:\code\meu-widget --id io.github.voce.meu-widget
rmw plugin dev C:\code\meu-widget          # roda da pasta; recarrega ao salvar (< 1 s); mostra o log
```

Para ver o widget na barra, ponha-o num rice:

```toml
[plugins]
requires = ["io.github.voce.meu-widget"]

[[bar]]
right = ["io.github.voce.meu-widget", "clock"]
```

e aplique o rice (`rmw apply <pasta-do-rice>`). Salvar `main.luau` ou `widget.slint` recarrega na hora.

Outros comandos:

| Comando | Para quê |
|---|---|
| `rmw plugin validate <pasta>` | as mesmas checagens da instalação (manifesto, script compila, `.slint` validado, imagens) |
| `rmw plugin pack <pasta> [--out arquivo.rmwp]` | gera o pacote para compartilhar (sem os arquivos do editor) |
| `rmw plugin install <pasta> [--yes]` | instala uma pasta (mostra as permissões e pergunta). Instalar direto de um `.rmwp` chega no M8, com o leitor de pacotes |
| `rmw plugin remove <id>` | desinstala (e apaga o `storage` do plugin) |
| `rmw plugin approve <id>` | aprova as permissões (uma atualização que pede **mais** espera nova aprovação) |
| `rmw plugin enable/disable <id>` | liga ações e regras sem precisar de um rice; religa um plugin parado |
| `rmw plugin list` / `rmw plugin status` | o que está instalado, aprovado, rodando, parado e por quê |
| `rmw action <id>:<ação>` | chama uma ação de plugin (o mesmo que um atalho faz) |

Modelos: `widget`, `action`, `rule`, `tweak`, `skin`. Os de script vêm com `rmw.d.luau` (os tipos da API), `.luaurc` e `.vscode/` para o editor autocompletar e conferir tipos com o luau-lsp. O [repositório-modelo](../../plugins/template) é o mesmo modelo `widget`, pronto para virar um repositório seu.

## Referência

- [manifesto.md](manifesto.md) — o `plugin.toml` inteiro.
- [api.md](api.md) — a API `rmw.*` do Luau, provedores e ações.
- [widgets.md](widgets.md) — o `.slint` do widget: o que é permitido, tipos, imagens, callbacks.
- [tweaks.md](tweaks.md) — tweaks: chaves permitidas, o que é sempre recusado, como desfaz. (Skins de barra: [manifesto.md](manifesto.md#skin--kind--barskin).)
- [seguranca.md](seguranca.md) — o modelo de segurança e os limites.

## Publicar

- Plugins da comunidade **não** ficam neste repositório (só os exemplos oficiais). Publique no seu repositório e compartilhe o `.rmwp`.
- Peça **só as permissões que usa**: `media`, `windows.list`, rede e `window.close` aparecem destacadas na instalação.
- Licença clara (`license` no manifesto), créditos de ícones e imagens, e um `homepage` (https).
- Versões semânticas: aumentar permissões numa atualização faz o usuário aprovar de novo.
