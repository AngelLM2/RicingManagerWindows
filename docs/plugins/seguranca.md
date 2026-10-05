# Segurança dos plugins

Plugins vêm de qualquer pessoa. O modelo parte daí: **um plugin malicioso não deve conseguir mais do que as permissões que o usuário aprovou**, e um plugin com defeito não deve derrubar a barra, o core ou o Windows. Este documento é para quem escreve plugins, para quem revisa e para quem quer saber o que está instalando. Vulnerabilidades: [SECURITY.md](../../SECURITY.md).

## Camadas

| Camada | O que garante |
|---|---|
| **Níveis** ([doc 12 §3](../12-plugins.md)) | P0 (dados) e P1 (tweaks) nunca executam nada; só P2 (Luau) roda código, e só no `rmw-plugin-host` |
| **Regra de ouro** | a barra e o core **nunca** executam código de plugin. A barra recebe valores pelo pipe e os desenha; o core recebe pedidos de ação e os confere |
| **Sandbox do Luau** | sem `io`, arquivos, processos, registro, rede direta, `loadstring`/`load`/`require`/`getfenv`/`setfenv`/`debug`; bibliotecas somente leitura; uma VM por plugin |
| **Permissões** | cada provedor, ação, domínio de rede e o armazenamento são conferidos **a cada chamada** contra o que o usuário aprovou |
| **Processo do host** | Job com teto de 256 MB, **proibido criar processos**, encerrado junto com o core; mitigações estritas (sem código dinâmico, só DLLs assinadas pela Microsoft depois da partida, sem pontos de extensão, sem imagens remotas ou de baixa integridade); vigia que o reinicia se travar por 3 s |
| **Validador do `.slint`** | o que entra na barra é só interface: nada de imports de arquivos, imagens fora da pasta, `Timer`, janelas, animações infinitas; limites de tamanho |
| **Validador do manifesto e dos tweaks** | chaves desconhecidas são erro; tweaks só em `HKCU` de uma lista permitida, com lista de bloqueio que vence a permitida ([tweaks.md](tweaks.md)) |

## Permissões e aprovação

- Instalar (`rmw plugin install`) mostra **tudo** o que o plugin pede, com as sensíveis destacadas, e pergunta. Sem aprovação o plugin fica instalado, mas o script não roda (`rmw plugin approve <id>` depois).
- Uma atualização que pede **mais** do que o aprovado fica parada até nova aprovação; uma que pede menos roda com menos.
- O script recebe a **interseção** do que o manifesto pede com o que foi aprovado.
- Sensíveis (destacadas): `media` e `windows.list` (privacidade: o que você ouve e os títulos das suas janelas), `window.close` (efeito que você sente), qualquer domínio de `network`.
- Os ícones de janelas (`"window:<id>"`) exigem `windows.list`: o ícone também diz o que está aberto.
- `rmw plugin dev` também pergunta antes de rodar uma pasta que pede permissões.

## Rede

O script nunca abre conexões. O host faz a requisição por ele, e só se:

- for `https://` na porta 443, para um domínio declarado e aprovado (IPs e nomes locais são recusados já no manifesto; o host também recusa nomes que resolvem para endereços locais);
- sem redirecionamentos, cookies, proxy de autenticação ou credenciais do Windows;
- no máximo 1 requisição a cada 10 s por plugin, 4 em andamento, corpo enviado ≤ 64 KB, resposta ≤ 1 MB;
- cabeçalhos que o host controla (`Host`, `Cookie`, `Content-Length`, `Connection`, `Proxy-*`, `Sec-*`...) não podem ser definidos.

## Limites (o que acontece quando um plugin passa deles)

| Recurso | Limite | Ao passar |
|---|---|---|
| Memória da VM | 16 MB | erro no script ("not enough memory") |
| Carregar o script | 200 ms | o plugin não carrega |
| Cada callback | 50 ms | o callback é interrompido (conta como falha) |
| Falhas | 3 em 5 min | o plugin é **parado** até `rmw plugin enable` |
| Host inteiro | 256 MB, sem resposta por 3 s | o host é reiniciado; um plugin que estava rodando em duas quedas em 5 min é parado |
| Timers / handlers | 64 / 64 | erro no script |
| Atualizações de widget | 1 a cada 250 ms por lugar (cliques: na hora) | agrupadas |
| Armazenamento | 1 MB | o `set` falha |
| Log | 50 linhas / 10 s, 1 KB por linha | descartado |
| Valores para fora | 64 KB por texto, 16 níveis, 10 000 elementos; texto de widget 4 KB, listas 256 | erro no script |
| Script / `.slint` / imagem | 256 KB / 64 KB / 2 MB e 1024 px | o plugin não carrega |

Quando o host cai, o core sabe qual plugin estava rodando (um registro compartilhado que o host atualiza antes de cada callback) e culpa esse plugin, não os outros.

## O que a barra aceita do host

- Só conexões do `rmw-plugin-host.exe` da mesma pasta (o caminho do processo do outro lado do pipe é conferido).
- Cada valor é conferido de novo contra o tipo declarado no manifesto; imagens só da pasta do plugin (o caminho final, depois de resolver links) ou ícones de janelas.
- Ações de janela (`window.*`) são decididas pela barra no momento do clique, para janelas que ela mesma lista.

## O que o core aceita do host

- Só as ações da lista de ações para plugins, e só se o plugin tem a permissão (o host confere; o core confere de novo que é uma ação de plugin).
- `rice.apply` só aplica um rice **de dentro da pasta do plugin**, e passa pelo mesmo validador e pelo mesmo motor (snapshot e desfazer) que qualquer rice.

## Revisando um plugin

1. `rmw plugin validate <pasta>` — o mesmo validador da instalação.
2. Leia `[permissions]`: cada item tem um motivo no plugin? Rede para quê, e para quais domínios?
3. Leia o `.luau`: o que vai em `rmw.http`, o que vai para `rmw.storage`, quais ações chama e quando (num clique ou sozinho).
4. Tweaks: cada `[[tweak]]` diz exatamente a chave e o valor; `rmw restore` desfaz.
5. Imagens e licenças: créditos de tudo o que o plugin não criou.
