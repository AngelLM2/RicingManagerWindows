# ADR-0002 — Spike S6: mitigações de processo do core

- **Status:** aceito (parte "CRT estático × `slint_cpp.dll`" transferida para o spike S2)
- **Data:** 2026-10-05
- **Spike:** `spikes/s6-mitigations/`

## Pergunta

Quais políticas de mitigação de processo (doc 06 §5) o `rmw-core` pode ativar sem quebrar o que ele faz: COM do Shell (`IDesktopWallpaper`), registro, janela oculta com loop de mensagens, `SystemParametersInfo`, pastas conhecidas?

## Método

Executável descartável que aplica um conjunto de políticas via `SetProcessMitigationPolicy` **depois** de inicializar o COM (STA) e então executa as operações de **leitura** do core. Rodado na máquina de referência (Win11 Pro 25H2, build 26200).

| Políticas aplicadas | Resultado |
|---|---|
| nenhuma | ✅ tudo OK |
| ACG (`ProhibitDynamicCode`) | ✅ |
| *Extension points* desabilitados (AppInit DLLs, hooks globais, LSPs, IMEs legadas) | ✅ |
| Carga de imagens (`NoRemoteImages`, `NoLowMandatoryLabelImages`, `PreferSystem32Images`) | ✅ |
| Assinatura `MicrosoftSignedOnly` | ✅ |
| Assinatura `MitigationOptIn` | ✅ |
| **ACG + extension points + carga de imagens + MicrosoftSignedOnly** | ✅ tudo OK (`IDesktopWallpaper` listou o monitor e leu o wallpaper) |

## Decisão

1. O `rmw-core` (processo principal) aplica, logo após inicializar o COM: **ACG, extension points desabilitados, política de carga de imagens e `MicrosoftSignedOnly`** — além de CFG/CET (link) e *strict handle checks* (`hardenProcess`, já ativo).
   - Efeito colateral desejado: DLLs de terceiros (hooks globais, overlays, "injetores") **não conseguem carregar** dentro do core.
2. **Chave para desligar** (modo seguro e variável `RMW_DISABLE_MITIGATIONS=1`), registrada no log, para diagnosticar incompatibilidades em máquinas de usuários.
3. O `--apply-helper` (que **escreve**: `SetWallpaper`, `SPI_SETCURSORS`, `IThemeManager2`) só recebe as mesmas políticas depois que o spike **S4** validar as operações de escrita no Windows Sandbox.
4. A convivência do CRT estático com `slint_cpp.dll` é verificada no **S2** (precisa do SDK do Slint).

## Riscos

- Operações não testadas aqui (escrita, WinRT de energia/sessão) podem exigir relaxar uma política — por isso a chave para desligar e a reavaliação no S4/M2.
- `MicrosoftSignedOnly` impede qualquer DLL não Microsoft no core: o core **nunca** poderá hospedar plugins nativos (já era a regra — doc 12 §3).
