plugin.define({ id = "example.api_info", name = "Plugin API Info", version = "2.2", api_min = 1 })

-- Comprehensive API and Capability Inspector for Open Source Player.
-- Demonstrates all available namespaces, functions, capabilities, and runtime info.

local CAPABILITIES = {
    "ui.list", "ui.settings", "ui.row_width", "ui.text_input", "ui.toast", "ui.theme",
    "filesystem.sd", "playback.control", "playback.state", "playback.events",
    "library.artist_albums", "library.paged", "network.http.sync", "network.http.async",
    "network.http.download", "filesystem.mkdir", "crypto.md5", "audio.peq", "data.json",
    "storage.namespaced", "storage.secrets", "playback.remote", "filesystem.playlists", "library.refresh"
}

local CATEGORIES = {
    {
        title = "Runtime & Overview",
        get_rows = function()
            local info = plugin.get_app_info()
            return {
                "Player Version: " .. tostring(info.version or "unknown"),
                "Build Stamp: " .. tostring(info.build or "unknown"),
                "Platform: " .. tostring(info.platform or "unknown"),
                "Plugin API Level: " .. tostring(plugin.api_version()),
                "SD Root: " .. plugin.sd_root(),
                "Active Play Mode: " .. tostring(plugin.get_play_mode()),
                "Is Playing: " .. (plugin.is_playing() and "Yes" or "No"),
                "Is Paused: " .. (plugin.is_paused() and "Yes" or "No"),
            }
        end
    },
    {
        title = "Capabilities",
        get_rows = function()
            local rows = {}
            for _, cap in ipairs(CAPABILITIES) do
                local supported = plugin.has_capability(cap)
                rows[#rows + 1] = (supported and "✓ " or "✗ ") .. cap
            end
            return rows
        end
    },
    {
        title = "Media Capabilities",
        get_rows = function()
            if not plugin.media_capabilities then return { "media_capabilities() not available" } end
            local mc = plugin.media_capabilities()
            local codecs = table.concat(mc.codecs or {}, ", ")
            local containers = table.concat(mc.containers or {}, ", ")
            return {
                "Codecs: " .. (codecs ~= "" and codecs or "none"),
                "Containers: " .. (containers ~= "" and containers or "none"),
                "Direct HTTP Streaming: " .. (mc.direct_http_streaming and "Yes" or "No"),
                "Max Bit Depth: " .. tostring(mc.max_bit_depth or 16) .. "-bit",
                "Max Channels: " .. tostring(mc.max_channels or 2) .. " (Stereo)",
                "Range Seeking: " .. (mc.range_seeking and "Yes" or "No"),
                "HLS / DASH: " .. ((mc.hls or mc.dash) and "Yes" or "No"),
            }
        end
    },
    {
        title = "Playback & Transport APIs",
        get_rows = function()
            return {
                "plugin.play_file(path)",
                "plugin.play_list(paths, [start_idx])",
                "plugin.play_remote(track)",
                "plugin.queue_remote_list(tracks, [start_idx])",
                "plugin.toggle_pause()",
                "plugin.stop()",
                "plugin.next_track()",
                "plugin.prev_track()",
                "plugin.seek(seconds)",
                "plugin.set_volume(0..100)",
                "plugin.is_playing() -> bool",
                "plugin.is_paused() -> bool",
                "plugin.get_position() -> sec",
                "plugin.get_duration() -> sec",
                "plugin.get_now_playing() -> title, artist, album, duration",
                "plugin.get_play_mode() -> str",
                "plugin.get_current_track_path() -> str",
            }
        end
    },
    {
        title = "Parametric EQ (PEQ) APIs",
        get_rows = function()
            return {
                "plugin.eq_load_profile(path) -> bool",
                "plugin.eq_save_profile(path) -> bool",
                "plugin.eq_reset()",
                "plugin.eq_set_bypass(bool)",
                "plugin.eq_set_preamp(gain_db)",
                "plugin.eq_set_band(band, freq, gain, q)",
                "plugin.eq_set_band_type(band, type)",
                "plugin.eq_set_band_enabled(band, bool)",
            }
        end
    },
    {
        title = "UI & Interaction APIs",
        get_rows = function()
            return {
                "plugin.show_list(title, items, on_select, [opts])",
                "plugin.show_settings_list(title, items)",
                "plugin.show_text_input(title, initial, password, cb)",
                "plugin.show_toast(msg)",
                "plugin.register_list_item(list_id, label, on_open, [opts])",
                "plugin.register_stream_media_tile(label, on_open, [icon])",
                "plugin.set_icon(slot, path)",
                "plugin.set_background_color(slot, rgb)",
                "plugin.set_text_color(slot, rgb)",
            }
        end
    },
    {
        title = "Filesystem & Playlists APIs",
        get_rows = function()
            return {
                "plugin.sd_root() -> str",
                "plugin.list_dir(path) -> table",
                "plugin.mkdir(path) -> bool, err",
                "plugin.playlist_list() -> table",
                "plugin.playlist_read(m3u_path) -> table",
                "plugin.playlist_create(name, song_path) -> m3u_path",
                "plugin.playlist_add(m3u_path, song_path) -> bool",
                "plugin.playlist_remove(m3u_path, song_path) -> bool",
                "plugin.playlist_delete(m3u_path) -> bool",
            }
        end
    },
    {
        title = "Storage & Secrets APIs",
        get_rows = function()
            return {
                "plugin.storage.get(key, [default]) -> val",
                "plugin.storage.set(key, val) -> bool",
                "plugin.storage.delete(key) -> bool",
                "plugin.storage.list([prefix]) -> table",
                "plugin.secrets.set(key, val) -> bool",
                "plugin.secrets.exists(key) -> bool",
                "plugin.secrets.delete(key) -> bool",
            }
        end
    },
    {
        title = "Networking & Async APIs",
        get_rows = function()
            return {
                "plugin.http_request(opts, callback)",
                "plugin.download_file_async(url, dest, [tls], cb)",
                "plugin.cancel(handle)",
                "plugin.http_get(url, [verify_tls]) -> status, body",
                "plugin.http_post(url, body, [content_type], [verify_tls]) -> status, body",
            }
        end
    },
    {
        title = "Library & Database APIs",
        get_rows = function()
            return {
                "plugin.library_song_count() -> int",
                "plugin.library_get_songs([offset], [limit], [filters])",
                "  -> songs, total",
                "plugin.library_search(query, [limit]) -> songs",
                "plugin.library_get_song(song_id) -> table",
                "plugin.library_get_artists([offset], [limit]) -> table",
                "plugin.library_get_albums([offset], [limit], [artist])",
                "plugin.get_artist_albums(artist) -> table",
                "plugin.get_album_tracks(artist, album) -> table",
                "plugin.get_next_album_tracks(artist, album) -> table",
                "plugin.refresh_library() -> bool, status",
            }
        end
    },
    {
        title = "Automation, Events & Data",
        get_rows = function()
            return {
                "plugin.define(manifest)",
                "plugin.api_version() -> int",
                "plugin.has_capability(name) -> bool",
                "plugin.get_app_info() -> table",
                "plugin.on(event, handler)",
                "plugin.set_interval(sec, handler) -> handle",
                "plugin.clear_interval(handle)",
                "plugin.json_decode(str, [limits]) -> table",
                "plugin.json_encode(table, [limits]) -> str",
                "plugin.md5(str) -> hex_str",
            }
        end
    },
}

local function open_category(cat)
    local rows = cat.get_rows()
    plugin.show_list(cat.title, rows, function(index)
        plugin.show_toast(rows[index])
    end)
end

local function open_info()
    local menu_labels = {}
    for i, cat in ipairs(CATEGORIES) do
        menu_labels[i] = cat.title
    end
    plugin.show_list("Plugin API Info", menu_labels, function(index)
        open_category(CATEGORIES[index])
    end)
end

plugin.register_list_item("system", "Plugin API Info", open_info)
