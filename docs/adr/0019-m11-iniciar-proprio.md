# ADR-0019 — M11: um Iniciar nosso

- **Status:** aceito
- **Data:** 2026-10-06
- **Contexto:** doc 14 §M11. O dono aprovou "um Iniciar nosso" em vez de modificar o do Windows: mais leve, e sem injetar nada no `StartMenuExperienceHost` (como o Windhawk faz). Não injetamos código em outros processos.

## Decisões

1. **No rice: `[start]`.**
   - **`menu`:** `"ours"` (o nosso, aberto pelo botão Iniciar da nossa barra) ou `"windows"`.
   - **`pinned`:** até 24 ids do `rmw apps`.
   - **Opções do Iniciar do Windows, que as Configurações dele permitem:**
     - `layout`: `Start_Layout`;
     - `recommendations`: `Start_TrackDocs` e `Start_IrisRecommendations`.
   - **Quem aplica as opções do Windows:** o módulo da barra nativa, com captura e desfazer como os outros campos.

2. **O menu mora no `rmw-bar`.**
   - **Janela:** uma janela própria (`MenuWindow`), em camadas com alpha por pixel, nas cores da primeira barra do rice. Diferente da barra, ela recebe o foco (para digitar) e some quando perde o foco, como o Iniciar do Windows. O clique no botão que a fez sumir não a reabre.
   - **Teclado:** passa para o Slint (`WM_KEYDOWN` para as teclas com nome, `WM_CHAR` para o texto, inclusive pares substitutos, Ctrl+letra para copiar e colar).
   - **Conteúdo:**
     - todos os apps (a mesma lista do `launch.app`, que só abre apps instalados);
     - busca instantânea: começa com, depois começa uma palavra, depois contém, sem diferença de maiúsculas nem de acentos;
     - fixados;
     - pastas do usuário e Configurações;
     - energia: bloquear, suspender, sair, reiniciar, desligar. Desligar e reiniciar pedem o privilégio de desligamento, que o usuário tem mas que fica desligado no processo até ser pedido.
   - **Ícones:** os fixados quando o menu é feito; os outros, alguns por vez, enquanto ele está aberto.
   - **Idioma:** o Slint do menu é compilado em tempo de execução e não tem catálogo de tradução. As palavras vêm do C++ em inglês ou português, conforme o idioma que o core escolheu (o do `settings.toml`, senão o do Windows) e passou no plano.

3. **A tecla Win, opcional (`settings.toml` → `[start] win_key`).**
   - **Gancho:** um gancho de teclado de baixo nível (`WH_KEYBOARD_LL`) numa thread própria do `rmw-bar`, como o PowerToys. A barra ocupada nunca atrasa a digitação, e o Windows não remove o gancho por demora.
   - **O que ele bloqueia:** só a soltura de um Win **sozinho**. No lugar dela, envia uma tecla sem uso (`0xE8`, a "tecla de máscara" do AutoHotkey) e a própria soltura do Win. Assim o Windows vê um "atalho" de Win e não abre o Iniciar dele, e o nosso abre.
   - **De onde ele envia:** as teclas saem do laço de mensagens da thread do gancho, logo depois, e não de dentro do gancho. Um `SendInput` dentro do gancho faz os ganchos rodarem de novo, o nosso inclusive, enquanto ele ainda está ocupado.
   - **Atalhos:** Win+qualquer tecla, Win+L e Ctrl+Alt+Del continuam do Windows.
   - **Teclas que o gancho mesmo envia:** são marcadas (`dwExtraInfo`) e ignoradas. As de outros programas (inclusive testes e scripts do AutoHotkey) contam como do usuário.
   - **Se o `rmw-bar` cai:** o gancho vai junto e a tecla Win volta a ser do Windows sozinha.
   - **Foco:** o processo que acabou de enviar entrada (as teclas do gancho, ou o clique na nossa barra) pode trazer a própria janela para a frente. Não pegamos a entrada do thread em frente (`AttachThreadInput`), que pode travar quando esse thread está ocupado.

4. **Uma barra que reserva espaço não desliza** (corrige o M10). Na Sandbox, com os efeitos de animação ligados, uma barra reservada que deslizava para dentro da própria faixa deixou de responder e foi desligada pelo supervisor: a lógica de barras de aplicativo do shell e a janela em movimento se alimentavam. Só as barras flutuantes e as de auto-ocultar deslizam.

## Números

| Medida | Resultado |
|---|---|
| Abrir o menu (clique no botão até a janela visível, Sandbox) | 61 ms na primeira vez, 30 ms depois (aceite: < 150 ms); o primeiro quadro é desenhado sem ser visto quando o menu é feito |
| Busca em 300 apps | 3 ms (aceite: < 30 ms) |
| Testes sem janela | o menu compila e desenha (aberto, buscando, todos os apps, energia); lugar ao lado da barra (no meio, à esquerda, ao lado de uma barra lateral); tecla Win (sozinha, com atalhos, com repetição) |
