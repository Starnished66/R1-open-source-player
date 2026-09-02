-- HomeBackground.lua
--
-- Sets a static image behind Home's own tiles, using
-- plugin.set_home_layout()'s options.background_image (PLUGIN API 10).
-- Only Home's background changes -- every other screen is unaffected.
--
-- Put a 480x800 (R1 panel's real full-screen resolution -- confirmed in
-- main.c's own SCREEN_WIDTH/SCREEN_HEIGHT, lv_conf.h, and the bootloader's
-- own fb_draw.c; 480x320 is a specific sub-panel elsewhere in the UI, not
-- the whole screen) .png or .jpg named "home_bg.jpg" next to this plugin
-- file on the SD card, under .plugins/. LVGL draws a background image at
-- its native size, centered, never stretched, so a file at any other
-- resolution will not fill the screen edge-to-edge.

plugin.define({
    id = "org.example.home_background",
    name = "Home Background",
    version = "1.0.0",
    api_min = 10,
})

if plugin.has_capability("ui.home_background") then
    local image_path = plugin.sd_root() .. "/.plugins/home_bg.jpg"

    -- set_home_layout() must run at top-level load, same requirement
    -- plugin.set_icon() has, so the image is in place before Home is
    -- first built this boot -- see PLUGINS.md's own doc section.
    plugin.set_home_layout({}, { background_image = image_path })
else
    plugin.show_toast("Home Background needs a newer player build")
end
