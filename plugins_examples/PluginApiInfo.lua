plugin.define({ id = "example.api_info", name = "Plugin API Info", version = "1.0", api_min = 1 })

-- Demonstrates API/build discovery and optional-capability checks. Plugins can
-- use has_capability() to retain one file that works across multiple player
-- versions instead of assuming every newer function exists.

local capabilities = {
    "network.http.async",
    "playback.events",
    "ui.row_width",
    "library.artist_albums",
    "audio.peq",
}

local function open_info()
    local info = plugin.get_app_info()
    local rows = {
        "Player: " .. info.version,
        "Build: " .. info.build,
        "Platform: " .. info.platform,
        "Plugin API: " .. plugin.api_version(),
    }
    for _, capability in ipairs(capabilities) do
        table.insert(rows, capability .. ": " .. (plugin.has_capability(capability) and "yes" or "no"))
    end
    plugin.show_list("Plugin API Info", rows, function(index)
        plugin.show_toast(rows[index])
    end, { height = 72 })
end

plugin.register_list_item("system", "Plugin API Info", open_info)
