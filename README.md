# Ricing Manager for Windows

> 🇧🇷 [Português](#português) · 🇺🇸 [English](#english)

**Status: alpha (v0.4.0, milestone M4 "Plugins").** The engine works from the command line: apply a rice, undo it, go back to the original Windows; light/dark mode on a schedule, global hotkeys and rules, per-monitor slideshows; a custom, fully resizable taskbar; community plugins (bar widgets, skins, tweaks, actions, rules) in a Luau sandbox. No settings window yet (see [docs/04-mvp.md](docs/04-mvp.md)).

---

## Português

Customizador geral do Windows 11, **open source (GPLv3)** e **nativo em C++**: wallpaper estático e em vídeo por monitor, modo claro/escuro (inclusive por horário), cor de destaque, cursores, **barra de tarefas própria totalmente dimensionável** e **plugins da comunidade** — tudo descrito num arquivo `rice.toml` que você pode compartilhar.

### Princípios

- **Leve**: o que fica residente custa poucos MB; sem nada dinâmico ativo, nada fica rodando.
- **Seguro e reversível**: toda mudança tem snapshot e pode ser desfeita; "Restaurar Windows original" sempre disponível. Sem admin por padrão. O app nunca injeta código em outros processos nem altera arquivos do sistema.
- **Extensível**: widgets, skins, ajustes, ações e regras podem ser criados pela comunidade, rodando isolados e com permissões.

Planejamento completo: [docs/README.md](docs/README.md).

### Usar (linha de comando, v0.4)

Um rice é uma pasta com um `rice.toml` e seus arquivos:

```toml
schema = 1

[meta]
name = "Tokyo Night"

[appearance]
mode = "schedule"            # ou "dark" / "light"
accent = "#7aa2f7"

[appearance.schedule]
light_at = "07:00"
dark_at = "19:00"

[taskbar]
search = "icon"

[[wallpaper.monitor]]
match = "primary"
kind = "slideshow"           # uma pasta; troca a cada `interval` segundos
source = "assets/slides"
interval = 900

[[wallpaper.monitor]]
match = "all"
source = "assets/mountains.jpg"
```

```powershell
rmw validate .\tokyo-night     # confere o rice (erros com linha e coluna)
rmw apply .\tokyo-night        # aplica; tudo o que muda fica guardado
rmw history                    # o que foi feito
rmw restore --snapshot 3       # volta a antes da operação 3
rmw restore --original         # volta ao Windows de antes do primeiro apply
rmw export .\meu-visual        # salva o visual atual como um rice
rmw monitors                   # monitores e suas chaves (para match = "edid:...")
rmw status                     # o que está gerenciado, agendamento, atalhos, modo seguro
```

Ajustes que você mudar por conta própria depois de aplicar deixam de seguir o rice (o seu ajuste fica) e não são desfeitos por engano (use `--force` para restaurar mesmo assim). O formato completo está no [doc 03 §6](docs/03-arquitetura.md).

**Atalhos e regras** ficam em `%LOCALAPPDATA%\RicingManagerWindows\config\settings.toml` (lido de novo assim que você salva):

```toml
[core]
keep_in_tray = true

[[hotkey]]
keys = "Win+Alt+D"
action = "appearance.toggle"

[[hotkey]]
keys = "Ctrl+Win+Alt+N"
action = "rice.apply"
rice = 'C:\Rices\night'

[[rule]]
when = "battery"             # "time" (com at = "HH:MM"), "battery", "ac", "monitor-connected"
action = "appearance.dark"
```

Ações: `rice.apply`, `rice.undo`, `restore.original`, `appearance.toggle`, `appearance.dark`, `appearance.light`, `bar.toggle`, `bar.on`, `bar.off`, `wallpaper.next` — e as dos plugins, como `<id do plugin>:<ação>`. Com atalhos, regras ou agendamento, o app fica residente (ícone na bandeja); sem nada disso, ele aplica e sai.

```powershell
rmw autostart on               # iniciar com o Windows (desligado até você pedir)
rmw settings reload            # relê o settings.toml
```

Se o app parar inesperadamente 3 vezes seguidas ao iniciar (ou com `Shift` pressionado no logon), ele abre no **modo seguro**: nada é aplicado automaticamente e "Restaurar Windows original" fica à mão.

**Barra de tarefas própria** — quantas quiser, em qualquer borda e monitor, do tamanho que quiser:

```toml
[[bar]]
monitor = "primary"            # ou "all", "left", "edid:<chave>"
position = "bottom"            # top, bottom, left, right
mode = "reserve"               # ou "float" (por cima, sem reservar espaço)
thickness = 44
length = "100%"                # ou em pixels: 900
margin = [0, 0]                # [vertical, horizontal] ou [cima, direita, baixo, esquerda]
radius = 0
background = { kind = "acrylic", tint = "palette.night", opacity = 0.4 }   # solid, transparent, acrylic, mica
autohide = false
hide_native_taskbar = true
left = ["start", "desktops", "windows"]
center = ["clock"]
right = ["tray", "media", "cpu", "memory", "network", "volume", "battery", "notifications",
         { id = "clock", format = "%d/%m/%Y" }]
```

O `tray` é a bandeja do sistema: os ícones dos apps (Discord, Steam, OneDrive…), com clique, menu e dica; mostra os que o Windows mostra na barra dele e guarda o resto atrás de uma seta (`{ id = "tray", show = "all" }` mostra todos). Salvar o `rice.toml` atualiza a barra na hora. Se algo der errado, **Ctrl+Alt+Win+T** desliga a barra e traz a do Windows de volta (`rmw action bar.on` religa). Se a barra travar ou cair, a do Windows volta em menos de um segundo.

**Plugins** — widgets da barra, skins, tweaks, ações e regras feitos pela comunidade. Scripts em Luau rodam isolados num processo próprio, só com as permissões que você aprovou; a barra e o core nunca executam código de plugin.

```powershell
rmw plugin install .\now-playing     # mostra o que o plugin pede e pergunta
rmw plugin list                      # instalados, aprovados, ligados, parados
rmw plugin new widget .\meu-widget   # um plugin novo, com os tipos para o editor
rmw plugin dev .\meu-widget          # roda da pasta, recarrega ao salvar, mostra o log
```

```toml
[plugins]
requires = ["io.github.angellm2.now-playing"]

[[bar]]
right = ["io.github.angellm2.now-playing", "clock"]
```

Oito exemplos em [`plugins/examples`](plugins/examples) (tocando agora, clima, gráfico de CPU, lista de janelas, bandeja compacta, modo foco, relógio, bateria); guia para quem escreve plugins em [docs/plugins](docs/plugins/README.md).

### Compilar

Requisitos: Windows 11, Visual Studio 2022+ ou Build Tools com *Desktop development with C++*, componentes *MSVC Spectre-mitigated libs*, *C++ Clang tools for Windows* e *vcpkg*.

```powershell
.\scripts\build.ps1 -Preset debug -Test     # compila e roda os testes
.\scripts\build.ps1 -Preset release         # build de distribuição
.\scripts\build.ps1 -Preset asan -Test      # testes com AddressSanitizer
.\scripts\sandbox.ps1 -Preset debug         # testes dentro do Windows Sandbox
```

---

## English

A general Windows 11 customizer, **open source (GPLv3)** and **native C++**: static and video wallpapers per monitor, light/dark mode (also scheduled), accent color, cursors, a **fully resizable custom taskbar** and **community plugins** — all described in a shareable `rice.toml` file.

### Principles

- **Lightweight**: resident parts cost a few MB; with nothing dynamic active, nothing keeps running.
- **Safe and reversible**: every change is snapshotted and can be undone; "Restore original Windows" is always available. No admin by default. The app never injects code into other processes and never modifies system files.
- **Extensible**: widgets, skins, tweaks, actions and rules can be written by the community, running isolated and with permissions.

The planning documents (in Portuguese) are in [docs/](docs/README.md).

### Use (command line, v0.4)

A rice is a folder with a `rice.toml` (example above) and its files.

```powershell
rmw validate .\tokyo-night     # check a rice (errors with line and column)
rmw apply .\tokyo-night        # apply it; everything it changes is recorded
rmw history                    # what was done
rmw restore --snapshot 3       # back to before operation 3
rmw restore --original         # back to the Windows you had before the first apply
rmw export .\my-look           # save the current look as a rice
rmw monitors                   # monitors and their keys (for match = "edid:...")
rmw status                     # what is managed, schedule, hotkeys, safe mode
```

Settings you change yourself after applying stop following the rice (your change stays) and are not undone by mistake (`--force` restores them anyway).

**Hotkeys and rules** live in `%LOCALAPPDATA%\RicingManagerWindows\config\settings.toml` (example above; re-read as soon as you save it). Actions: `rice.apply`, `rice.undo`, `restore.original`, `appearance.toggle`, `appearance.dark`, `appearance.light`, `bar.toggle`, `bar.on`, `bar.off`, `wallpaper.next` — and plugins' own, as `<plugin id>:<action>`. With hotkeys, rules or a schedule the app stays resident (tray icon); otherwise it applies and exits.

```powershell
rmw autostart on               # start with Windows (off until you ask)
rmw settings reload            # re-read settings.toml
```

If the app stops unexpectedly 3 starts in a row (or `Shift` is held at sign-in), it opens in **safe mode**: nothing is applied automatically and "Restore original Windows" is at hand.

**A taskbar of your own** — as many as you like, on any edge and monitor, any size: see the `[[bar]]` example above (widgets: `start`, `windows`, `desktops`, `tray` — the system tray, with the apps' icons, clicks, menus and tips —, `clock`, `media`, `cpu`, `memory`, `network`, `volume`, `battery`, `notifications`). Saving `rice.toml` updates it live. If anything goes wrong, **Ctrl+Alt+Win+T** turns the bar off and brings Windows' taskbar back (`rmw action bar.on` turns it on again); if the bar hangs or crashes, Windows' taskbar is back in under a second.

**Plugins** — bar widgets, skins, tweaks, actions and rules made by the community. Luau scripts run isolated in a process of their own, with only the permissions you approved; the bar and the core never run plugin code. `rmw plugin install <folder>` shows what a plugin asks for and asks you; `rmw plugin new widget <folder>` starts one (with the API's types for your editor) and `rmw plugin dev <folder>` runs it from the folder, reloading on save. A rice asks for plugins in `[plugins] requires` and puts widgets in a `[[bar]]` by their id. Eight examples are in [`plugins/examples`](plugins/examples); the plugin author's guide (in Portuguese) is in [docs/plugins](docs/plugins/README.md).

### Build

Requirements: Windows 11, Visual Studio 2022+ or Build Tools with *Desktop development with C++*, plus the *MSVC Spectre-mitigated libs*, *C++ Clang tools for Windows* and *vcpkg* components.

```powershell
.\scripts\build.ps1 -Preset debug -Test
.\scripts\build.ps1 -Preset release
.\scripts\build.ps1 -Preset asan -Test
.\scripts\sandbox.ps1 -Preset debug
```

## License

[GPL-3.0-or-later](LICENSE).
