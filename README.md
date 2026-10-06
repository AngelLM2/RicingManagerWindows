# Ricing Manager for Windows

> 🇧🇷 [Português](#português) · 🇺🇸 [English](#english)

**Status: v1.1.0 (milestone M9 "Polish").** A per-user installer with four official rices; a settings window and the command line: apply a rice, edit it, share it as a `.rice`, import one, undo it, go back to the original Windows; light/dark mode on a schedule, global hotkeys and rules, per-monitor slideshows and video wallpapers; a custom, fully resizable taskbar with the system tray; community plugins (bar widgets, skins, tweaks, actions, rules) in a Luau sandbox. User guide: [docs/user-guide.md](docs/user-guide.md).

---

## Português

Customizador geral do Windows 11, **open source (GPLv3)** e **nativo em C++**: wallpaper estático e em vídeo por monitor, modo claro/escuro (inclusive por horário), cor de destaque, cursores, **barra de tarefas própria totalmente dimensionável** e **plugins da comunidade** — tudo descrito num arquivo `rice.toml` que você pode compartilhar.

### Princípios

- **Leve**: o que fica residente custa poucos MB; sem nada dinâmico ativo, nada fica rodando.
- **Seguro e reversível**: toda mudança tem snapshot e pode ser desfeita; "Restaurar Windows original" sempre disponível. Sem admin por padrão. O app nunca injeta código em outros processos nem altera arquivos do sistema.
- **Extensível**: widgets, skins, ajustes, ações e regras podem ser criados pela comunidade, rodando isolados e com permissões.

Planejamento completo: [docs/README.md](docs/README.md).

### Instalar

Rode o `RicingManager-<versão>-setup.exe`: sem admin, os programas vão para `%LOCALAPPDATA%\Programs\Ricing Manager` e os seus dados para `%LOCALAPPDATA%\RicingManagerWindows`. Ele já traz quatro **rices oficiais**, com arte original desenhada por código (sem nada baixado nem feito por IA):

- **Diva Stage:** palco, holofotes e equalizador em verde-água e rosa, com vídeo.
- **Tokyo Midnight:** cidade na chuva com neon, com vídeo.
- **Sakura Dawn:** manhã clara de primavera.
- **Mocha Kawaii:** pastel, pixel art e kaomoji.
- **Your Rice:** um kit com as cores e a barra do Diva Stage, para usar com a sua própria imagem.

Para desinstalar (Configurações → Apps): ele pergunta se você quer o visual do Windows de antes de volta e se mantém os seus dados; a barra do Windows sempre volta. Guia de uso: [docs/user-guide.md](docs/user-guide.md).

### A janela de configurações (v1.0)

`rmw-settings.exe` abre a janela do Ricing Manager (Fluent, claro ou escuro como o Windows, em pt-BR ou inglês):

- **Início:** o rice aplicado, o que está sendo gerenciado, "Desfazer" e "Restaurar o Windows original", e o histórico.
- **Rices:** aplicar, editar, duplicar, compartilhar (um arquivo `.rice`), importar um `.rice`, salvar o visual atual como um rice, adicionar uma pasta.
- **Aparência, Wallpapers, Cursores, Barra de tarefas e Plugins:** mudam o rice escolhido.
  - Os arquivos que você escolhe são copiados para dentro do rice, que continua inteiro e compartilhável.
  - As barras próprias têm um editor visual: arraste os widgets entre as zonas, ou use os botões.
- **Atalhos e regras:** inclusive "Abrir um app" (`launch.app`), escolhido na lista de apps instalados.
- **Diagnóstico:** processos, memória, problemas recentes e um pacote de suporte.
- **Configurações:** iniciar com o Windows, idioma, sobre.

Fechar a janela encerra o processo: ela não fica rodando.

### Usar (linha de comando, v1.0)

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
rmw pack .\tokyo-night        # faz o tokyo-night.rice para compartilhar
rmw import .\amigo.rice        # adiciona um rice compartilhado à biblioteca (conferido num processo à parte)
rmw monitors                   # monitores e suas chaves (para match = "edid:...")
rmw apps                       # apps instalados e seus ids (para launch.app)
rmw status                     # o que está gerenciado, agendamento, atalhos, modo seguro
```

Ajustes que você mudar por conta própria depois de aplicar deixam de seguir o rice (o seu ajuste fica) e não são desfeitos por engano (use `--force` para restaurar mesmo assim). O formato completo está no [doc 03 §6](docs/03-arquitetura.md).

**Atalhos e regras** ficam em `%LOCALAPPDATA%\RicingManagerWindows\config\settings.toml` (lido de novo assim que você salva):

```toml
[core]
keep_in_tray = true

[ui]
language = "pt-BR"            # ou "en"; sem isto, o idioma do Windows

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

Ações: `rice.apply`, `rice.undo`, `restore.original`, `appearance.toggle`, `appearance.dark`, `appearance.light`, `bar.toggle`, `bar.on`, `bar.off`, `wallpaper.next`, `launch.app` (com `app = "<id>"`, de `rmw apps`) — e as dos plugins, como `<id do plugin>:<ação>`. O ícone fica na bandeja, perto do relógio: um clique abre a janela; o botão direito tem "Iniciar com o Windows". Quem quiser que nada fique rodando sem atalhos, regras ou agendamento usa `keep_in_tray = false` (Configurações).

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

**Wallpaper em vídeo** — MP4 (H.264; HEVC/AV1 se a extensão da Microsoft Store estiver instalada) atrás dos ícones, por monitor, decodificado pela GPU:

```toml
[wallpaper.policy]
pause_on_fullscreen = true     # jogo ou vídeo em tela cheia: pausa
pause_on_battery = true        # na bateria ou na economia de energia: pausa
pause_when_covered = true      # janela maximizada por cima: pausa
fps_limit = 30

[[wallpaper.monitor]]
match = "primary"
kind = "video"
source = "assets/loop.mp4"
fallback_image = "assets/loop.jpg"   # o wallpaper do Windows por baixo do vídeo
fit = "fill"
```

Pausado, o vídeo não gasta CPU; depois de 2 minutos pausado, a memória dele é liberada e a imagem de reserva aparece. Se o processo do vídeo cair ou travar, ele volta sozinho em segundos — e a imagem de reserva está sempre por baixo. `rmw status` mostra o que toca em cada monitor e, se algo não toca, por quê.

**Plugins** — widgets da barra, skins, tweaks, ações e regras feitos pela comunidade. Scripts em Luau rodam isolados num processo próprio, só com as permissões que você aprovou; a barra e o core nunca executam código de plugin.

```powershell
rmw plugin install .\now-playing     # mostra o que o plugin pede e pergunta (pasta ou .rmwp)
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
.\scripts\package.ps1                       # instalador, rices oficiais, plugins .rmwp, SHA256SUMS, SBOM
```

Pacotes de fora (`.rice`, `.rmwp`) só são abertos num processo próprio, com teto de memória; nomes, tamanhos, tipos e o conteúdo de cada arquivo são conferidos antes de qualquer gravação (doc 06 §4).

---

## English

A general Windows 11 customizer, **open source (GPLv3)** and **native C++**: static and video wallpapers per monitor, light/dark mode (also scheduled), accent color, cursors, a **fully resizable custom taskbar** and **community plugins** — all described in a shareable `rice.toml` file.

### Principles

- **Lightweight**: resident parts cost a few MB; with nothing dynamic active, nothing keeps running.
- **Safe and reversible**: every change is snapshotted and can be undone; "Restore original Windows" is always available. No admin by default. The app never injects code into other processes and never modifies system files.
- **Extensible**: widgets, skins, tweaks, actions and rules can be written by the community, running isolated and with permissions.

The planning documents (in Portuguese) are in [docs/](docs/README.md).

### Install

Run `RicingManager-<version>-setup.exe`: no admin rights; the programs go to `%LOCALAPPDATA%\Programs\Ricing Manager`, your data to `%LOCALAPPDATA%\RicingManagerWindows`. It brings four **official rices**, with original art drawn from code (nothing downloaded, nothing made by AI):

- **Diva Stage:** a concert stage, spotlights and an equalizer in teal and pink, with video.
- **Tokyo Midnight:** a city in the rain with neon signs, with video.
- **Sakura Dawn:** a light spring morning.
- **Mocha Kawaii:** pastel, pixel art and kaomoji.
- **Your Rice:** a kit with Diva Stage's colors and bar, for your own picture.

Uninstalling (Settings → Apps) asks whether to bring back the Windows look you had before and whether to keep your data; Windows' taskbar always comes back. User guide: [docs/user-guide.md](docs/user-guide.md).

### The settings window (v1.0)

`rmw-settings.exe` opens Ricing Manager's window (Fluent, light or dark like Windows, in English or Portuguese). Home shows the applied rice, what is managed, Undo, Restore the original Windows and the history; Rices applies, edits, duplicates, shares (a `.rice` file), imports a `.rice` and saves your current look as a rice. Appearance, Wallpapers, Cursors, Taskbar (with a visual editor of your own bars: drag widgets between zones, or use the buttons) and Plugins change the chosen rice; the files you pick are copied into it, so it stays whole and shareable. Hotkeys and rules include `Open an app` (`launch.app`, picked from the installed apps); Diagnostics shows processes, memory, recent problems and makes a support bundle. Closing the window ends the process.

### Use (command line, v1.0)

A rice is a folder with a `rice.toml` (example above) and its files.

```powershell
rmw validate .\tokyo-night     # check a rice (errors with line and column)
rmw apply .\tokyo-night        # apply it; everything it changes is recorded
rmw history                    # what was done
rmw restore --snapshot 3       # back to before operation 3
rmw restore --original         # back to the Windows you had before the first apply
rmw export .\my-look           # save the current look as a rice
rmw pack .\tokyo-night        # make tokyo-night.rice to share
rmw import .\friend.rice       # add a shared rice to your library (checked in a process of its own)
rmw monitors                   # monitors and their keys (for match = "edid:...")
rmw apps                       # installed apps and their ids (for launch.app)
rmw status                     # what is managed, schedule, hotkeys, safe mode
```

Settings you change yourself after applying stop following the rice (your change stays) and are not undone by mistake (`--force` restores them anyway).

**Hotkeys and rules** live in `%LOCALAPPDATA%\RicingManagerWindows\config\settings.toml` (example above; re-read as soon as you save it). Actions: `rice.apply`, `rice.undo`, `restore.original`, `appearance.toggle`, `appearance.dark`, `appearance.light`, `bar.toggle`, `bar.on`, `bar.off`, `wallpaper.next`, `launch.app` (with `app = "<id>"`, from `rmw apps`) — and plugins' own, as `<plugin id>:<action>`. Its icon stays in the tray, next to the clock: a click opens the window; the right click has "Start with Windows". For nothing to keep running without hotkeys, rules or a schedule, use `keep_in_tray = false` (Settings).

```powershell
rmw autostart on               # start with Windows (off until you ask)
rmw settings reload            # re-read settings.toml
```

If the app stops unexpectedly 3 starts in a row (or `Shift` is held at sign-in), it opens in **safe mode**: nothing is applied automatically and "Restore original Windows" is at hand.

**A taskbar of your own** — as many as you like, on any edge and monitor, any size: see the `[[bar]]` example above (widgets: `start`, `windows`, `desktops`, `tray` — the system tray, with the apps' icons, clicks, menus and tips —, `clock`, `media`, `cpu`, `memory`, `network`, `volume`, `battery`, `notifications`). Saving `rice.toml` updates it live. If anything goes wrong, **Ctrl+Alt+Win+T** turns the bar off and brings Windows' taskbar back (`rmw action bar.on` turns it on again); if the bar hangs or crashes, Windows' taskbar is back in under a second.

**Live wallpapers** — a picture, a video or a color with effects (brightness, contrast, saturation, hue, blur, parallax that follows the mouse, a slow zoom, a vignette, a video's speed) and particle layers (sparkles, stars, snow, rain, petals, fireflies, magic dust, music notes, hearts, bubbles), drawn behind the desktop icons: `effects = { ... }` and `particles = [ ... ]` in a `[[wallpaper.monitor]]`. A still picture costs nothing once drawn. **Wallpaper Engine**: the settings list the wallpapers Wallpaper Engine has on this computer (videos play as they are; scenes use their picture); a rice that uses one is for this computer only (`shareable = false`) and is never packed.

**Video wallpapers** — MP4 or WebM (H.264; HEVC/AV1 with the Microsoft Store extension installed) behind the desktop icons, per monitor, decoded by the GPU: `kind = "video"` with a `fallback_image` in a `[[wallpaper.monitor]]` (example above), and a `[wallpaper.policy]` to pause on fullscreen apps, on battery or when the desktop is covered, with an FPS limit. Paused, it costs no CPU; paused for 2 minutes, its memory is freed and the fallback image shows. If the video process crashes or hangs it comes back by itself in seconds, and the fallback image is always underneath. `rmw status` shows what plays on each monitor and, when something does not, why.

**Plugins** — bar widgets, skins, tweaks, actions and rules made by the community. Luau scripts run isolated in a process of their own, with only the permissions you approved; the bar and the core never run plugin code. `rmw plugin install <folder or .rmwp>` shows what a plugin asks for and asks you; `rmw plugin new widget <folder>` starts one (with the API's types for your editor) and `rmw plugin dev <folder>` runs it from the folder, reloading on save. A rice asks for plugins in `[plugins] requires` and puts widgets in a `[[bar]]` by their id. Eight examples are in [`plugins/examples`](plugins/examples); the plugin author's guide (in Portuguese) is in [docs/plugins](docs/plugins/README.md).

### Build

Requirements: Windows 11, Visual Studio 2022+ or Build Tools with *Desktop development with C++*, plus the *MSVC Spectre-mitigated libs*, *C++ Clang tools for Windows* and *vcpkg* components.

```powershell
.\scripts\build.ps1 -Preset debug -Test
.\scripts\build.ps1 -Preset release
.\scripts\build.ps1 -Preset asan -Test
.\scripts\sandbox.ps1 -Preset debug
.\scripts\package.ps1          # installer, official rices, .rmwp plugins, SHA256SUMS, SBOM
```

Packages from outside (`.rice`, `.rmwp`) are only opened in a process of their own with a memory cap; every file's name, size, type and content is checked before anything is written (docs/06 §4).

## License

[GPL-3.0-or-later](LICENSE).
