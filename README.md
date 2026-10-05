# Ricing Manager for Windows

> 🇧🇷 [Português](#português) · 🇺🇸 [English](#english)

**Status: alpha (v0.1.0, milestone M1 "Motor").** The engine works from the command line: apply a rice, undo it, go back to the original Windows. No graphical interface yet (see [docs/04-mvp.md](docs/04-mvp.md)).

---

## Português

Customizador geral do Windows 11, **open source (GPLv3)** e **nativo em C++**: wallpaper estático e em vídeo por monitor, modo claro/escuro (inclusive por horário), cor de destaque, cursores, **barra de tarefas própria totalmente dimensionável** e **plugins da comunidade** — tudo descrito num arquivo `rice.toml` que você pode compartilhar.

### Princípios

- **Leve**: o que fica residente custa poucos MB; sem nada dinâmico ativo, nada fica rodando.
- **Seguro e reversível**: toda mudança tem snapshot e pode ser desfeita; "Restaurar Windows original" sempre disponível. Sem admin por padrão. O app nunca injeta código em outros processos nem altera arquivos do sistema.
- **Extensível**: widgets, skins, ajustes, ações e regras podem ser criados pela comunidade, rodando isolados e com permissões.

Planejamento completo: [docs/README.md](docs/README.md).

### Usar (linha de comando, v0.1)

Um rice é uma pasta com um `rice.toml` e seus arquivos:

```toml
schema = 1

[meta]
name = "Tokyo Night"

[appearance]
mode = "dark"
accent = "#7aa2f7"

[taskbar]
search = "icon"

[[wallpaper.monitor]]
match = "all"
source = "assets/mountains.jpg"
```

```powershell
rmw validate .	okyo-night     # confere o rice (erros com linha e coluna)
rmw apply .	okyo-night        # aplica; tudo o que muda fica guardado
rmw history                    # o que foi feito
rmw restore --snapshot 3       # volta a antes da operação 3
rmw restore --original         # volta ao Windows de antes do primeiro apply
rmw export .\meu-visual        # salva o visual atual como um rice
rmw monitors                   # monitores e suas chaves (para match = "edid:...")
```

O formato completo está no [doc 03 §6](docs/03-arquitetura.md). Ajustes que você mudar por conta própria depois de aplicar não são desfeitos por engano (use `--force` para restaurar mesmo assim).

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

### Use (command line, v0.1)

A rice is a folder with a `rice.toml` (example above) and its files.

```powershell
rmw validate .	okyo-night     # check a rice (errors with line and column)
rmw apply .	okyo-night        # apply it; everything it changes is recorded
rmw history                    # what was done
rmw restore --snapshot 3       # back to before operation 3
rmw restore --original         # back to the Windows you had before the first apply
rmw export .\my-look           # save the current look as a rice
rmw monitors                   # monitors and their keys (for match = "edid:...")
```

Settings you change yourself after applying are not undone by mistake (`--force` restores them anyway).

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
