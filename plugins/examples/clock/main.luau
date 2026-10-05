--!strict
-- The clock: os.date on a timer. It sends only what changed, so the bar redraws once a minute
-- for "%H:%M".

local function stamp(format: string): string
    local ok, text = pcall(function()
        return os.date(format)
    end)
    return if ok and type(text) == "string" then text else "?"
end

rmw.widget(function(w)
    local shown = { time = "", date = "" }
    local dateFormat: string = w.config.date_format
    local function tick()
        local now = {
            time = stamp(w.config.format),
            date = if dateFormat ~= "" then stamp(dateFormat) else "",
        }
        if now.time ~= shown.time or now.date ~= shown.date then
            shown = now
            w:set(now)
        end
    end
    tick()
    w:every(1000, tick)
end)
