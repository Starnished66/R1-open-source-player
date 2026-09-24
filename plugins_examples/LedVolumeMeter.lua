plugin.define({ id = "example.led_volume_meter", name = "LED Volume Meter", version = "1.0", api_min = 13 })

-- Blue shows ordinary volume; red takes over above the high-volume threshold.
-- While paused, red breathes. LED effects are released automatically when
-- plugins are torn down or reloaded; stopped also demonstrates manual release.

local HIGH_VOLUME = 75

local function update_leds()
    if not plugin.led_available() then return end
    local volume = plugin.get_volume()

    if plugin.is_paused() then
        plugin.led_status("blue")
        plugin.led_breathe("red", 60, math.max(20, volume))
    elseif volume >= HIGH_VOLUME then
        plugin.led_status("blue")
        plugin.led_set("red", volume)
    else
        plugin.led_status("red")
        plugin.led_set("blue", volume)
    end
end

plugin.on("volume_changed", update_leds)
plugin.on("paused", update_leds)
plugin.on("resumed", update_leds)
plugin.on("track_started", function() update_leds() end)
plugin.on("stopped", function()
    plugin.led_release()
end)

update_leds()
