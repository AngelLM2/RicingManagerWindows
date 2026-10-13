# 17 — Plano v1.9: o quarto teste real — o Windows da pessoa, nunca os padrões

> Origem: o teste do dono com a v1.8.1 (2026-10-13). Ele relatou: "a sincronização, a abertura e o fechamento do app" com problemas; "falha em voltar o sistema todo pro normal, ele volta pras configs padrões do Windows: isso deve ser feito em última instância, porque assim a gente quebra o sistema já personalizado de alguém"; modificações aplicadas que "muitas vezes bugam"; "não temos menos que 32 de tamanho de mouse"; "modificações destroem o PC, o app não fecha ou fecha bugado". Pediu para analisar e depurar cada funcionalidade, com no máximo dois agentes, e testar o que fosse preciso.
>
> Os logs do PC dele não existiam mais: a pasta de dados foi apagada ao desinstalar. Tudo abaixo vem do código, de duas revisões feitas por agentes que só leram o código e de uma suíte nova da Sandbox.

## A suíte `fidelity`: um Windows que já é de alguém

As outras suítes começam de uma Sandbox nos padrões do Windows, onde voltar aos padrões não se distingue de restaurar. A `fidelity` primeiro faz da Sandbox o Windows de uma pessoa:

- o tema dela (`Meu tema.theme`, aplicado pelo próprio Windows), a foto dela e o modo escuro com o acento dela;
- "sem sons", e os ponteiros Windows Black em 64 px;
- uma proteção de tela, os ícones da área de trabalho e a barra de tarefas à esquerda.

Depois, cada caminho tem que deixar tudo isso como estava, comparando o registro e o que se vê. Os caminhos testados:

- um rice só de cores, pelo caminho normal e pelo legado;
- os rices oficiais um atrás do outro;
- o tema da pessoa apagado;
- fechar e abrir o app com um rice aplicado;
- a pessoa mudando algo com o rice aplicado;
- "sem sons" e o Explorer reiniciando;
- "Desfazer";
- a pasta de dados perdida.

**Primeira rodada, no código da v1.8.1: 10 falhas.**

| # | O que a suíte viu | Causa |
|---|---|---|
| F1 | Um rice só de cores, pelo gerenciador de temas legado (o caminho de um tema que "não aparece", e sempre a terceira tentativa): **"Sem sons" virou os sons do Windows**, em todos os eventos, e não voltou | O legado aplica o arquivo de tema inteiro; só os ponteiros e as cores recentes eram guardados e devolvidos |
| F2 | O tema da pessoa apagado e o legado: **a foto dela virou a do Windows** (`img19.jpg`) | A base do tema gerenciado vira o tema do Windows quando o tema atual não pode ser lido |
| F3 | Restaurar com o tema da pessoa apagado: o `dark.theme` do Windows nomeado como o tema dela | Um tema que sumiu era trocado pelo do Windows para o modo |
| F4 | Pasta de dados perdida com um rice aplicado: a restauração **aplicou o `aero.theme`** | O "original" era tirado de novo sobre o rice; um tema nosso como original virava o do Windows |
| — | Os caminhos normais (cores, rices oficiais, fechar e abrir) | Nada além do pedido mudou ✔ |

## Correções

### Restaurar (o pedido central)

1. **Um tema aplicado muda só o visual.** Toda aplicação de tema, legado incluído, guarda e devolve:
   - os sons (o esquema e cada evento);
   - a foto de cada monitor e o ajuste (uma foto em todos volta do jeito das Configurações);
   - a proteção de tela;
   - os ícones da área de trabalho e as figuras deles.

   Antes, só os ponteiros e as cores recentes eram devolvidos.
2. **Nunca o tema do Windows no lugar do da pessoa.** Um tema dela que sumiu volta como visual (modo e acento guardados) e com o nome dele, como ela tinha. Um original que era tema nosso fica nosso, com as cores tiradas. Depois de restaurar, os temas "Ricing Manager" saem da lista das Configurações.
3. **O original acompanha a pessoa.** Um campo que o app não segura guarda o valor dela, seja o de antes da primeira aplicação, seja o que ela escolheu depois (de uma restauração, ou de uma mudança dela que o tirou do app). Esse é o original a trazer de volta; antes, a restauração trazia o Windows de semanas atrás. As regras e as páginas do slideshow também guardam originais.
4. **A cópia do original mora ao lado da pasta de dados** (`RicingManagerWindows.original.json`), com o que o app segura. A pasta perdida com um rice aplicado (desinstalado sem restaurar, dados removidos) não transforma mais o rice, nem os padrões do Windows, no "original". Uma instalação nova restaura o Windows da pessoa exatamente.
5. **O papel de parede nunca volta como a foto do Windows.** Quando a foto e a cópia sumiram, fica a foto mostrada, e o log diz por quê. Arquivos .tif, .webp, .heic, .avif e .jxr contam como foto. Depois de um rice com o slideshow do Windows, a foto da pessoa volta e o slideshow para.
6. **O Destaque da tela de bloqueio volta ligado**, sem uma foto parada por cima. Uma foto definida desligava o Destaque.

### O app parando de desfazer o que a pessoa muda

7. **Reaplicar só o que o evento reinicia.** Ao reiniciar, o Explorer reinicia a barra de tarefas e pode perder o acento do tema. Quando um monitor se acomoda, o Windows mexe no papel de parede. Fora disso, o que difere do que o app definiu é da pessoa e fica: a v1.8 punha de volta um som ou ícone mudado à mão a cada reinício do Explorer.
8. **Ícones:** um programa instalado depois de uma aplicação não conta mais como mudança da pessoa. Antes, a restauração deixava todos os ícones do rice. Um atalho que não pode ser escrito mantém o ícone, em vez de desfazer o rice inteiro.

### Aplicar melhor

9. **"Sem sons" cala todos os eventos**, como o "Sem sons" do Windows: os do Windows além dos dezoito e os dos apps. A restauração devolve cada um.
10. **Os rices oficiais não forçam mais o ponteiro em 32**: o ponteiro maior de qualquer pessoa encolhia. O mesmo vale para os rices pessoais.
11. **Ponteiros de 16 e 24 px.** O Windows desenha ponteiros em 32 px no mínimo. Abaixo disso, os ponteiros do rice são desenhados menores na mesma tela de 32, com o ponto de clique junto, e o Windows fica em 32. Imagens de 32 bits são redesenhadas; PNG ou poucas cores ficam como estão.
12. **"Desfazer" volta ao momento antes da última mudança**, com o estado do core (barras, papel de parede vivo, agenda). Antes, aplicava o rice anterior de novo, e o que ele não define ficava como o último deixou. Desfazer duas vezes continua alternando.
13. **A janela diz o que manteve:** "Restaurado. Mantido como você mudou: Papel de parede", em vez de "Restaurado" quando algo foi deixado.

### Desinstalar

14. O resultado da restauração decide o que é apagado.
15. Sem restaurar, a barra de tarefas do Windows volta como era: `rmw restore --original --module taskbar` tira o auto-ocultar que era para a nossa barra.
16. Sem restaurar e com "remover dados", ficam o `store` (as fotos, ponteiros e sons que o Windows ainda usa) e a cópia do original. Antes, o Windows ficava com setas padrão, silêncio e ícones em branco.
17. Restaurando e removendo os dados, a cópia do original também sai.

### Abertura, fechamento e sincronização

Um agente separado trabalha nisto (ver a seção de resultados):

- a barra é encerrada de verdade ao sair, e não morta, para a faixa reservada voltar;
- `rmw shutdown` não abre o app inteiro só para fechar;
- reabrir logo depois de fechar funciona;
- o supervisor não mata workers ao acordar do sono;
- a ordem de saída na hora de sair do Windows;
- a janela não apaga edições feitas no `settings.toml` por fora;
- teclas presas.

## Limites conhecidos

- **Destaque da área de trabalho:** volta como foto parada, porque o estado dele não tem uma forma documentada de voltar.
- **Destaque da tela de bloqueio:** volta ligado, mas a Sandbox não tem rede para confirmar que o Windows volta a trocar as fotos. Isso se confere no PC real.
- **Um atalho com o ícone mudado à mão** tira todo o campo de ícones de apps do app. Os outros ícones do rice ficam até a pessoa trocar o rice.
- **Ponteiros abaixo de 32 px** só funcionam com os ponteiros do próprio rice. Os do Windows não são redesenhados.
