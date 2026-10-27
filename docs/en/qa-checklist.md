# QA checklist — v1.0

> [Português](../qa-checklist.md) · [Documentation index](README.md)

This is the historical v1.0 acceptance checklist, not evidence of a current run. Sandbox means Windows Sandbox (`scripts\sandbox.ps1 -Suite <name>`), a disposable environment used for tests that change Windows.

## Before release

```powershell
.\scripts\format.ps1
.\scripts\tidy.ps1 -Path apps; .\scripts\tidy.ps1 -Path libs; .\scripts\tidy.ps1 -Path tests
.\scripts\build.ps1 -Preset debug -Test
.\scripts\build.ps1 -Preset asan -Test
.\scripts\fuzz.ps1 -Seconds 120
.\scripts\package.ps1
foreach ($s in 'unit','system','bar','plugins','tray','video','settings','packages') { .\scripts\sandbox.ps1 -Preset debug -Suite $s }
.\scripts\sandbox.ps1 -Preset release -Suite installer
```

## Acceptance criteria

| Criterion | Verification | Environment |
|---|---|---|
| Install and uninstall without admin; offer original restoration and return native taskbar | `installer`: per-user files and registry, restore and taskbar checks | Sandbox |
| Apply a full rice in under 3 typical seconds and undo without residue | `installer` measures four official rices; `system` compares state after undo and restore | Sandbox and real PC |
| Custom bar with tray; native bar returns after failure | `bar` and `tray`, including termination and geometry | Sandbox and real PC with actual tray apps |
| Hostile or stuck plugins stay isolated | `plugins` hostile script cases | Sandbox |
| Memory budgets | `settings`, `bar`, and `video`; measure GPU video separately | Sandbox and real PC |
| Worker recovery | `system` supervisor and `video` crash/hang cases | Sandbox |
| Windows 11 24H2/25H2 | Sandbox uses host Windows; other versions are best effort | Real PC |
| Mixed-DPI monitors and reconnect | Sandbox has one monitor | Real PC with second monitor |
| Hostile packages rejected without crashes | `packages`, unit tests, and package/rice/cursor/tweak fuzzing | Sandbox and local tests |
| ASan and fuzzing pass | Run the commands above | Local environment |

## On a real PC

1. Install the setup executable and open it from Start.
2. Apply each official rice and check video, cursors, and bar.
3. Check tray icons, clicks, menus, and tooltips with real applications.
4. Check that video wallpaper pauses for fullscreen content and resumes.
5. Measure 4K/60 video wallpaper memory on the target GPU.
6. Import a `.rice` and install a `.rmwp` through the interface.
7. Exercise everyday hotkeys, rules, and safe mode (Shift at sign-in).
8. Connect a second monitor and check per-monitor bar and wallpaper, mixed DPI, disconnect, and reconnect.
9. Uninstall with restoration selected and check the previous Windows appearance.
