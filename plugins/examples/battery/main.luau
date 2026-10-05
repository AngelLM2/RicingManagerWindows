--!strict
-- Battery: a small gauge and the percentage; nothing at all on a PC without a battery.

local widgets = {}

local function draw(w, battery)
    if not battery or not battery.present then
        w:set({ shown = false })
        return
    end
    w:set({
        shown = true,
        level = battery.percent / 100,
        text = string.format("%d%%", battery.percent),
        charging = battery.plugged_in,
        warn = battery.percent < w.config.low_at and not battery.plugged_in,
        low = w.config.low,
    })
end

rmw.widget(function(w)
    widgets[w.id] = w
    draw(w, rmw.provider("battery"))
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("battery", function(battery)
    for _, w in widgets do
        draw(w, battery)
    end
end)
