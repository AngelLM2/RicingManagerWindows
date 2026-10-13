# ADR-0026 — O Windows da pessoa, nunca os padrões (v1.9)

- **Data:** 2026-10-13
- **Estado:** aceita
- **Contexto:** o quarto teste real do dono (doc 17): "a restauração volta para as configurações padrão do Windows; isso deve ser a última instância, porque quebra um sistema que alguém já personalizou"; "modificações destroem o PC".

## Decisões

1. **Os padrões do Windows nunca entram no lugar de algo da pessoa.** Isso vale para o tema dela que sumiu, para um original tirado sobre um rice, para a foto e a cópia perdidas e para a base de um tema gerenciado ilegível. Em cada caso, o que se tem da pessoa volta (o visual guardado, o nome do tema, a foto mostrada agora), e o log diz o que faltou.
2. **Aplicar um tema muda só o visual.** O gerenciador de temas legado aplica o arquivo inteiro. Em volta de toda aplicação de tema, legado incluído, o app guarda e devolve:
   - os ponteiros;
   - os sons (o esquema e cada evento);
   - a foto de cada monitor e o ajuste;
   - a proteção de tela;
   - os ícones da área de trabalho e as figuras deles;
   - as cores recentes.

   Isto foi medido na suíte `fidelity` com o legado forçado (`RMW_TEST_THEME_LEGACY=1`).
3. **O original de um campo é o valor da pessoa logo antes de o app tomá-lo.** Ao aplicar, um campo que o app não segura tem o original atualizado com o valor de agora. Pode ser o de antes da primeira aplicação, o que a pessoa escolheu depois de uma restauração, ou uma mudança dela que o tirou do app. "Restaurar o original" deixa de trazer o Windows de semanas atrás.
4. **A cópia do original vive fora da pasta de dados** (`<dados>.original.json`), junto com o que o app segura naquele momento. Com a pasta perdida, o próximo core adota essa cópia, e os campos que o rice ainda ocupa voltam a ser do app. Assim, uma restauração traz o Windows da pessoa, e não o rice. O desinstalador só apaga essa cópia depois de uma restauração bem-sucedida.
5. **Reaplicar só o que o evento reinicia no Windows.** No reinício do Explorer, isso é a barra de tarefas e o acento do tema; num monitor que se acomoda, é o papel de parede. Em qualquer outro módulo, o que difere do que o app definiu é mudança da pessoa: o campo sai do app e fica como está.
6. **Ponteiros menores que 32 px são desenhados menores**, e não pedidos ao Windows. O Windows aceita e devolve o valor 16 ou 24, mas não está documentado e não há como confirmar que ele os desenha menores: na Sandbox, a medida do ponteiro mostrado não mudou nem para 64. Desenhar menor funciona de qualquer jeito. A tela de cada imagem continua a mesma e o ponto de clique acompanha a escala.
7. **"Desfazer" é voltar ao snapshot da última operação** (`restoreHistory`), não reaplicar o rice anterior.

## Consequências

- Um "original" pode mudar depois da primeira aplicação; isso é intencional, porque segue as escolhas da pessoa. Uma mudança feita fora do app enquanto um rice está aplicado é dela e não é desfeita.
- A cópia do original fica em `%LOCALAPPDATA%` mesmo depois de desinstalar sem restaurar. É ela que permite, numa instalação futura, trazer o Windows da pessoa de volta.
- O Destaque (área de trabalho) continua com fidelidade parcial: volta como foto, com o aviso já existente.
