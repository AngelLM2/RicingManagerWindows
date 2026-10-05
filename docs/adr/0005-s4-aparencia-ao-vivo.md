# ADR-0005 — Spike S4: modo claro/escuro e cor de destaque ao vivo

- **Status:** aceito
- **Data:** 2026-10-05
- **Spike:** `spikes/s4-appearance/` (executado **dentro do Windows Sandbox**, Win11 25H2 build 26200)

## Pergunta

Um `.theme` gerado + `IThemeManager2` (COM não documentado) aplica modo escuro/claro e cor de destaque **sem logoff**, de forma confiável (critério: ≥ 99% em 200 alternâncias), e em quanto tempo? Funciona com as mitigações do S6 (para rodar no `--apply-helper`)?

## Caminho até funcionar (aprendizados)

1. `AddAndSelectTheme(path)` → `E_FAIL`.
2. `IThemeManager2` só lista temas **válidos**; um `.theme` escrito "à mão" (mínimo, em UTF-16 ou ASCII) **não entra na lista** e o `IThemeManager::ApplyTheme` legado também o recusa (`E_FAIL`).
3. ✅ Gerar o tema **a partir de um tema válido do Windows** (aqui o `aero.theme`) trocando só `DisplayName`, `SystemMode`, `AppMode` e `ColorizationColor` → o tema aparece direto na lista e é aplicado por índice (`SetCurrentTheme`) com as flags de "ignorar".

Sequência final (a mesma do Auto Dark Mode): gravar o `.theme` em `%LOCALAPPDATA%\Microsoft\Windows\Themes\` → criar um `IThemeManager2` **novo** (relê a pasta) → `Init(0)` → achar pelo `DisplayName` → `SetCurrentTheme(nullptr, idx, 1, flags, 0)` com `flags = ignorar fundo | cursores | ícones | sons | protetor | sem ampulheta`.

## Resultados

| Teste | Resultado |
|---|---|
| 200 alternâncias escuro↔claro, cor de destaque mudando a cada vez | **199/200 (99,5%)** dentro da janela de 3 s · mediana **202 ms** · p95 **260 ms** · máx. ~4 s (a falha foi só lentidão) |
| 40 alternâncias **com** ACG + extension points + carga de imagens + `MicrosoftSignedOnly` | 39/40 — a falha foi o mesmo atraso eventual > 3 s; **mitigações compatíveis** |
| Fallback só por registro (`AppsUseLightTheme`/`SystemUsesLightTheme` + broadcast `ImmersiveColorSet`) | 20/20 gravações, broadcast p95 ≈ 80 ms (a atualização visual da barra não é verificável automaticamente) |

## Decisões

1. **Módulo de aparência (M1)** usa a sequência acima. A base do `.theme` gerenciado é o **tema ativo atual** (não o `aero.theme`), para preservar cursores, sons e wallpaper do usuário — as flags de "ignorar" são uma segunda proteção.
2. Nome do arquivo/tema: `RicingManager.theme` / `DisplayName=Ricing Manager`.
3. Sucesso = registro reflete o modo e a cor; espera de até **15 s** (timeout do `--apply-helper`, doc 03 §5) — cobre os atrasos raros observados.
4. Fallbacks, em ordem: `IThemeManager::ApplyTheme` (legado, ignora flags — só se o tema não aparecer na lista) → registro + broadcast (modo apenas, com aviso de que a barra pode não atualizar até o próximo logon).
5. O `--apply-helper` roda com as **mesmas mitigações do core** (ADR-0002): validado aqui para a escrita de tema.
6. `ColorPrevalence` (destaque na barra/Iniciar e nas barras de título) é gravado direto no registro (`Themes\Personalize` e `DWM`), como o Auto Dark Mode.

## A confirmar no teste real (fim do M1)

- Que a **barra de tarefas e o Iniciar** mudam de cor visualmente (o spike verifica registro, não pixels).
- Comportamento quando o tema ativo do usuário é de terceiros/alto contraste (usar como base ou cair para o tema padrão).
