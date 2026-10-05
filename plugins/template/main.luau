--!strict
-- Runs in rmw-plugin-host, sandboxed: see docs/plugins and rmw.d.luau. Save the file and the
-- widget reloads (rmw plugin dev .).

local widgets: { [string]: RmwWidget } = {}

rmw.widget(function(w)
    widgets[w.id] = w
    w:set("text", w.config.prefix .. "…")
    w:on("clicked", function()
        rmw.log("clicked on " .. w.id)
    end)
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("system.cpu", function(cpu: RmwCpu)
    for _, w in widgets do
        w:set("text", string.format("%s%d%%", w.config.prefix, math.floor(cpu.usage + 0.5)))
    end
end)
