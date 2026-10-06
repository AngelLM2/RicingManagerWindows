# ADR-0018 — M10: estilos de barra, dock e animações

- **Status:** aceito
- **Data:** 2026-10-06
- **Contexto:** doc 14 §M10. O dono pediu "mais barras de tarefa, talvez algumas animações" e uma interface mais intuitiva.

## Decisões

1. **Estilos são skins que vêm com o app.**
   - **No rice:** `style = "windows" | "dock" | "island" | "status" | "side"` num `[[bar]]`. O estilo é uma barra inteira (`domain::barStyle`), aplicada como um skin, com esta ordem de precedência:
     1. as chaves que a própria barra escreve;
     2. as do skin dela;
     3. as do estilo.
   - **Gravação:** uma barra só com estilo é gravada só com o estilo.
   - **Quem aplica:** o core aplica o estilo ao montar o plano da barra (`bar::planFromRice`), e o módulo da barra nativa o considera, porque todos os estilos escondem a barra nativa.
   - **Na janela de configurações:** escolher um estilo põe a barra inteira no estilo. O editor mostra os valores efetivos, e o que a pessoa muda passa a ser dela. "Do meu jeito" converte o estilo em chaves próprias.

2. **`length = "auto"`: do tamanho do que mostra.**
   - **Medida:** a barra gerada expõe `content-length`, o espaço que as três zonas precisam sem que o centro deixe de ficar centralizado, mais os cantos.
   - **Acompanhamento:** a janela segue esse valor quando ele muda (um app abre, a bandeja cresce).
   - **Uso:** é o que faz o Dock e a Ilha.

3. **Widgets novos.**
   - **`dock`:**
     - mostra os apps fixados (ids do `rmw apps`, só apps que o menu Iniciar lista) e depois cada app com janela aberta, um ícone por app (as janelas agrupadas pela identidade do app: AppUserModelID ou executável);
     - clique: abre o app fixado sem janela, traz a janela para a frente ou passa para a próxima janela do app;
     - um ponto indica que o app está rodando (com a cor de destaque para o que está na frente);
     - ícones crescem sob o mouse (e um pouco os vizinhos) **sem mudar a largura da célula**: o dock não muda de tamanho enquanto o mouse passa.
   - **`spacer` e `separator`.**
   - **`text`:** um texto e/ou um glifo da Segoe Fluent Icons, numa cor do rice; pode abrir um app instalado.
   - **Fixar apps no dock pela janela de configurações:** a mesma lista de apps do `launch.app`. Um dock que vem do estilo é copiado para a zona da barra antes de mudar.

4. **Animações.**
   - **Ao aparecer:** a barra desliza da sua borda (200 ms, desacelerando).
   - **Auto-ocultar:** desliza para fora e de volta; se o mouse volta durante a saída, ela volta.
   - **Efeitos de animação desligados no Windows** (Acessibilidade > Efeitos visuais): nada desliza.
   - **Destaques e indicador:** os do M9 continuam (fade e deslize).
   - **Troca de rice:** as barras novas deslizam para dentro.

## Números (Sandbox)

| Medida | Resultado |
|---|---|
| Os 5 estilos aplicados | cada um no lugar certo (reserva, borda, centro, tamanho) |
| Dock | cresce com os apps abertos; 13,9 MB privados; 0,00% de CPU em 10 s parado |
| Testes da barra sem janela | `content-length` cresce ~50 px por app; widgets novos compilam e desenham na horizontal e na vertical |
