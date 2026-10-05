# ADR-0011 — M4: plugins

- **Status:** aceito
- **Data:** 2026-10-05
- **Contexto:** doc 12 (plugins), doc 04 (M4), ADR-0002 (mitigações), ADR-0008 (S7: widgets e plugins), ADR-0010 (barra própria)

## Decisões

1. **Três níveis, um só formato.** Um plugin é uma pasta com `plugin.toml` (`schema = 1`). P0 (`bar.skin`) e P1 (`tweak`) são dados lidos pelo core; P2 (`bar.widget`, `action`, `rule`) traz um script Luau que roda só no `rmw-plugin-host`. Um manifesto pode ter vários tipos; cada tipo exige a sua parte (`[widget]`, `[skin]`, `[[tweak]]`, `[[action]]`) e uma parte sem o tipo é erro. Chaves desconhecidas são erro: um erro de digitação não passa calado num arquivo que pede permissões.

2. **Um host para todos os plugins, uma VM por plugin.** O S7 mediu ≈ 0,35 MB por VM Luau; um host por plugin custaria um processo inteiro cada (a opção de isolar um plugin suspeito num host próprio fica para quando houver um caso). O isolamento entre plugins fica nas VMs (`luaL_sandbox` + `luaL_sandboxthread`, alocador com teto de 16 MB, *interrupt* de 50 ms por callback) e na culpa: o host escreve num registro compartilhado (`--blame`, herdado) qual plugin está rodando antes de cada callback. Se o host cai ou trava, o core sabe quem estava rodando; duas quedas em 5 min param esse plugin, e os outros voltam com o host.

3. **O Job do host não usa restrições de UI.** Qualquer `JOB_OBJECT_UILIMIT_*` (mesmo só `HANDLES`) esconde do processo as janelas de outros processos: `EnumWindows` passou a devolver só as do próprio Job, e `windows.list` ficou vazio na Sandbox. O host não tem janelas nem precisa de mensagens de outras: o que ele perde sem essas restrições é pouco, e o resto do confinamento continua (teto de memória, `ActiveProcessLimit = 1` — sem processos filhos —, mitigações estritas: sem código dinâmico, só DLLs assinadas pela Microsoft, sem pontos de extensão). O Luau roda sem JIT, então o ACG não custa nada.

4. **Permissões conferidas três vezes.** O manifesto declara; o usuário aprova (`rmw plugin install`/`approve`); o host dá ao script a interseção dos dois e confere cada chamada. A barra confere de novo cada valor contra os tipos do manifesto e as imagens contra a pasta do plugin; o core confere que uma ação pedida pelo host é uma ação de plugin. Uma atualização que pede mais para até nova aprovação. `media`, `windows.list`, `window.close` e rede são destacados.

5. **A barra compila o `.slint` do plugin, depois de um validador nosso.** É a opção A do S7. O interpretador do Slint não tem gancho para o que carrega, então o arquivo passa antes por um *tokenizer* nosso que o reescreve token a token, sem comentários: só imports de `std-widgets.slint`, `@image-url` só com caminhos simples da pasta do plugin (reescritos para absolutos), sem `Timer`, janelas, menus ou `iteration-count`, repetidores ≤ 256, limites de tamanho e aninhamento. Os nomes de nível superior ganham o prefixo `Rmwp<n>-`, e nomes `Rmw*` são proibidos: um plugin não alcança a barra nem outro plugin. Cada token fica **na linha em que estava**, para que um erro do compilador do Slint aponte a linha do arquivo do plugin.

6. **Um widget que o Slint recusa não derruba os outros.** Quando a barra com todos os widgets não compila, ela compila cada widget de plugin sozinho, deixa de fora os recusados (com a mensagem do Slint em `widget.slint:<linha>:<coluna>`) e mostra o resto. Só se os que passaram sozinhos falharem juntos a barra aparece sem nenhum widget de plugin. O custo só existe quando há erro.

7. **Ponte barra ↔ host: JSON enquadrado num pipe da barra.** A barra serve o pipe (nome dado pelo core em `--bridge`) e aceita só o `rmw-plugin-host.exe` da mesma pasta (caminho do processo do outro lado). Mensagens: `hello`, `instances` (onde cada widget aparece, com a configuração), `set` (valores), `event` (callbacks) e `action` (ações de janela). O host agrupa os valores (≥ 250 ms por lugar; a resposta a um clique vai na hora); a barra guarda os últimos valores por lugar e por plugin, e os reaplica quando o host reinicia ou o plugin recarrega.

8. **Ações de janela são decididas pela barra, na hora do clique.** `window.activate` (trazer para a frente ou minimizar, como um botão da barra de tarefas) depende de qual janela está na frente *no momento do clique*. O host vê os eventos de janela com atraso de segundos depois que outro processo muda o foco (medido na Sandbox), então a ação vai para a barra, que decide e só age em janelas que uma barra de tarefas mostraria. `windows.list` é, além dos eventos (num thread com `GetMessage` próprio), lido a cada 1 s.

9. **O `interrupt` do Luau também para padrões de string.** O teste do S7 previa que `string.find` com um padrão patológico prenderia o callback dentro do C; o Luau 0.729 chama o *interrupt* também ali, e o callback é interrompido como um laço infinito. O vigia do processo (3 s) continua para o que escapar.

10. **Tweaks são um módulo do motor.** O módulo `tweaks` recebe campos `"<chave>|<valor>"` com `{type, data}` ou `null` (não existia). O motor passou a lembrar o original de um campo na primeira vez que o grava (`rememberOriginals`), também para campos que só um rice posterior pede; assim `restore` devolve exatamente o que havia. Allowlist de chaves de `HKCU`, block-list de componentes (`Run`, `Classes`, `Policies`, `Winlogon`...) que vence a allowlist, valores que os módulos já cuidam proibidos (papel de parede, claro/escuro, barra de tarefas), textos sem `%`, caminhos de rede, extensões de código ou lançadores.

11. **Desenvolvimento.** `rmw plugin dev <pasta>` registra a pasta (roda mesmo sem estar num rice), vigia os arquivos e recarrega script e widget em < 1 s, mostrando o log do host e da barra. `rmw plugin new` gera o modelo de `plugins/template` (a CLI o embute no build: uma fonte só) com `rmw.d.luau`, `.luaurc` e `.vscode/` para o luau-lsp. Os testes carregam o `rmw.d.luau` no analisador do próprio Luau e conferem todos os scripts de exemplo em modo estrito, com o solver padrão do luau-lsp (o novo, na 0.729, ainda perde `any` em refinamentos).

12. **Mesma API para nós.** Os sete exemplos (`plugins/examples`) são a prova do doc 12 §7: tocando agora, relógio e bateria refazem widgets embutidos; lista de janelas usa structs, ícones de janela e barra vertical; clima usa rede e `storage`; modo foco é uma ação com um rice dentro do plugin (`rice.apply` só aceita rices da pasta do próprio plugin).

## Adiado

- **Instalar de `.rmwp`**: no M8, com o leitor seguro de pacotes (libzip). Até lá, `install` aceita pastas; `pack` já gera o `.rmwp`.
- **`launch.app`**: precisa de uma lista de apps instalados por ID (sem linha de comando); fica para quando a interface (M7) puder mostrar e confirmar.
- **Capa do que está tocando** (`media` com imagem): fora; exigiria passar bytes de imagem do host para a barra.
- **Configuração de plugin no `settings.toml`** (sem rice): hoje as configurações vêm do rice e da barra; a página gerada do esquema chega com a interface (M7).

## Números (Sandbox, build debug)

| Medida | Resultado |
|---|---|
| Suíte `plugins` | 43/43 |
| Host com 6 plugins | 6,8–7,4 MB privados |
| Barra com 6 widgets de plugin | 10,6–10,9 MB privados |
| Salvar → widget recarregado | 221–317 ms (inclui 150 ms de espera para juntar gravações) |
| Salvar → script recarregado | 237–241 ms |
| Bomba de memória / laço infinito / padrão patológico | parados com a causa; barra e outros plugins seguem |
| Host morto | reiniciado; widgets voltam com os últimos valores |
