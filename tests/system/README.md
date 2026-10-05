# System tests (Windows Sandbox)

Tests that must change **real** Windows settings — cursors (`SPI_SETCURSORS`), wallpaper
(`IDesktopWallpaper`), theme (`IThemeManager2`), taskbar (`SHAppBarMessage`), the bar and the
video wallpaper — run inside **Windows Sandbox**, a disposable Windows that is thrown away
when it closes. They never touch the developer's own settings (docs/08 §4).

```powershell
.\scripts\build.ps1 -Preset debug
.\scripts\sandbox.ps1 -Preset debug -Suite unit     # unit tests inside the sandbox (smoke)
.\scripts\sandbox.ps1 -Preset debug -Suite system   # system tests (from milestone M1)
```

- The build output is mapped **read-only**; results come back in `build\sandbox\<preset>\`.
- Networking is disabled inside the sandbox.
- Only one Windows Sandbox can run at a time.
