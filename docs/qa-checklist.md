# Checklist de QA — v1.0

Os critérios de aceite da v1.0 (doc 04, "Definition of MVP"), como cada um é conferido e o que
fica para o teste no PC real. **Sandbox** = Windows Sandbox (`scripts\sandbox.ps1 -Suite <nome>`),
onde tudo o que muda o Windows é testado. O PC do desenvolvimento nunca é alterado.

## Antes de publicar

```powershell
.\scripts\format.ps1                                 # sem diferenças
.\scripts\tidy.ps1 -Path apps; .\scripts\tidy.ps1 -Path libs; .\scripts\tidy.ps1 -Path tests
.\scripts\build.ps1 -Preset debug -Test              # testes de unidade
.\scripts\build.ps1 -Preset asan -Test               # com AddressSanitizer
.\scripts\fuzz.ps1 -Seconds 120                      # todos os alvos, sem crash
.\scripts\package.ps1                                # build release + instalador + rices + plugins + SBOM
foreach ($s in 'unit','system','bar','plugins','tray','video','settings','packages') { .\scripts\sandbox.ps1 -Preset debug -Suite $s }
.\scripts\sandbox.ps1 -Preset release -Suite installer
```

## Critérios

| Critério | Como se confere | Onde |
|---|---|---|
| Instala e desinstala sem admin; desinstalar oferece restaurar o Windows original e sempre devolve a barra nativa | `installer`: arquivos em `%LOCALAPPDATA%`, entrada só no HKCU, `restore --original` ao desinstalar, barra do Windows visível depois, com e sem restaurar | Sandbox |
| Rice completo aplicado em < 3 s típicos e desfeito sem resíduos | `installer`: os 4 rices oficiais aplicados e medidos; `system`: desfazer/restaurar comparando o estado | Sandbox + **PC real** |
| Barra própria em qualquer tamanho/posição, com bandeja; barra nativa restaurada em qualquer falha | `bar` (bordas, tamanhos, matar a barra), `tray` | Sandbox + **PC real** (bandeja com Discord, Steam, OneDrive, GPU) |
| Plugins de terceiros isolados: malicioso/travado não afeta barra, core nem sistema | `plugins`: scripts hostis (laço infinito, memória, rede, chamadas proibidas) | Sandbox |
| Orçamentos do doc 01 (Private Bytes) | `settings` (12 MB), `bar` (12 MB), `video` (46 MB na Sandbox) | Sandbox + **PC real** (vídeo na GPU AMD, meta 90 MB) |
| Nenhum crash conhecido; workers se recuperam sozinhos | `system` (supervisor), `video` (trava e morte do worker) | Sandbox |
| Windows 11 24H2/25H2 | Sandbox = o Windows do PC; o resto: melhor esforço | **PC real** |
| Multi-monitor com DPI misto e reconexão de monitor | — (a Sandbox tem um monitor) | **PC real, por último, com o secundário** |
| Pacotes maliciosos rejeitados sem crash (zip slip, ADS, symlink, zip bomb, imagem gigante, TOML profundo, `.ani` malformado, tweak em chave proibida) | `packages` (zip slip, `\`, absoluto, ADS, nomes de dispositivo, 8.3, bomba, script num rice, exe renomeado, imagem gigante, duplicados) + testes de unidade (symlink, criptografia, métodos, CRC, junction) + fuzzing (`package`, `rice_toml`, `cursor_file`, `tweaks`) | Sandbox + CI |
| CI verde com ASan e fuzzing | `ci.yml` (debug, release, asan, fuzz-smoke) | CI (depois do push autorizado) |

## No PC real (o dono, no fim)

1. Instalar com o `RicingManager-<versão>-setup.exe`; abrir pelo menu Iniciar.
2. Aplicar cada rice oficial; conferir vídeo (Diva Stage, Tokyo Midnight), cursores, barra.
3. Bandeja com os apps de verdade (Discord, Steam, OneDrive, o painel da GPU): clique, menu, dica.
4. Jogo ou vídeo em tela cheia: o wallpaper de vídeo pausa; volta depois.
5. Memória do `rmw-wallpaper` tocando vídeo 4K/60 na GPU AMD (meta: 90 MB).
6. Importar um `.rice` e instalar um `.rmwp` pela janela.
7. Atalhos e regras do dia a dia; modo seguro (Shift no logon).
8. **Por último:** conectar o monitor secundário — barra e wallpaper por monitor, DPI misto, desconectar e reconectar.
9. Desinstalar: escolher restaurar; conferir que o Windows voltou ao que era.
