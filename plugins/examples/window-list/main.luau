--!strict
-- An alternative window list: chips with the app's icon and a short title. Icons are named, not
-- sent: "window:<id>" asks the bar for that window's icon.

local windows = {}
local widgets = {}

local function short(title: string, max: number): string
    if utf8.len(title) and utf8.len(title) > max then
        local cut = utf8.offset(title, max) or #title
        return string.sub(title, 1, cut - 1) .. "…"
    end
    return title
end

local function draw(w)
    local entries = {}
    -- The provider lists the top of the z-order first; a taskbar shows the oldest first.
    for i = #windows, 1, -1 do
        local window = windows[i]
        if #entries >= w.config.max then
            break
        end
        table.insert(entries, {
            id = window.id,
            title = short(window.title, w.config.title_max),
            icon = "window:" .. tostring(window.id),
            active = window.active,
        })
    end
    w:set({ windows = entries, labels = w.config.labels })
end

rmw.widget(function(w)
    widgets[w.id] = w
    draw(w)
    w:on("pick", function(id: number)
        for _, window in windows do
            if window.id == id then
                -- Like a taskbar button: the window in front goes down, any other comes up.
                rmw.action(if window.active then "window.minimize" else "window.focus", { id = id })
                return
            end
        end
    end)
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("windows.list", function(list)
    windows = list
    for _, w in widgets do
        draw(w)
    end
end)

windows = rmw.provider("windows.list") or {}
