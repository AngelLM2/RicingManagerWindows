--!strict
-- Focus mode: an action (hotkeys, rules and `rmw action` call it). On: the bar goes away and the
-- rice in the "focus" folder of this plugin applies. Off: the rice before it comes back
-- (rice.undo) with the bar. The state is kept, so a restart of the host does not lose track.

rmw.onaction("toggle", function(args)
    local on = rmw.storage.get("on") == true
    if on then
        rmw.action("rice.undo")
        rmw.action("bar.on")
    else
        rmw.action("bar.off")
        rmw.action("rice.apply", { rice = "focus" })
    end
    rmw.storage.set("on", not on)
    rmw.log(if on then "focus mode off" else "focus mode on")
end)
