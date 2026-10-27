# Plugin author guide

> [Português](../plugins/README.md) · [Documentation index](README.md)

Plugins can provide bar widgets and skins, tweaks, actions, and rules. The built-in widgets and plugins use the same data and actions; the clock and battery in [plugin examples](../../plugins/examples) demonstrate the API.

## Plugin kinds

| Kind | Purpose | Runs code? |
|---|---|---|
| `bar.widget` | A `.slint` component, optionally fed by a Luau script | Yes, in `rmw-plugin-host` |
| `bar.skin` | Starting values for a `[[bar]]`: dimensions, background, corners, and widgets | No |
| `tweak` | Allow-listed HKCU registry values applied and undone like other changes | No |
| `action` | Named actions (`<id>:<action>`) for hotkeys, rules, and CLI | Yes |
| `rule` | A script that observes providers and invokes actions | Yes |

A plugin can combine kinds. Scripts run in a Luau sandbox per plugin in `rmw-plugin-host.exe`, rather than in the bar or core. The documented limits include 16 MB per VM, 50 ms per callback, and a process watchdog. A plugin that fails three times in five minutes is stopped until enabled again. Widget `.slint` files are validated before compilation. See the detailed [security model](../plugins/seguranca.md) (Portuguese).

## Get started

```powershell
rmw plugin new widget C:\code\my-widget --id io.github.you.my-widget
rmw plugin dev C:\code\my-widget
```

Add the plugin to a rice and apply the rice:

```toml
[plugins]
requires = ["io.github.you.my-widget"]

[[bar]]
right = ["io.github.you.my-widget", "clock"]
```

`rmw plugin dev` runs from the folder, reloads when files are saved, and shows the log. Templates are available for `widget`, `action`, `rule`, `tweak`, and `skin`; the [template repository](../../plugins/template) contains a ready-to-use widget template.

| Command | Purpose |
|---|---|
| `rmw plugin validate <folder>` | Check manifest, script, widget and images |
| `rmw plugin pack <folder> [--out file.rmwp]` | Create a package to share |
| `rmw plugin install <folder>` | Install and review requested permissions |
| `rmw plugin remove <id>` | Uninstall and delete plugin storage |
| `rmw plugin approve <id>` | Approve permissions; increased permissions require renewed approval |
| `rmw plugin enable <id>` / `rmw plugin disable <id>` | Enable or disable actions and rules; re-enable a stopped plugin |
| `rmw plugin list` / `rmw plugin status` | Inspect installation, approval, and execution state |
| `rmw action <id>:<action>` | Invoke a plugin action |

The full [manifest](../plugins/manifesto.md), [Luau API](../plugins/api.md), [widget](../plugins/widgets.md), [tweak](../plugins/tweaks.md), and [security](../plugins/seguranca.md) references are currently in Portuguese. Community plugins are published separately; request only the permissions used, specify a license, credit assets, and use semantic versions.
