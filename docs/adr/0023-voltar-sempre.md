# ADR-0023 — Voltar ao Windows original sempre volta o que dá; Sair é sair

- **Status:** aceito (v1.7)
- **Data:** 2026-10-06
- **Contexto:** no segundo teste real, "voltar ao Windows normal dps de aplicar alguns ricing buga, e não volta" e "funções na barra de segundo plano tbm n funcionam". O log do core no PC do dono mostra, em toda tentativa (pela bandeja e pelas configurações), `restore failed: … apply wallpaper: SetWallpaper: The system cannot find the file specified; undoing`.

## O que aconteceu no PC do dono

1. Numa desinstalação com "apagar os dados", o core ainda rodava: a janela de configurações aberta o tinha iniciado de novo logo depois do `rmw shutdown` do desinstalador (o cliente reconecta e, sem core, inicia um). O `state.db` (aberto pelo core) e os logs abertos ficaram; o armazém (`store\`), os rices e o resto foram apagados.
2. A instalação seguinte usou esse `state.db`. O "Windows original" dele (capturado em 2026-10-06 00:30, depois de uma reinstalação anterior) tinha o papel de parede numa cópia do armazém que não existia mais, e um tema nosso (`RicingManager-2.theme`) como tema do usuário.
3. Restaurar era tudo-ou-nada: o papel de parede falhava e o motor desfazia o que já tinha voltado. Nada voltava, nunca.
4. "Sair" na bandeja fechava o core, e a janela aberta o iniciava de novo em 3 s: a bandeja voltava como se nada tivesse acontecido.

## Decisões

1. **Restaurar continua depois de um módulo que falha.** O motor (`run`, operação `restore`) não desfaz: o módulo que falhou fica como está, continua "nosso" (o próximo restaurar tenta de novo) e entra em `report.failed`; os outros voltam. Só quando nenhum volta o restaurar falha (e nada é registrado). **Aplicar continua tudo-ou-nada**: um rice pela metade é pior que nenhum.
2. **O que é nosso sai mesmo se o Windows não voltar inteiro:** ao restaurar o original, barra, *tiling*, wallpaper vivo, aparência das janelas e ícones do PC são desligados antes de olhar o resultado do motor.
3. **Um arquivo que sumiu nunca impede a volta:**
   - papel de parede: o arquivo do usuário, senão a nossa cópia (`copy`, como cursores e sons desde a v1.6), senão a imagem do próprio Windows (`Web\Wallpaper\Windows\img0.jpg`), senão aquele monitor fica como está; nunca uma falha;
   - tela de bloqueio: o arquivo, senão a cópia, senão a imagem fica como está.
4. **O papel de parede guarda o caminho do usuário** (e a cópia ao lado). O Windows volta a apontar para o arquivo dele, não para o armazém, que vai embora com uma desinstalação que apaga os dados. As **imagens recentes** do Configurações (`Explorer\Wallpapers\BackgroundHistoryPath0..4`), onde o Windows põe cada imagem de rice, também voltam como eram (campo `history`, que segue as imagens como `wallPaperValue`); a Sandbox passou a comparar essa chave também. Uma imagem que o próprio usuário pôs depois de um rice fica na tela; da lista de recentes ela sai ao restaurar.
5. **Um original que não pode ser do usuário** (`forOriginal`, novo no `IModuleLogic`): um tema nosso guardado como original vira o tema do Windows para o modo guardado (`aero.theme` claro, `dark.theme` escuro); um tema do usuário que sumiu também.
6. **A bandeja e as configurações dizem o resultado:** "O Windows original voltou" ou "O resto voltou. Não voltou: …", com o motivo.
7. **Sair é sair.** O core fecha a janela de configurações (um evento nomeado que a janela espera) quando sai pela bandeja ou por `rmw shutdown`, e a janela, fechando, não inicia o core de novo. `rmw shutdown --wait` também para um core que voltou enquanto espera e, depois de 30 s, encerra o que sobrou da instalação e devolve a barra do Windows; o desinstalador avisa se algum arquivo ficou.

## Consequências

- Um restaurar pode terminar "pela metade" de propósito; o relatório diz o quê, e o histórico guarda o passo (dá para voltar).
- No PC do dono, a v1.7 restaura o que dá desse `state.db` antigo: o papel de parede vira a imagem do Windows, o tema vira o do Windows para o modo, o resto volta como era.
- Aceite na Sandbox: vários rices em sequência (oficiais, pessoais e "Meu visual") e restaurar deixam o registro como antes; um original com a imagem e a cópia apagadas restaura o resto e põe a imagem do Windows; com a janela de configurações aberta, `rmw shutdown` a fecha e nenhum core volta; depois de desinstalar apagando os dados, nenhum valor do usuário aponta para o armazém.
