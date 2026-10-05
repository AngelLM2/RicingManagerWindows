# 12 — Plugins: tudo é customizável pela comunidade

> Decisão do dono do projeto: **"tudo no programa é com mentalidade de ser customizável com plugin da comunidade"**. Este documento é a regra que vale para todas as features, do MVP em diante.

## 1. Princípio

> **Se é uma feature, é um ponto de extensão.** O que nós construímos usa as **mesmas APIs** que a comunidade usa (*dogfooding*). Se uma feature nossa não pode ser refeita por um plugin, a API está incompleta.

Na prática, cada feature nasce com:
1. **Dados expostos** por *providers* (ex.: lista de janelas, mídia tocando, CPU) — leitura, com permissão.
2. **Ações** chamáveis (ex.: focar janela, aplicar rice, próximo wallpaper) — por plugin, atalho, regra, CLI ou barra.
3. **Configuração com esquema** — a UI gera o formulário automaticamente, inclusive para plugins.
4. **Estilo por *tokens*** ligados à paleta do rice (cores, raio de canto, espaçamento, fonte).

## 2. Pontos de extensão (o que a comunidade cria)

| Ponto | O que é | Exemplo | Fase |
|---|---|---|---|
| `rice` | visual completo (dados) | "Tokyo Midnight" | MVP |
| `asset-pack` | paletas, cursores, ícones, fontes, wallpapers | pacote de cursores | MVP |
| `bar.widget` | módulo da barra | "tocando agora", clima, temperatura da GPU, lista de janelas alternativa | MVP |
| `bar.skin` | aparência/layout da barra | barra flutuante com cantos, estilo "ilha" | MVP |
| `tweak` | ajuste declarativo do Windows (registro HKCU de **lista permitida**) com desfazer | "esconder ícones da área de trabalho" | MVP |
| `action` | ação nomeada | "modo foco": esconde barra + wallpaper estático | MVP |
| `rule` | gatilho → ação/rice (horário, bateria, app aberto, monitor conectado) | "abriu jogo → rice de jogo" | MVP (básico) |
| `settings.page` | página de configuração (gerada do esquema) | — | MVP |
| `integration` | ponte com ferramentas externas | TranslucentTB, Windhawk | v1.1 / Fase 2 |
| `desktop.widget` | widget na área de trabalho (mesma engine da barra) | calendário, relógio grande | Fase 2 |
| `wallpaper.shader` | wallpaper procedural (HLSL) | chuva, partículas | Fase 2 |
| `wallpaper.source` | fonte de wallpapers (pasta inteligente; online só com permissão de rede) | "imagem do dia" | Fase 2 |
| `palette.generator` | gera paleta a partir de imagem | estilo pywal | Fase 2 |
| `provider` nativo | nova fonte de dados (ex.: sensores de hardware) | ponte com LibreHardwareMonitor | Fase 3 |

## 3. Níveis de confiança (o que o código do plugin pode executar)

| Nível | Formato | Onde roda | Pode | Não pode | Fase |
|---|---|---|---|---|---|
| **P0 — Dados** | TOML + assets | validador do core | descrever | executar qualquer coisa | MVP |
| **P1 — Tweak declarativo** | TOML | módulo `tweaks` do core | gravar valores em chaves HKCU de uma **lista permitida**, com snapshot/desfazer; pedir reinício do Explorer | chaves fora da lista; valores que pareçam caminhos para `.exe`/`.dll`; `Run`, `RunOnce`, `Software\Classes\CLSID`, `Policies`, `Environment`, `Winlogon`… (lista de bloqueio fixa) | MVP |
| **P2 — Script** | **Luau** + UI declarativa | **`rmw-plugin-host.exe`** (Job com teto de memória, sem rede, sem arquivos, sem processos) | ler *providers* permitidos, desenhar widgets, chamar ações permitidas, guardar estado próprio (cota), fazer requisições HTTP **pelo host** se tiver permissão de rede para domínios declarados | acessar diretamente arquivo, rede, registro, processos, outras janelas | MVP |
| **P3 — Shader** | HLSL | GPU, no `rmw-wallpaper`/`rmw-bar` | desenhar | qualquer coisa fora da GPU (watchdog de TDR) | Fase 2 |
| **P4 — Nativo** | C++ (DLL) | `rmw-pluginhost-native.exe` (Low IL, Job), só IPC | o que as permissões aprovadas liberarem | rodar dentro do core, da barra ou da UI | Fase 3 |

**Regra de ouro:** a barra (`rmw-bar`) e o core **nunca executam código de plugin**. Scripts rodam no `rmw-plugin-host`; a barra só recebe a árvore de widgets/valores por IPC e desenha. Plugin travou ou estourou memória → o host é reiniciado; a barra continua de pé mostrando o último estado.

## 4. Pacote e manifesto

> Implementado no M4 (ADR-0011). A referência completa, para quem escreve plugins, está em [`docs/plugins/`](plugins/README.md).

Pacote `.rmwp` (zip) lido pelo **mesmo leitor seguro** dos `.rice` (doc 06 §4; instalar de `.rmwp` chega no M8 — até lá, de uma pasta). Manifesto `plugin.toml`:

```toml
schema = 1

[plugin]
id       = "io.github.fulano.now-playing"   # reverse-DNS, único
name     = "Tocando agora"
version  = "1.2.0"
api      = "^1.0"                            # versão da API de plugins
authors  = ["fulano"]
license  = "MIT"
homepage = "https://github.com/fulano/now-playing"
kind     = ["bar.widget"]

[permissions]
providers = ["media"]            # o que pode LER
actions   = ["media.control"]    # o que pode FAZER
network   = []                   # domínios permitidos (vazio = sem rede)

[entry]
script = "main.luau"

[widget]                         # S7: um componente .slint, validado e compilado pela barra
ui        = "widget.slint"
component = "NowPlaying"

[widget.properties]              # o que o script envia (w:set), com tipo conferido pela barra
title   = "string"
playing = "bool"

[widget.callbacks]               # o que a barra devolve ao script (w:on)
toggle = []

[config]                         # esquema → formulário automático na UI
show_artist = { type = "bool", default = true,  label = "Mostrar artista" }
max_width   = { type = "int",  default = 240, min = 80, max = 600, label = "Largura máxima" }
accent      = { type = "color", default = "palette.accent", label = "Cor de destaque" }
```

Outras partes, por tipo: `[skin]` (`bar.skin`: as chaves de uma `[[bar]]`), `[[tweak]]` (`tweak`) e `[[action]]` (`action`: ações chamáveis como `<id>:<nome>`).

## 5. Permissões

Declaradas no manifesto, **mostradas na instalação** e verificadas pelo host em cada chamada.

| Permissão | Exemplo | Sensível? |
|---|---|---|
| `providers`: `time`, `system.cpu`, `system.memory`, `network.stats`, `battery`, `audio.volume`, `virtual-desktops`, `monitors` | barra de status | não |
| `providers`: `media` (título/artista/app), `windows.list` (títulos das janelas abertas; também os ícones `window:<id>`), `tray` (ícones e dicas da bandeja; imagens `tray:<id>`) | lista de janelas, tocando agora, bandeja alternativa | **sim — privacidade** (destacado na instalação) |
| `actions`: `media.control`, `window.activate/focus/minimize`, `tray.click`, `rice.apply` (só rices da pasta do plugin), `rice.undo`, `wallpaper.next`, `bar.toggle/on/off`, `appearance.toggle/dark/light` | botões da barra, modo foco | não |
| `actions`: `window.close` | fechar janelas | **sim** |
| `network`: lista de domínios públicos (sem IPs nem nomes locais) | clima | **sim** — requisição feita pelo host, com limite de tamanho e frequência |
| `storage` | lembrar preferências | não (cota de 1 MB) |

Atualização que **aumenta** permissões exige nova aprovação; sem aprovação, o plugin fica instalado e o script não roda. Adiados: `system.gpu` (precisa de um provedor de sensores, Fase 3) e `launch.app` (só apps instalados, por ID — com a interface, M7).

## 6. Orçamentos por plugin (impostos pelo host)

| Recurso | Limite inicial |
|---|---|
| Memória do VM Luau | 16 MB (alocador próprio) |
| Tempo por *callback* | 50 ms (interrupt) + watchdog de processo |
| Frequência de atualização | ≥ 250 ms entre atualizações (exceto animações declaradas) |
| Armazenamento | 1 MB |
| Rede | só domínios declarados; resposta ≤ 1 MB; ≤ 1 req/10 s por padrão |

Plugin que falha 3× em 5 min é parado (`rmw plugin status` diz por quê; `rmw plugin enable` religa); um plugin que estava rodando quando o host caiu duas vezes em 5 min também. A notificação na tela chega com a bandeja (M5).

## 7. API de plugins

- **Versionada** (`api = "^1.0"`, semver). O host suporta a versão atual e a anterior por um ciclo; avisos de depreciação no log do desenvolvedor.
- **Tipada**: publicamos definições de tipo Luau (`rmw.d.luau`) para autocompletar no editor (luau-lsp).
- **Mesma API para nós**: widgets nativos (relógio, mídia, CPU…) são reescritos ou espelhados como plugins Luau de exemplo — prova de que a API basta.
- **Estável a partir da v1.0**; antes disso marcada como *preview*.

## 8. Experiência do desenvolvedor de plugin

| Comando | O que faz |
|---|---|
| `rmw plugin new <modelo> <pasta>` | cria um plugin a partir de um modelo (widget, tweak, action, rule, skin), com os tipos da API (`rmw.d.luau`) e a configuração do luau-lsp |
| `rmw plugin dev <pasta>` | carrega a pasta com **recarga automática** ao salvar (< 1 s) + log do host e da barra |
| `rmw plugin validate <pasta>` | roda o mesmo validador da instalação |
| `rmw plugin pack <pasta>` | gera o `.rmwp` |
| `rmw plugin install/approve/enable/disable/remove/list/status` | gerenciar os instalados |

Mais: [repositório-modelo](../plugins/template), [documentação em `docs/plugins/`](plugins/README.md), 8 plugins de exemplo ([`plugins/examples`](../plugins/examples)) e o guia de publicação (licença, créditos, permissões mínimas).

## 9. Distribuição

- Plugins **não** ficam no repositório do projeto (exceto os exemplos oficiais).
- Instalação manual de `.rmwp` desde o MVP; **galeria** (só índice, pacotes nos repositórios dos autores) na Fase 3, com a mesma política de conteúdo dos rices (doc 11 §3) + permissões exibidas.
- Verificação de integridade por hash; assinatura de plugins pelo autor (opcional, Fase 3).

## 10. Decisões técnicas (spike S7 e M4)

Decididas: UI dos widgets = **(A) `.slint`** compilado pela barra depois de um validador nosso (ADR-0008, ADR-0011 §5–6); **um host compartilhado**, uma VM por plugin, culpa por plugin quando o host cai (ADR-0011 §2); lote por lugar a cada ≥ 250 ms, cliques na hora (ADR-0011 §7). As perguntas originais:

| Pergunta | Opções | Critério |
|---|---|---|
| Como plugins descrevem a **UI** do widget | (A) arquivo `.slint` interpretado pela barra; (B) árvore declarativa própria (linha/coluna/texto/ícone/imagem/gráfico/barra de progresso) vinda do Luau, desenhada em Direct2D | RAM da barra ≤ 25 MB com 10 widgets; atualização ≤ 4 ms; **conseguir restringir o carregamento de imagens/fontes à pasta do plugin** (requisito de segurança para A); recarga automática |
| Um host para todos os plugins ou um por plugin | um host compartilhado (menos RAM) + opção de isolar plugins novos/suspeitos num host próprio | medir RAM por VM Luau |
| Latência IPC host → barra | atualização em lote por quadro | ≤ 2 ms por lote |
