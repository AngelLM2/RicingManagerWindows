# Contributing

Thanks for helping! Ricing Manager for Windows is GPL-3.0-or-later. By contributing you agree that your contribution is licensed under the same terms.

## Ways to contribute

| You are… | You can… | Needs C++? |
|---|---|---|
| a designer | make a **rice** (`rice.toml` + assets) | no |
| a scripter | write a **plugin** in Luau (widgets, skins, tweaks, actions) — from milestone M4 | no |
| a C++ developer | work on the engine, modules, bar, UI | yes |
| anyone | report bugs, test on your Windows build, translate | no |

## Ground rules for code (summary of [docs/08](docs/08-engenharia-build-testes-ci.md))

1. No manual `new`/`delete`/`malloc`; every Windows resource lives in a RAII wrapper (WIL).
2. Raw pointers never own anything.
3. Domain APIs return `rmw::Result<T>`; exceptions never cross thread, callback, IPC or process boundaries.
4. UTF-16 only at the Win32 boundary, UTF-8 everywhere else. Never `path::string()` or `std::wcout`.
5. Every external input has a limit (`libs/base/include/rmw/base/Limits.hpp`).
6. `libs/domain` never includes `<windows.h>`; only `libs/platform` talks to Win32.
7. Anything that changes the system implements the `IModule` contract (capture / plan / apply / restore) and passes the contract test.
8. A bug fix comes with a test that reproduces the bug.

## Workflow

1. Build and test: `.\scripts\build.ps1 -Preset debug -Test` (and `-Preset asan -Test`).
2. Format: `.\scripts\format.ps1`.
3. Commit with [Conventional Commits](https://www.conventionalcommits.org/) (`feat(bar): …`, `fix(cursors): …`).
4. Open a pull request against `develop`. CI must be green.

Significant design changes need an ADR in `docs/adr/`.

## Security

Never report vulnerabilities in public issues — see [SECURITY.md](SECURITY.md).
