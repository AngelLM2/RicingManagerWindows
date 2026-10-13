# Tweaks (`kind = "tweak"`)

Um tweak é **dado**, não código: valores do registro do usuário (`HKEY_CURRENT_USER`) que o Ricing Manager grava quando um rice pede o plugin e **desfaz** como qualquer outra mudança (`rmw action rice.undo`, `rmw restore`). O módulo `tweaks` lembra o valor original de cada campo na primeira vez que o grava — inclusive "não existia" — e é isso que volta.

```toml
schema = 1

[plugin]
id      = "io.github.voce.extensoes"
name    = "Mostrar extensões"
version = "1.0.0"
api     = "^1.0"
kind    = ["tweak"]

[[tweak]]
key   = 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
value = "HideFileExt"
type  = "dword"
data  = 0
restart_explorer = true
```

```toml
# rice.toml
[plugins]
requires = ["io.github.voce.extensoes"]
```

## Campos

| Chave | Regras |
|---|---|
| `key` | `HKCU\...` ou `HKEY_CURRENT_USER\...` (o prefixo é opcional); `/` vale como `\`. Só chaves da lista abaixo (ou subchaves delas). Letras, dígitos, espaço e `_-.{}()&,'+`; até 255 caracteres e 16 níveis |
| `value` | o nome do valor; ASCII imprimível, até 128 |
| `type` | `dword` (inteiro de 0 a 4 294 967 295) ou `string` (até 260 caracteres) |
| `data` | o valor |
| `restart_explorer` | opcional: o Explorer só mostra a mudança depois de reiniciar (o Ricing Manager **não** reinicia o Explorer sozinho; avisa) |

## Chaves permitidas

Abaixo de `HKEY_CURRENT_USER`:

| Chave | Para quê |
|---|---|
| `Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced` | Explorer, barra de tarefas, menu Iniciar |
| `Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons` | ícones da área de trabalho |
| `Software\Microsoft\Windows\CurrentVersion\Search` | pesquisa |
| `Software\Microsoft\Windows\CurrentVersion\Feeds` | widgets/notícias |
| `Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager` | sugestões e dicas |
| `Software\Microsoft\Windows\CurrentVersion\AdvertisingInfo` | id de publicidade |
| `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` | personalização |
| `Control Panel\Desktop` | área de trabalho, animações, fontes |
| `Control Panel\Colors` | cores clássicas |
| `Control Panel\Mouse` | mouse |
| `Control Panel\Keyboard` | teclado |
| `AppEvents\Schemes` | (nenhum valor: os sons são do módulo de sons, abaixo) |

Pedidos de chaves novas: abra uma issue com o caso de uso. Uma chave entra na lista só se nada abaixo dela puder iniciar programas.

## O que é sempre recusado

- **Qualquer outra raiz** (`HKLM`, `HKCR`...) — tweaks nunca pedem administrador.
- **Componentes bloqueados**, em qualquer nível do caminho: `Run`, `RunOnce`, `RunOnceEx`, `RunServices`, `RunServicesOnce`, `StartupApproved`, `Startup`, `Classes`, `CLSID`, `Policies`, `Environment`, `Volatile Environment`, `Winlogon`, `Windows NT`, `Image File Execution Options`, `SilentProcessExit`, `AppCompatFlags`, `App Paths`, `Shell Extensions`, `ShellServiceObjectDelayLoad`, `ShellExecuteHooks`, `ShellIconOverlayIdentifiers`, `Browser Helper Objects`, `Command Processor`, `Terminal Server`, `Services`, `Scripts`, `Shell Folders`, `User Shell Folders`, `FileExts`, `Internet Settings`, `Protocols`. O bloqueio vence a lista permitida.
- **Valores que iniciam programas**, em qualquer chave: `Shell`, `Userinit`, `Load`, `Run`, `Debugger`, `AppInit_DLLs`, `SCRNSAVE.EXE`, `AutoRun`.
- **Valores que os módulos do Ricing Manager já cuidam** (o tweak brigaria com eles): `Wallpaper*`, `TileWallpaper`, `Transcoded*` (papel de parede), `AppsUseLightTheme`, `SystemUsesLightTheme` (modo claro/escuro), `EnableTransparency`, `ColorPrevalence` (aparência), `TaskbarAl`, `ShowTaskViewButton`, `SearchboxTaskbarMode` (barra de tarefas), `Start_Layout`, `Start_TrackDocs`, `Start_IrisRecommendations` (menu Iniciar), `RotatingLockScreen*`, `SubscribedContent-338387Enabled` (tela de bloqueio) e tudo em `AppEvents\Schemes` e abaixo (sons: o módulo cuida de todos os eventos). Use as seções do rice para eles.
- **Textos** com variáveis de ambiente (`%...%`), caminhos de rede ou URLs (`\\`, `//`), extensões de programas, scripts ou bibliotecas (`.exe`, `.dll`, `.ps1`, `.lnk`, `.msi`...) ou nomes de lançadores (`powershell`, `cmd`, `rundll32`, `mshta`, `regsvr32`...).

## Como é aplicado

- Os tweaks entram na mesma operação do rice: `rmw action rice.undo` desfaz o rice inteiro, tweaks incluídos.
- Só os plugins que o rice pede (`[plugins] requires`) e que estão instalados e ligados contam. Uma versão que não atende ao requisito do rice fica de fora, com aviso.
- Como as outras seções, rices **se somam**: aplicar outro rice que não pede o tweak não o desfaz. `rmw restore` volta tudo (tweaks incluídos) ao que era antes do primeiro rice.
