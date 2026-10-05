# Security policy

Ricing Manager for Windows changes system settings and loads content made by other people (rices and plugins), so security reports are taken seriously.

## Supported versions

Until 1.0, only the latest pre-release receives security fixes.

## Reporting a vulnerability

**Do not open a public issue.** Use GitHub's private reporting instead:
*Security* tab → *Report a vulnerability* in this repository.

Please include: affected version/commit, Windows version, steps to reproduce, and the impact you observed. A proof-of-concept rice or plugin package is very welcome.

You will get an acknowledgement within 7 days. We will agree on a disclosure date with you; credit is given in the release notes unless you prefer otherwise.

## In scope (examples)

- A rice (`.rice`) or plugin (`.rmwp`) package that escapes the package folder, executes code, crashes the app or exhausts memory/disk.
- A plugin that bypasses its declared permissions or the sandbox.
- A tweak that writes outside the allowed registry list.
- Any way for another user, a low-integrity or an AppContainer process to control the app through its IPC.
- Anything that leaves Windows in a state that "Restore original Windows" cannot undo.

## Design references

[docs/06 — Security, memory and stability](docs/06-seguranca-memoria-e-estabilidade.md) and [docs/12 — Plugins](docs/12-plugins.md).
