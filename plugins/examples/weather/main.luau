--!strict
-- Weather from Open-Meteo. The network goes through the plugin host, to api.open-meteo.com only;
-- the last reading is kept (rmw.storage) so the bar shows something at once after a restart.

-- WMO weather interpretation codes, grouped.
local function sky(code: number): string
    if code == 0 then
        return "Clear"
    elseif code <= 3 then
        return "Cloudy"
    elseif code <= 48 then
        return "Fog"
    elseif code <= 67 or (code >= 80 and code <= 82) then
        return "Rain"
    elseif code <= 77 or code == 85 or code == 86 then
        return "Snow"
    else
        return "Storm"
    end
end

-- The last reading, kept in rmw.storage.
type Reading = { temperature: number, code: number, at: number }

local widgets: { [string]: RmwWidget } = {}

local function show(w: RmwWidget, reading: Reading?)
    if not reading then
        w:set({ text = "…", tip = w.config.place })
        return
    end
    local symbol = if w.config.unit == "fahrenheit" then "°F" else "°"
    w:set({
        text = string.format("%d%s %s", math.floor(reading.temperature + 0.5), symbol, sky(reading.code)),
        tip = string.format("%s, %s", w.config.place, os.date("%H:%M", reading.at)),
    })
end

local function key(w: RmwWidget): string
    return string.format("%.2f,%.2f,%s", w.config.latitude, w.config.longitude, w.config.unit)
end

local function refresh(w: RmwWidget)
    local url = string.format(
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&temperature_unit=%s",
        w.config.latitude,
        w.config.longitude,
        w.config.unit
    )
    rmw.http.get(url, function(response: RmwHttpResponse)
        if not response.ok then
            rmw.warn("weather: " .. (response.error or ("HTTP " .. response.status)))
            return
        end
        local data: any = rmw.json.decode(response.body)
        if not data or not data.current then
            rmw.warn("weather: unexpected answer")
            return
        end
        local reading: Reading = {
            temperature = data.current.temperature_2m,
            code = data.current.weather_code,
            at = os.time(),
        }
        rmw.storage.set(key(w), reading)
        if widgets[w.id] then
            show(w, reading)
        end
    end)
end

rmw.widget(function(w)
    widgets[w.id] = w
    show(w, rmw.storage.get(key(w)))
    refresh(w)
    w:every(w.config.minutes * 60 * 1000, function()
        refresh(w)
    end)
    w:on("clicked", function()
        refresh(w) -- at most one request every 10 s: a quick second click is answered "too many"
    end)
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)
