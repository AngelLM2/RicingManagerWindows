--!strict
-- Mini tray: the system tray from the "tray" provider, drawn by the plugin. Icons are named, not
-- sent: "tray:<id>" asks the bar for that icon's picture; clicks go back through tray.click.

local widgets: { [string]: RmwWidget } = {}
local icons: { RmwTrayIcon } = {}

local function draw(w: RmwWidget)
    local items = {}
    for _, icon in icons do
        if #items >= w.config.max then
            break
        end
        if w.config.all or icon.promoted then
            table.insert(items, { id = icon.id, icon = "tray:" .. tostring(icon.id) })
        end
    end
    w:set({ icons = items })
end

rmw.widget(function(w)
    widgets[w.id] = w
    draw(w)
    w:on("click", function(id: number)
        rmw.action("tray.click", { id = id, button = "left" })
    end)
    w:on("menu", function(id: number)
        rmw.action("tray.click", { id = id, button = "right" })
    end)
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("tray", function(list: { RmwTrayIcon })
    icons = list
    for _, w in widgets do
        draw(w)
    end
end)

icons = rmw.provider("tray") or {}
