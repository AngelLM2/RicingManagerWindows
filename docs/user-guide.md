# Ricing Manager for Windows — user guide

Ricing Manager changes how Windows looks — wallpaper (pictures, slideshows, video), light/dark mode
and colors, cursors, Windows' taskbar or bars of your own — as a **rice**: one folder (or one
`.rice` file) with the whole look, that you apply, undo and share. Everything it changes can be put
back as it was.

> Em português: as seções seguem a mesma ordem da janela do app; o app inteiro fala português
> (Configurações → Idioma).

## Install and uninstall

- **Install:** run `RicingManager-<version>-setup.exe`. It needs no administrator rights: the
  programs go to `%LOCALAPPDATA%\Programs\Ricing Manager`, your data to
  `%LOCALAPPDATA%\RicingManagerWindows`. The official rices are added to your library (you can
  untick that).
- **Update:** run the new setup. It stops Ricing Manager, replaces the programs and starts it again
  if it started with Windows. Your rices and settings stay.
- **Uninstall** (Settings → Apps, or the Start menu): it asks whether to **restore the Windows look
  you had before** (wallpaper, colors, cursors, taskbar) and whether to **keep your data**. Windows'
  own taskbar always comes back.

## First steps

1. Open **Ricing Manager** from the Start menu.
2. **Rices** lists your library. Pick one — *Diva Stage*, *Tokyo Midnight*, *Sakura Dawn*,
   *Mocha Kawaii* — and press **Apply**. It takes a second or two.
3. Don't like it? **Home → Undo** goes back to how things were before that rice.
   **Restore the original Windows** goes back to how Windows was before you first used Ricing
   Manager.

The first time you apply anything, Ricing Manager notes how your Windows looks, so it can always go
back.

## The window

| Page | What it does |
|---|---|
| **Home** | The applied rice, what Ricing Manager manages now, Undo, Restore, the history |
| **Rices** | Apply, edit, duplicate, share (**Share…** makes a `.rice` file), import (**Import a .rice…**), save your current look as a rice |
| **Appearance** | Light/dark (or on a schedule), accent color, transparency |
| **Wallpapers** | Per monitor: a picture, a slideshow folder, a video or a color; how it fits; when videos pause |
| **Cursors** | A cursor scheme (a folder with `install.inf`, or one file per cursor) and its size |
| **Taskbar** | Windows' taskbar (alignment, search, auto-hide) — or **bars of your own**: any edge, any size, widgets you drag between left, center and right |
| **Plugins** | Install (**from a file…** for `.rmwp`, or from a folder), see what each asks for, approve, turn on/off, use in the rice, configure |
| **Hotkeys and rules** | A hotkey or a rule (time of day, battery, a monitor plugged in) runs an action: apply a rice, toggle dark mode, next wallpaper, open an app… |
| **Diagnostics** | What runs and how much memory it takes, recent problems, **Make a support bundle** |
| **Settings** | Language, start with Windows, the tray icon |

Changes to a rice are saved as you make them; **Apply** puts them on the screen. When the rice you
edit is the applied one, its bar follows each change by itself.

## Rices

- **Import a `.rice`** someone shared: Rices → *Import a .rice…*. Every file in it is checked
  first, in a process of its own: names, sizes, types, and the pictures and cursors themselves. A
  package that tries anything odd (files outside its folder, links, programs, a "zip bomb") is
  refused and nothing of it stays.
- **Share yours:** *Share…* on its card makes a `.rice` file (a zip) with the rice and the files it
  uses.
- **Your Rice** is a kit: the Diva Stage colors, cursors and bar with no picture of ours.
  *Duplicate* it, then choose your own wallpaper in **Wallpapers → Choose…**. The picture is copied
  into your copy and stays on your PC.
- A rice is plain text (`rice.toml`) plus its files; the format is in the
  [README](../README.md) and in `docs/03-arquitetura.md`.

## Video wallpapers

A video plays behind the desktop icons and pauses by itself when it is not seen (a full-screen app
or game, the desktop covered, the screen off or locked) and on battery, where the rice's still
picture shows instead. After two minutes paused, its memory is given back.

## Bars of your own

A rice can hide Windows' taskbar and show its own bars, with Start, your windows, the system tray,
the clock, media, volume, network, battery and more (plugins add widgets). **If anything goes
wrong, Ctrl+Alt+Win+T** turns the bar off and brings Windows' taskbar back. If the bar hangs or
stops, Windows' taskbar comes back in under a second.

## Plugins

Plugins add widgets, bar skins, tweaks, actions and rules. They come as `.rmwp` files or folders.

- Installing shows **what the plugin asks for** (for example: to see the titles of your windows, or
  to reach a weather site). Its script runs only once you approve. An update that asks for more
  needs a new approval.
- Scripts run apart from everything else, in a sandbox, with limits on memory and time. A plugin
  that misbehaves is stopped and named on screen; it cannot touch the bar, the rest of Ricing
  Manager or Windows.
- Writing plugins: `docs/plugins/` (also installed with the app).

## Hotkeys and rules

In the window (**Hotkeys and rules**) or in `%LOCALAPPDATA%\RicingManagerWindows\config\settings.toml`.
Ricing Manager keeps its icon in the tray, next to the clock: a click opens the window, the right
click has **Start with Windows** and **Restore the original Windows**. To have nothing running when
there are no hotkeys, rules or schedule, turn off **Keep the icon in the tray** in Settings.

## When something goes wrong

- **Safe mode:** hold **Shift** while Windows signs in, or it starts that way by itself after 3
  crashed starts in a row. Nothing is applied automatically, and *Restore the original Windows* is
  at hand.
- **Diagnostics → Make a support bundle** gathers logs and settings (your user name removed) into a
  zip you can attach to an issue. Nothing is ever sent by Ricing Manager.
- Logs: `%LOCALAPPDATA%\RicingManagerWindows\logs`.
- Everything from the command line: `rmw status`, `rmw restore --original`, `rmw action bar.on`.

## Privacy

Ricing Manager sends nothing anywhere. The only network requests are those of plugins you allowed
to reach a site (the weather example), made for them by Ricing Manager, within limits.

## Command line

```text
rmw apply <folder>         apply a rice          rmw status             what is managed
rmw restore --original     the original Windows  rmw history            what you can go back to
rmw import <file.rice>     add a shared rice     rmw pack <folder>      make a .rice to share
rmw plugin install <x>     a .rmwp or a folder   rmw apps               installed apps (for launch.app)
```

`rmw --help` lists everything.
