# ADR-0014 — M7: a interface (rmw-settings)

- **Status:** aceito
- **Data:** 2026-10-05
- **Contexto:** doc 04 (M7), doc 07 (interface), ADR-0007 (S2: Slint com renderizador por software), ADR-0011 (plugins)

## Decisões

1. **Um processo próprio, aberto quando o usuário quer.** O `rmw-settings.exe` é um cliente do core; fechar a janela encerra o processo e libera tudo o que ele usava.
   - **Renderizador:** o Slint usa o renderizador por software (`SLINT_BACKEND=winit-software`, ADR-0007), fixado pelo próprio app.
   - **Uma instância:** um *mutex* e um evento com nome. Abrir de novo traz a janela que já existe para a frente. A segunda instância sai na hora com `TerminateProcess`, sem descarregar DLLs: na Sandbox, uma segunda instância que só retornava do `wWinMain` ficava presa ao sair.
   - **Tamanho da janela:** cabe no monitor em qualquer escala (a 200%, 1180×720 lógicos seriam 2360×1440 pixels).

2. **A thread da janela nunca espera o core.** As chamadas vão para uma thread com o próprio `CoreClient`, e as respostas voltam pela fila do Slint (`invoke_from_event_loop`). O `CoreClient` virou a biblioteca `rmw::client`, a mesma da CLI. Operações que mudam o sistema (aplicar, restaurar, desfazer, exportar) mostram o estado de "ocupado" e esperam enquanto o core está ocupado.

3. **Editar um rice é editar o modelo e gravar o `rice.toml`:**
   - **Gravador:** `domain::writeRice` grava a partir do `domain::Rice`. A ida e volta é garantida: ler, gravar e ler de novo dá o mesmo texto. Isso é conferido nos testes de unidade e, para qualquer entrada, pelo alvo de fuzzing `rice_toml`. O mesmo vale para o `settings.toml` (`writeSettings`).
   - **Comentários:** não são mantidos. O arquivo ganha a linha `# Written by Ricing Manager…`, e a interface avisa antes quando o rice foi escrito à mão, sugerindo duplicá-lo.
   - **Gravação contínua, aplicação explícita:** cada mudança é gravada na hora, mas aplicar é um botão, para o histórico continuar significando algo. Se o rice editado é o aplicado, a barra acompanha cada gravação sozinha (o core já recarregava a barra ao salvar o `rice.toml`, M3).
   - **Arquivos escolhidos entram no rice:** imagens, vídeos, pastas de apresentação e esquemas de cursor são copiados para `assets/` (`importAsset`). Assim o rice continua inteiro e compartilhável. Links são recusados, conteúdo igual é reaproveitado e nome repetido com conteúdo diferente ganha outro nome.
   - **Nenhum arquivo ilegível:** um tipo de wallpaper que precisa de arquivo só é gravado depois que o arquivo é escolhido, e uma cor de paleta que o rice não tem não é gravada.

4. **A biblioteca de rices:** é `<data>\rices` mais as pastas que o usuário adicionou e o rice aplicado, guardadas em `config\settings-app.json`. O doc 07 previa um `library.db`; uma lista de pastas basta por enquanto.

5. **Miniaturas do jeito do Explorer.** `IShellItemImageFactory` numa thread STA própria gera a mesma miniatura que o Explorer mostra, inclusive de vídeo e de cursor. O cache é em memória (até 256 imagens). O processo da interface é o único que lê as imagens do usuário; o core nunca decodifica imagens.

6. **Textos traduzidos:**
   - **No Slint:** os textos ficam em inglês com `@tr`. O pt-BR fica em `apps/settings/lang/pt_BR.tsv`, e o `scripts/translations.ps1` gera o `.po` que o Slint embute no executável. O script falha quando falta tradução ou sobra uma que ninguém usa.
   - **Do lado C++:** os textos (nomes de monitor, barras, ações, permissões, estados) vêm de funções do *global* `Words` do Slint. O C++ nunca escreve um texto da interface.
   - **Escolha do idioma:** fica em `[ui] language` no `settings.toml` (`en`, `pt-BR`; ausente = o idioma do Windows). Vale para a janela, na hora, e para o menu da bandeja do core.

7. **Acessível:**
   - **Controles próprios:** os componentes padrão do Slint já são acessíveis; os próprios (navegação, chips, mapa de monitores, zonas da barra, cartões) têm papel, nome e ação padrão.
   - **Sem arrastar:** tudo o que se arrasta também se faz com botões (mover o widget antes/depois, para outra zona).
   - **Prova:** a suíte `settings` da Sandbox comanda a janela só pela UI Automation, o mesmo que o Narrador lê e aciona.

8. **`launch.app`, adiado no M4.** Abre um app instalado pelo ID da lista "Todos os apps" do Iniciar (`shell:AppsFolder`), nunca por caminho ou com argumentos. Se o ID não está na lista, nada abre.
   - **Disponível como:** ação do core, `apps.list`, `rmw apps`, `rmw action launch.app --app <id>` e permissão de plugin.
   - **Na interface:** escolhido numa lista com busca, nos atalhos e regras.

9. **`status.get` diz qual rice está aplicado** e qual veio antes (o que o "Desfazer" traz de volta).

## Adiado

- **Galeria de wallpapers** (biblioteca virtualizada com os de todos os rices) e **arrastar um wallpaper para um monitor:** hoje clicar no monitor escolhe a regra, e "Escolher…" escolhe o arquivo.
- **Cache de miniaturas em disco, seletor de cor completo** (hoje: amostras e `#RRGGBB`) e **fundo mica na janela** (o renderizador por software pinta por cima; ADR-0007).
- **Configurações de plugin fora de um rice** (no `settings.toml`): hoje cada rice configura seus plugins.

## Números (Sandbox, build debug)

| Medida | Resultado |
|---|---|
| Suíte `settings` | 26/26 (UI Automation de ponta a ponta, em inglês e em pt-BR) |
| Abrir a janela, a frio / já aberta | 700–970 ms / 63–71 ms (meta: < 600 ms com a janela já aberta) |
| Memória privada depois de passar pelas 10 páginas | 12,3 MB (meta: < 120 MB) |
| Páginas e controles achados pelo nome (UIA) | 10/10 páginas, 43 controles com nome |
| Editar → escuro → aplicar | `AppsUseLightTheme` 1 → 0 no registro |
| `apps.list` na Sandbox | 38 apps; `launch.app` abre o escolhido e recusa um caminho (`cmd.exe`) |
| A 200% de escala | a janela cabe no monitor |
| Fechar a janela | o processo termina; segunda instância traz a primeira para a frente |
| Regressão (system, bar, plugins, tray, video) | 54/54, 28/28, 43/43, 43/43, 26/26 |
| Testes de unidade | 319 |
