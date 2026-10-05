--!strict
-- An alternative window list: chips with the app's icon and a short title. Icons are named, not
-- sent: "window:<id>" asks the bar for that window's icon.

local order: { number } = {} -- window ids, oldest first; a window keeps its place while it lives
local byId: { [number]: RmwWindow } = {}
local widgets: { [string]: RmwWidget } = {}

local function short(title: string, max: number): string
    local length = utf8.len(title)
    if length and length > max then
        local cut = utf8.offset(title, max) or #title
        return string.sub(title, 1, cut - 1) .. "…"
    end
    return title
end

-- The provider lists the top of the z-order first. Bringing a window forward changes that order,
-- so the chips follow their own: known windows stay where they are, new ones go at the end.
local function update(list: { RmwWindow })
    byId = {}
    for _, window in list do
        byId[window.id] = window
    end
    local kept = {}
    local known = {}
    for _, id in order do
        if byId[id] then
            table.insert(kept, id)
            known[id] = true
        end
    end
    for i = #list, 1, -1 do -- the bottom of the z-order: the oldest of the new ones
        local id = list[i].id
        if not known[id] then
            table.insert(kept, id)
            known[id] = true
        end
    end
    order = kept
end

local function draw(w: RmwWidget)
    local entries = {}
    for _, id in order do
        if #entries >= w.config.max then
            break
        end
        local window = byId[id]
        table.insert(entries, {
            id = id,
            title = short(window.title, w.config.title_max),
            icon = "window:" .. tostring(id),
            active = window.active,
        })
    end
    w:set({ windows = entries, labels = w.config.labels })
end

rmw.widget(function(w)
    widgets[w.id] = w
    draw(w)
    w:on("pick", function(id: number)
        -- Like a taskbar button: the window in front goes down, any other comes up (the bar knows
        -- which one is in front at the moment of the click).
        rmw.action("window.activate", { id = id })
    end)
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("windows.list", function(list: { RmwWindow })
    update(list)
    for _, w in widgets do
        draw(w)
    end
end)

update(rmw.provider("windows.list") or {})
