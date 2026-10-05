# Diário de progresso

> Atualizado a cada tarefa. Uma nova sessão retoma daqui ("continua").

## Como o trabalho está organizado no Git (local, sem push)

- `main` — intocada até o dono do projeto decidir mesclar.
- `develop` — branch de integração. Cada tarefa é feita numa branch própria (`plan/…`, `fix/…`, `chore/…`, `feat/…`, `spike/…`) e mesclada em `develop` com `--no-ff`.
- Para ver tudo o que foi feito: `git log --oneline --graph develop`.

## Como compilar e testar

```powershell
.\scripts\build.ps1 -Preset debug -Test      # build + 47 testes
.\scripts\build.ps1 -Preset release -Test    # LTO, PDB, Spectre
.\scripts\build.ps1 -Preset asan -Test       # AddressSanitizer
.\scripts\sandbox.ps1 -Preset debug          # testes dentro do Windows Sandbox
```

Particularidade desta máquina: o perfil `C:\Users\user'\` tem apóstrofo, o que quebra ports do vcpkg baseados em meson. O `build.ps1` detecta isso e põe os caches do vcpkg em `build\.vcpkg\`.

## Estado atual

| Item | Estado |
|---|---|
| Planejamento (docs 01–13) | ✅ aprovado ("OK" em 2026-10-05), sem push |
| Fase 0 — F0.1 build verde | ✅ |
| Fase 0 — F0.2/F0.3 build system | ✅ presets, Ninja, warnings, hardening, triplet endurecido, vcpkg |
| Fase 0 — F0.4/F0.5 estrutura + `libs/base` | ✅ `legacy/`, `libs/base`, `libs/platform`, `libs/storage`, `apps/cli` |
| Fase 0 — F0.6 testes + Windows Sandbox | ✅ GoogleTest (47 testes), `scripts/sandbox.ps1` |
| Fase 0 — F0.7 CI | ✅ workflows prontos (rodam só após push) |
| Fase 0 — F0.8/F0.9 comunidade + ADR-0001 | ✅ |
| S6 — mitigações | ✅ ADR-0002: ACG + extension points + carga de imagens + MicrosoftSignedOnly compatíveis |
| S3 — barra nativa | ✅ ADR-0003: ajustes ao vivo (exceto alinhamento: reinício do Explorer); esconder/restaurar ok |
| S5 — monitores/snapshot | ✅ ADR-0004: chave de identidade em níveis; restaurar wallpaper/slideshow ok |
| S4 — aparência | ✅ ADR-0005: 199/200 alternâncias, ~200 ms; compatível com mitigações |
| Spikes S1, S2, S7, S8 | 🔄 próximo: S1 (vídeo) |
| S0 (CI) | ⏸ precisa de push |

## Registro

| Data | Passo | Branch | Resultado |
|---|---|---|---|
| 2026-10-05 | 0 — planejamento versionado | `plan/mvp` | docs 01–13 + este diário |
| 2026-10-05 | F0.1 — build verde | `fix/green-build` | protótipo compila e linka |
| 2026-10-05 | F0.2–F0.9 — fundação | `chore/build-system` | build moderno, libs base/platform/storage, CLI, 47 testes, CI, docs de comunidade, ADR-0001; binário com ASLR/DEP/CFG/CET/EH-continuation e só `KERNEL32.dll` importada |
| 2026-10-05 | S6 | `spike/s6-mitigations` | ADR-0002 |
| 2026-10-05 | S3 (Sandbox) | `spike/s3-taskbar` | ADR-0003 |
| 2026-10-05 | S5 (host + Sandbox) | `spike/s5-monitors-snapshot` | ADR-0004 |
| 2026-10-05 | S4 (Sandbox) | `spike/s4-appearance` | ADR-0005 |

## Pendências conhecidas

- **S0 (CI)** só pode rodar com push — aguardando autorização ("OK com push").
- `luau` entra no `vcpkg.json` no spike S7.
