# My widget — a Ricing Manager plugin

A starting point for a [Ricing Manager for Windows](https://github.com/AngelLM2/RicingManagerWindows) plugin: a bar widget (`widget.slint`) fed by a Luau script (`main.luau`). Copy this folder (or use it as a GitHub template), then:

1. In `plugin.toml`, set `id` (reverse-DNS: `io.github.<you>.<name>`), `name`, `authors`, `license` and `description`.
2. Ask only for the permissions you use (`[permissions]`).
3. Run it from the folder, with live reload and the log:

   ```powershell
   rmw plugin dev .
   ```

   and put it in a rice's bar:

   ```toml
   [plugins]
   requires = ["io.github.you.my-widget"]

   [[bar]]
   right = ["io.github.you.my-widget", "clock"]
   ```

4. Check and pack it:

   ```powershell
   rmw plugin validate .
   rmw plugin pack . --out my-widget.rmwp
   ```

## Editor

VS Code with [luau-lsp](https://marketplace.visualstudio.com/items?itemName=JohnnyMorganz.luau-lsp) and the [Slint](https://marketplace.visualstudio.com/items?itemName=Slint.slint) extension: `.vscode/settings.json` points luau-lsp at `rmw.d.luau` (the types of the `rmw` API) and `.luaurc` turns on strict mode. `rmw plugin new` writes the same files.

## Docs

- [Guide](https://github.com/AngelLM2/RicingManagerWindows/tree/main/docs/plugins) (in Portuguese): manifest, Luau API, widgets, tweaks, security.
- [Example plugins](https://github.com/AngelLM2/RicingManagerWindows/tree/main/plugins/examples).

Files whose names start with `.` and `.d.luau` files stay out of the `.rmwp`.
