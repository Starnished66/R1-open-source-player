plugin.define({ id = "example.net_radio", name = "Net Radio", version = "1.2", api_min = 1 })

-- Net Radio reads its stations from Radio.txt at the root of the SD card:
--
--   Station Name | http://example.com/direct-stream.mp3
--
-- Blank lines and lines beginning with # are ignored. A line containing
-- only an http(s) URL is also accepted; the URL is then used as its label.
-- The file is read again every time the tile is opened, so station changes
-- do not require restarting the player or reloading the plugin.
--
-- Current stream limitations:
--   * Streams may serve MP3, FLAC, or ADTS-framed AAC/AAC+ audio.
--   * Streams cannot seek and do not auto-reconnect after a connection loss.
--   * ICY/stream metadata is not displayed; Radio.txt supplies the title.

local RADIO_FILE = plugin.sd_root() .. "/Radio.txt"

local function trim(value)
    return (value:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function load_stations()
    local file = io.open(RADIO_FILE, "r")
    if not file then
        return nil, "Radio.txt was not found in the SD card root"
    end

    local labels, urls = {}, {}
    for line in file:lines() do
        -- Lua 5.4 treats a generic-for control variable as const. Normalize
        -- into a separate local instead of assigning back into `line`.
        local text = trim(line:gsub("\r$", ""))
        if text ~= "" and text:sub(1, 1) ~= "#" then
            local name, url = text:match("^(.-)%s*|%s*(https?://.+)$")
            if not url and text:match("^https?://") then
                name, url = text, text
            end

            if url then
                name, url = trim(name), trim(url)
                if name == "" then name = url end
                labels[#labels + 1] = name
                urls[#urls + 1] = url
            end
        end
    end
    file:close()

    if #urls == 0 then
        return nil, "Radio.txt contains no valid stations"
    end
    return { labels = labels, urls = urls }
end

local function open_stations()
    local stations, err = load_stations()
    if not stations then
        plugin.show_toast(err)
        return
    end

    plugin.show_list("Net Radio", stations.labels, function(index)
        plugin.play_list(stations.urls, index)
    end)
end

plugin.register_stream_media_tile("Net Radio", open_stations, "stream_media/radio.png")
