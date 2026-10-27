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
  own taskbar always comes back, as it was (not left auto-hidden). If you keep the rice on screen and
  remove your data, the files it shows (pictures, pointers, sounds) stay in
  `%LOCALAPPDATA%\RicingManagerWindows\store`, because Windows uses them. A note of how your Windows looked
  stays next to that folder (`RicingManagerWindows.original.json`), so installing Ricing Manager again
  and restoring the original Windows still brings back your own.

## First steps

1. Open **Ricing Manager** from the Start menu.
2. **Rices** lists your library. Pick one — *Diva Stage*, *Tokyo Midnight*, *Sakura Dawn*,
   *Mocha Kawaii* — and press **Apply**. It takes a second or two.
3. Don't like it? **Home → Undo** goes back to exactly how things were before that change (pressing
   it again brings the change back). **Restore the original Windows** goes back to your own Windows.

Before Ricing Manager changes something, it notes how you had it, so it can always go back. What you
change yourself afterwards (in Windows' Settings, while a rice is on) is yours: Ricing Manager leaves
it as you set it, and a restore keeps it.

## The window

| Page | What it does |
|---|---|
| **Home** | The applied rice, what Ricing Manager manages now, Undo, Restore, the history |
| **Rices** | Apply, edit, duplicate, share (**Share…** makes a `.rice` file), import (**Import a .rice…**), save your current look as a rice |
| **Appearance** | Light/dark (or on a schedule), accent color, transparency |
| **Wallpapers** | Per monitor: a picture, a slideshow folder, a video or a color; how it fits; when videos pause |
| **Cursors** | A cursor scheme (a folder with `install.inf`, or one file per cursor) and its size (16 and 24 draw the rice's pointers smaller; Windows' smallest is 32) |
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
- **Restore the original Windows** brings back what you had: the theme you had selected (Settings
  shows it selected again), your mouse pointers as you had them (your own pointer style and size
  included), your sounds, icons, lock screen and wallpaper. Your own files are put back where they
  were; Ricing Manager keeps a copy only for when one of them was deleted. A rice never touches your
  recent colors or your pointer style behind your back. Windows' own defaults never take the place of
  anything of yours: a theme file of yours that is gone comes back as its colors and its name, and a
  picture that is gone with its copy leaves the one on screen. The message says what stayed, and what
  was kept the way you changed it.
- A rice is plain text (`rice.toml`) plus its files; the format is in the
  [README](../README.md) and in `docs/03-arquitetura.md`.

## Video wallpapers

A video plays behind the desktop icons and pauses by itself when it is not seen (a full-screen app
or game, the desktop covered, the screen off or locked) and on battery, where the rice's still
picture shows instead. After two minutes paused, its memory is given back.

## Live wallpapers: effects and particles

In **Wallpapers**, a picture, a video or a color can have **effects** (brightness, contrast,
saturation, hue, blur, a parallax that follows the mouse, a slow zoom, a vignette; a video also has
a speed) and **particles** over it: sparkles, stars, snow, rain, petals, fireflies, magic dust,
music notes, hearts or bubbles, each with its own color, amount, size, speed and an optional glow.
Ricing Manager draws them behind the desktop icons; Windows keeps the plain picture underneath. A
still picture costs nothing once drawn, and what moves pauses like a video.

## Wallpaper Engine

If you have Wallpaper Engine, **Wallpapers → From Wallpaper Engine…** lists the wallpapers it has on
this computer (read where Steam keeps them; nothing is downloaded or changed). If yours are
somewhere else, **Look in a folder…**. A video plays as it is; a scene or a web wallpaper only plays
inside Wallpaper Engine, so its picture is used, with a slow zoom (add particles to bring it to
life). Those wallpapers belong to their authors: a rice that uses one is marked as yours alone, and
it cannot be shared or packed.

## Sounds and icons

- **Sounds:** a rice can give its own `.wav` for each of Windows' sounds (notification, new mail,
  device connected, the Recycle Bin, the folder click...) or silence them. In **Sounds**, *Choose a
  .wav…* for an event, ▶ to hear it, *Silent* for none; **Silence for the events below that have no
  sound of their own** silences the rest.
- **Icons:** in **Icons**, a rice can change the desktop's icons (This PC, the Recycle Bin empty and
  full, Network, your user folder, Control Panel), your folders' (Desktop, Documents, Downloads,
  Pictures, Music, Videos) and a program's: write its file name (`firefox.exe`) and choose an `.ico`.
  Your own shortcuts to it take the icon (in your Start menu, on your desktop and pinned to the
  taskbar), and Ricing Manager's bar, dock and Start show it too. **File types** work the same way
  (`txt`): their files show the icon. Only `.ico` files, checked first.
- **Every folder and drive (advanced):** a rice can also change the icon of every folder and every
  drive, for every user of the PC. Turn on **Folder and drive icons for the whole PC** in
  **Settings** first; Windows then asks for administrator each time a rice changes them or you
  restore. Ricing Manager is not signed: the prompt says "unknown publisher".
- **Other apps' windows:** at the end of **Appearance**, a rice can give every app's window
  (Windows 11) its corners, border color, title bar and title colors, a Mica or Acrylic title bar and
  dark title bars, and leave some programs out. Ricing Manager sets them from the outside while it
  runs; when it closes or you restore, Windows' own come back. Apps running as administrator keep
  theirs.
- **Lock screen:** at the end of **Wallpapers**, a rice can give the lock screen its own picture
  (`.jpg` or `.png`) or turn **Windows Spotlight** on, and show or hide the fun facts and tips.
- Restoring brings back the sounds, icons and lock screen picture you had (with Spotlight, your
  "Picture" comes back first, then Spotlight is turned on again and moves on to its next picture
  when Windows gets one). Windows' own sounds, icons and pictures are never copied
  into a rice you share.

## Bars of your own

A rice can hide Windows' taskbar and show its own bars, with Start, your windows, the system tray,
the clock, media, volume, network, battery and more (plugins add widgets). In **Taskbar**, a
**Style** gives a whole bar at once: **Windows** (apps in the middle), **Dock** (floating, icons
that grow under the mouse), **Island** (a pill at the top), **Status line** or **Side**. Whatever
you change below the style stays yours. A dock shows the apps you pin (**Pin an app…**) and every
app with a window open; a bar can be **auto** long, as long as what it shows, and it grows and shrinks
smoothly as apps come and go or the tray's hidden icons slide in. The apps you open from the bar (its
Start, the dock) live on their own: if the bar stops, they stay open. Every bar keeps
maximized windows clear of it (**Reserve**, the default since v1.7, the dock included); choose
**Float** and windows go under it, like a bar drawn over the desktop. **If anything goes
wrong, Ctrl+Alt+Win+T** turns the bar off and brings Windows' taskbar back. If the bar hangs or
stops, Windows' taskbar comes back in under a second.

## A Start of your own

In **Taskbar → Start**, choose **Ricing Manager's** Start: the Start button of your bar then opens a
menu with all your apps and a search (type, then Enter), your pinned apps (**Pin an app to Start…**),
your folders, Settings and the power buttons, in the rice's colors. In **Settings**, **The Win key
opens Ricing Manager's Start** makes the Win key open it too; Win with any other key (Win+E, Win+L,
Win+arrows...) stays Windows'. If Ricing Manager's bar stops, the Win key is Windows' again by
itself. With **Windows'** Start, a rice can choose its layout (more pins or more recommendations)
and hide the recent files and tips; restoring brings back what you had.

## Tiling: windows side by side

In **Tiling**, turn on **Organize the windows**. Every app window then takes a place beside the
others, with a small gap; turning it off puts every window back where it was. Dialogs, windows of a
fixed size and the apps you list under **Apps** float; apps running as administrator are left alone.
An app that will not get smaller than a size of its own (Discord, Spotify...) gets that room and the
others share the rest; when they cannot all fit, none goes off the screen. A window in full screen (a
browser's F11, a video, a game) is left alone. With **Animations** on (and Windows' own animation
effects), windows slide to their places: a workspace that comes in, a window that opens, two that trade
places.

Workspaces:

- **Like Linux** (the default): workspaces 1 to 9 on each monitor. Ctrl+Win+3 shows workspace 3 of
  the monitor you are on; the windows of the others hide (and come back with their dialogs). Add the
  **Workspaces (tiling)** widget to your bar to see the ones in use and click to go there.
- **Like a Mac**: Windows' own virtual desktops, sliding as usual (Ctrl+Win+arrows, or Ctrl+Win+3 to
  go to the third). Windows does not let a program move another app's window to another desktop:
  for that, use Win+Tab.

The keys it comes with (all of them can be changed or turned off on the page; # is a number):

| Key | What it does |
|---|---|
| Ctrl+Alt+arrows | Focus the window that way (past the last one: the next monitor) |
| Ctrl+Alt+Shift+arrows | Move the window that way |
| Ctrl+Win+Left / Right | Previous / next workspace |
| Ctrl+Win+# | Go to workspace # |
| Ctrl+Win+Shift+# | Send the window to workspace # |
| Ctrl+Alt+F | The window floats (again: it is tiled again) |
| Ctrl+Alt+M | The window takes the whole screen, and back |
| Ctrl+Alt+Space | Next layout: halves, a main window and a stack, columns, one at a time |
| Ctrl+Alt+Enter | The window becomes the main one |

Dragging a window onto another swaps them; dropping it on another monitor moves it there. AltGr is
never taken for Ctrl+Alt, so it keeps typing characters. If the tiler stops for any reason, every
hidden window shows again by itself.

Windows does not let an ordinary program move the windows of apps running as administrator, nor hear
the keys typed into them. **Advanced mode** (on the same page) tiles them too: each time tiling
starts, Windows asks for your permission. If you say no, tiling goes on without them.

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
click has **Start with Windows** and **Restore the original Windows**. **Exit** closes Ricing
Manager, its window included. To have nothing running when there are no hotkeys, rules or schedule,
turn off **Keep the icon in the tray** in Settings.

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
