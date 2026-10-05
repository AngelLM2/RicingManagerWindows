--!strict
-- A bar graph of the CPU use: one sample a second, the newest on the right.

local history: { number } = {}
local widgets = {}

local function draw(w)
    local samples = {}
    local first = math.max(1, #history - w.config.count + 1)
    for i = first, #history do
        table.insert(samples, { level = history[i] / 100 })
    end
    local last = history[#history]
    w:set({
        samples = samples,
        slots = w.config.count,
        accent = w.config.color,
        label = if w.config.number and last then string.format("%d%%", math.floor(last + 0.5)) else "",
    })
end

rmw.widget(function(w)
    widgets[w.id] = w
    draw(w)
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("system.cpu", function(cpu)
    table.insert(history, cpu.usage)
    if #history > 60 then
        table.remove(history, 1)
    end
    for _, w in widgets do
        draw(w)
    end
end)
