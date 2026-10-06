# ADR-0016 — M9: polimento depois do primeiro teste real

- **Status:** aceito
- **Data:** 2026-10-06
- **Contexto:** doc 14 (o plano depois da v1.0), o teste do dono no PC real com a v1.0.0

## Decisões

1. **Os destaques da barra seguem os cantos dela.** No teste, um hover quadrado aparecia numa barra arredondada.
   - **O que a barra passa:** `bar-item-radius` = `max(4, min(raio − 4, 12))` e `bar-accent` (a cor de destaque do Windows, um pouco mais clara numa barra escura). Ela os passa a todo widget embutido e a todo widget de plugin que declare `item-radius`/`accent`.
   - **Exceção:** um plugin que já tem uma propriedade `accent` configurável fica com a dele.
   - **Recorte:** a barra recorta o que os widgets desenham (`clip`).
   - **Animação:** os destaques aparecem e somem em 120 ms; os indicadores (janela ativa, área de trabalho atual) deslizam em 160 ms.

2. **Ícones do Windows 11 e ícones dos apps nítidos.**
   - **Status:** volume, rede, bateria, CPU, memória e notificações mostram glifos da **Segoe Fluent Icons**. O volume e a bateria mudam de glifo com o nível e com o carregamento; o texto (porcentagem, taxa) aparece quando o rice pede (`label = true/false`; o volume vem sem texto, como no Windows).
   - **Apps:** o ícone de uma janela vem do Shell (`IShellItemImageFactory`, 48 px), pelo AppUserModelID da janela (apps empacotados, Explorer) ou pelo executável, uma vez por app. Antes era o ícone pequeno da própria janela (16 px) esticado. O da janela fica só como reserva.
   - **Relógio:** pode mostrar a data embaixo da hora (`date = "%d/%m/%Y"`).

3. **O ícone fica na bandeja (pedido do dono).**
   - **Padrão:** `keep_in_tray` passa a valer `true` quando o `settings.toml` não diz nada. Quem quiser o modo "zero residente" desliga em Configurações (`keep_in_tray = false`).
   - **Menu:** o clique esquerdo abre o Ricing Manager (o item padrão do menu, em negrito). O clique direito mostra o menu, que agora tem "Iniciar com o Windows", marcável.
   - **Visível:** no Windows 11, o core marca o próprio ícone como visível uma vez (`HKCU\Control Panel\NotifyIconSettings\…\IsPromoted`, a entrada do nosso executável), e um arquivo em `config\` lembra que foi feito. Se o usuário esconder o ícone de novo nas Configurações do Windows, a escolha dele fica.

4. **A janela de configurações sem becos sem saída.**
   - **Sem "Escolha um rice para mudar":** Aparência, Wallpapers, Cursores e Barra de tarefas abrem o editor do rice aplicado. Sem rice aplicado, o visual atual do Windows é salvo como "Meu visual" e é ele que muda. Se a pessoa escolher outro rice enquanto isso, a escolha dela vale.
   - **Rices:** virou uma galeria (cartões 16:9 com zoom suave ao passar o mouse, "aplicado" em destaque, ações em botões de ícone com nome para o Narrador, duplo clique aplica).
   - **Início:** mostra o rice aplicado grande, com "Personalizar" e "Trocar de rice", e uma faixa com a biblioteca, em que um clique aplica.
   - **Navegação:** dividida em PERSONALIZAR e MAIS.
   - **"Escrito à mão":** o aviso não aparece mais para um rice criado por `rmw export`, porque a nota do exportador é nossa.

5. **Repintura completa da janela de configurações.** No PC real, a janela ficou transparente depois de sair da tela. A janela agora observa as mensagens de mover (no fim de um arraste), restaurar, mudar de monitor e de DPI. Em cada uma, alterna uma camada invisível do tamanho da janela, o que obriga o renderizador por software a redesenhar tudo, e não só o que ele sabia que tinha mudado. Na Sandbox, a janela sai e volta da tela sem diferença de pixels.

## Números (Sandbox)

| Medida | Resultado |
|---|---|
| Suíte `bar` | 28/28 com os widgets novos |
| Suíte `system` | 56/56 (bandeja por padrão, "zero residente" quando pedido, ícone promovido) |
| Suíte `settings` | 30/30 (galeria, Início, editor sem beco; fora da tela e minimizada: diferença de 0,0 e 0,3 de 255) |
| Suítes `tray`, `plugins`, `video` | 43/43, 43/43, 26/26 |
| Testes de unidade | 351 |
