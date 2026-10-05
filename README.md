# Ricing Manager for Windows

> 🇧🇷 [Português](#português) · 🇺🇸 [English](#english)

**Status: pre-alpha.** The foundation is being built (see [docs/04-mvp.md](docs/04-mvp.md)). Nothing here is ready for end users yet.

---

## Português

Customizador geral do Windows 11, **open source (GPLv3)** e **nativo em C++**: wallpaper estático e em vídeo por monitor, modo claro/escuro (inclusive por horário), cor de destaque, cursores, **barra de tarefas própria totalmente dimensionável** e **plugins da comunidade** — tudo descrito num arquivo `rice.toml` que você pode compartilhar.

### Princípios

- **Leve**: o que fica residente custa poucos MB; sem nada dinâmico ativo, nada fica rodando.
- **Seguro e reversível**: toda mudança tem snapshot e pode ser desfeita; "Restaurar Windows original" sempre disponível. Sem admin por padrão. O app nunca injeta código em outros processos nem altera arquivos do sistema.
- **Extensível**: widgets, skins, ajustes, ações e regras podem ser criados pela comunidade, rodando isolados e com permissões.

Planejamento completo: [docs/README.md](docs/README.md).

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
