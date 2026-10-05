--!strict
-- Now playing: the bar's own media widget, rebuilt on the plugin API (docs/12 §7).

local widgets: { [string]: RmwWidget } = {}

local function describe(media: RmwMedia?): string
    if not media or not media.session or media.title == "" then
        return ""
    end
    return media.title
end

local function show(w: RmwWidget, media: RmwMedia?)
    local text: string = describe(media)
    if media and text ~= "" and w.config.show_artist and media.artist ~= "" then
        text ..= " — " .. media.artist
    end
    w:set({
        title = text,
        playing = media ~= nil and media.playing == true,
        shown = text ~= "",
        limit = w.config.max_width,
    })
end

local function command(name: string)
    return function()
        rmw.action("media.control", { command = name })
    end
end

rmw.widget(function(w)
    widgets[w.id] = w
    show(w, rmw.provider("media"))
    w:on("toggle", command("toggle"))
    w:on("next", command("next"))
    w:on("previous", command("previous"))
    w:onclose(function()
        widgets[w.id] = nil
    end)
end)

rmw.on("media", function(media: RmwMedia)
    for _, w in widgets do
        show(w, media)
    end
end)
