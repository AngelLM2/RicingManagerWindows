-- Ricing Manager plugin API 1.0: type definitions for luau-lsp (docs/plugins/api.md).
-- VS Code, in .vscode/settings.json:
--   "luau-lsp.platform.type": "standard",
--   "luau-lsp.types.definitionFiles": ["rmw.d.luau"]
-- The tests type-check every example plugin against this file (tests/unit/PluginTypesTests.cpp).

export type RmwConfig = { [string]: any }
export type RmwArgs = { [string]: any }

-- Types to annotate with (`function(cpu: RmwCpu)`): what providers give (rmw.provider and
-- rmw.on); "monitors", "windows.list" and "tray" give lists of RmwMonitor, RmwWindow and
-- RmwTrayIcon.
export type RmwTime = { unix: number, offset: number }
export type RmwCpu = { usage: number }
export type RmwMemory = { usage: number, total: number, available: number }
export type RmwNetwork = { down: number, up: number }
export type RmwBattery = { present: boolean, percent: number, plugged_in: boolean, charging: boolean }
export type RmwVolume = { device: string, level: number, muted: boolean }
export type RmwMonitor = {
    name: string,
    x: number,
    y: number,
    width: number,
    height: number,
    dpi: number,
    primary: boolean,
}
export type RmwDesktops = { count: number, current: number }
export type RmwMedia = {
    session: boolean,
    playing: boolean,
    title: string,
    artist: string,
    album: string,
    app: string,
}
export type RmwWindow = { id: number, title: string, app: string, active: boolean, minimized: boolean }
-- An icon of the system tray: "tray:<id>" is its picture in a widget; promoted = Windows shows it
-- on its taskbar (the others are in the overflow).
export type RmwTrayIcon = { id: number, tip: string, app: string, promoted: boolean }

-- Each needs the name in [permissions] providers.
export type RmwProviderName =
    "time"
    | "system.cpu"
    | "system.memory"
    | "network.stats"
    | "battery"
    | "audio.volume"
    | "monitors"
    | "virtual-desktops"
    | "media"
    | "windows.list"
    | "tray"

export type RmwActionName =
    "media.control"
    | "window.activate"
    | "window.focus"
    | "window.minimize"
    | "window.close"
    | "tray.click"
    | "rice.apply"
    | "rice.undo"
    | "wallpaper.next"
    | "launch.app"
    | "bar.toggle"
    | "bar.on"
    | "bar.off"
    | "appearance.toggle"
    | "appearance.dark"
    | "appearance.light"

export type RmwHttpResponse = { status: number, ok: boolean, body: string, type: string, error: string? }
export type RmwHttpOptions = { headers: { [string]: string }? }
export type RmwHttpCallback = (response: RmwHttpResponse) -> ()

-- One place where a bar.widget shows (rmw.widget).
export type RmwWidget = {
    id: string,
    config: RmwConfig,
    -- w:set({ name = value, ... }) or w:set("name", value): properties of [widget.properties].
    set: (self: RmwWidget, properties: { [string]: any } | string, value: any?) -> (),
    -- A callback of [widget.callbacks], with the arguments it declares.
    on: (self: RmwWidget, callback: string, fn: (...any) -> ()) -> (),
    every: (self: RmwWidget, ms: number, fn: () -> ()) -> number?,
    after: (self: RmwWidget, ms: number, fn: () -> ()) -> number?,
    onclose: (self: RmwWidget, fn: () -> ()) -> (),
}

declare rmw: {
    id: string,
    version: string,
    config: RmwConfig,

    log: (...any) -> (),
    warn: (...any) -> (),

    every: (ms: number, fn: () -> ()) -> number,
    after: (ms: number, fn: () -> ()) -> number,
    cancel: (id: number) -> (),

    -- The value (nil before the first reading) and its changes. Name the type where you take it:
    --   rmw.on("system.cpu", function(cpu: RmwCpu) ... end)
    --   local battery: RmwBattery? = rmw.provider("battery")
    provider: (name: RmwProviderName) -> any,
    on: (name: RmwProviderName, fn: (value: any) -> ()) -> (),

    -- Needs the name in [permissions] actions. Fire and forget: the outcome goes to the log.
    action: (name: RmwActionName, args: RmwArgs?) -> (),
    -- Answers "<plugin id>:<name>" (hotkeys, rules, rmw action); the name is in [[action]].
    onaction: (name: string, fn: (args: RmwArgs) -> ()) -> (),

    widget: (fn: (w: RmwWidget) -> ()) -> (),

    -- [permissions] storage = true; 1 MB in all.
    storage: {
        get: (key: string) -> any,
        set: (key: string, value: any) -> (),
        remove: (key: string) -> (),
    },

    -- [permissions] network = ["api.example.com"]; https only, one request every 10 s.
    http: {
        get: ((url: string, fn: RmwHttpCallback) -> ())
            & ((url: string, options: RmwHttpOptions, fn: RmwHttpCallback) -> ()),
        post: ((url: string, body: string, fn: RmwHttpCallback) -> ())
            & ((url: string, body: string, options: RmwHttpOptions, fn: RmwHttpCallback) -> ()),
    },

    json: {
        encode: (value: any) -> string,
        decode: (text: string) -> (any, string?),
    },
}
