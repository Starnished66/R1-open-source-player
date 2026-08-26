plugin.define({
    id = "example.play_through",
    name = "Play Through",
    version = "1.0.0",
    api_min = 1,
})

-- Continues playback after a playlist reaches its natural end:
--   * Play Through Albums starts the artist's next album.
--   * Play Through Folders starts the next sibling folder.
--
-- Both may be enabled. Album continuation takes priority when the current
-- file is the final track of a tagged album; folder continuation is the
-- fallback. An explicit Stop cancels the pending continuation. The two-poll
-- idle confirmation avoids racing the player's normal gapless/playlist
-- auto-advance, which can briefly report a transition between tracks.

local STATE_PATH = plugin.sd_root() .. "/.plugins/.play_through_state"
local state = { folders = false, albums = false }

local candidate_paths = nil
local stopped_polls = 0

local function load_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return end
    for line in f:lines() do
        local key, value = line:match("^([%a_]+)=(%d)$")
        if key == "folders" then state.folders = value == "1" end
        if key == "albums" then state.albums = value == "1" end
    end
    f:close()
end

local function save_state()
    local tmp = STATE_PATH .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then
        plugin.show_toast("Could not save Play Through settings")
        return
    end
    f:write("folders=", state.folders and "1" or "0", "\n")
    f:write("albums=", state.albums and "1" or "0", "\n")
    f:close()
    os.remove(STATE_PATH)
    os.rename(tmp, STATE_PATH)
end

local function natural_key(value)
    return tostring(value or ""):lower():gsub("%d+", function(number)
        return string.format("%012d", tonumber(number))
    end)
end

local function natural_less(a, b)
    return natural_key(a.name or a) < natural_key(b.name or b)
end

local function is_audio(name)
    local extension = tostring(name or ""):match("%.([%a%d]+)$")
    if not extension then return false end
    extension = extension:lower()
    local supported = {
        mp3 = true, flac = true, wav = true, aif = true, aiff = true,
        aifc = true, dsf = true, dff = true, aac = true, m4a = true,
        m4b = true, alac = true, ape = true, opus = true, ogg = true,
        wma = true,
    }
    return supported[extension] == true
end

local function split_path(path)
    if not path then return nil end
    local directory, name = path:match("^(.*)/([^/]+)$")
    if not directory or directory == "" then return nil end
    return directory, name
end

local function folder_tracks(directory)
    local tracks = {}
    for _, entry in ipairs(plugin.list_dir(directory)) do
        if not entry.dir and is_audio(entry.name) then
            tracks[#tracks + 1] = { name = entry.name, path = directory .. "/" .. entry.name }
        end
    end
    table.sort(tracks, natural_less)
    return tracks
end

local function next_folder_tracks(current_path)
    local current_directory, current_name = split_path(current_path)
    if not current_directory then return nil end

    local current_tracks = folder_tracks(current_directory)
    if #current_tracks == 0 or current_tracks[#current_tracks].name ~= current_name then return nil end

    local parent, folder_name = split_path(current_directory)
    if not parent then return nil end

    local folders = {}
    for _, entry in ipairs(plugin.list_dir(parent)) do
        if entry.dir then folders[#folders + 1] = entry end
    end
    table.sort(folders, natural_less)

    local found_current = false
    for _, folder in ipairs(folders) do
        if found_current then
            local tracks = folder_tracks(parent .. "/" .. folder.name)
            if #tracks > 0 then
                local paths = {}
                for i, track in ipairs(tracks) do paths[i] = track.path end
                return paths
            end
        elseif folder.name == folder_name then
            found_current = true
        end
    end
    return nil
end

local function same_path(a, b)
    return a ~= nil and b ~= nil and a == b
end

local function album_continuation(path, artist, album)
    if not state.albums or not artist or artist == "" or not album or album == "" then return nil end
    local current_tracks = plugin.get_album_tracks(artist, album)
    if not current_tracks or #current_tracks == 0 or not same_path(path, current_tracks[#current_tracks]) then
        return nil
    end
    return plugin.get_next_album_tracks(artist, album)
end

local function arm_continuation(_, artist, album)
    candidate_paths = nil
    stopped_polls = 0
    if plugin.get_play_mode() ~= "sequential" then return end

    local path = plugin.get_current_track_path()
    if not path or path:match("^https?://") then return end

    candidate_paths = album_continuation(path, artist, album)
    if not candidate_paths and state.folders then
        candidate_paths = next_folder_tracks(path)
    end
end

local function rearm_current_track()
    local _, artist, album = plugin.get_now_playing()
    if not artist then
        candidate_paths = nil
        stopped_polls = 0
        return
    end
    arm_continuation(nil, artist, album)
end

plugin.on("track_started", arm_continuation)

plugin.on("stopped", function()
    -- Explicit player/DAC/remote stops arrive here. Natural playlist-end is
    -- the documented event gap and is intentionally detected by the timer.
    candidate_paths = nil
    stopped_polls = 0
end)

plugin.on("resumed", function() stopped_polls = 0 end)

plugin.set_interval(1, function()
    if not candidate_paths or plugin.get_play_mode() ~= "sequential" then
        stopped_polls = 0
        return
    end
    if plugin.is_playing() or plugin.is_paused() then
        stopped_polls = 0
        return
    end

    stopped_polls = stopped_polls + 1
    if stopped_polls < 2 then return end

    local paths = candidate_paths
    candidate_paths = nil
    stopped_polls = 0
    plugin.play_list(paths, 1)
    plugin.show_toast("Continuing playback")
end)

local function open_about()
    plugin.show_list("About Play Through", {
        "Albums continue in the artist's alphabetical album order.",
        "Folders continue in natural sibling-folder order.",
        "Album mode has priority when both options are enabled.",
        "Shuffle and repeat modes are left unchanged.",
    }, function() end)
end

local function open_settings()
    plugin.show_settings_list("Play Through", {
        {
            type = "toggle",
            label = "Play Through Folders",
            value = state.folders,
            on_change = function(value)
                state.folders = value
                save_state()
                rearm_current_track()
            end,
        },
        {
            type = "toggle",
            label = "Play Through Albums",
            value = state.albums,
            on_change = function(value)
                state.albums = value
                save_state()
                rearm_current_track()
            end,
        },
        {
            type = "row",
            label = "How It Works",
            on_select = open_about,
        },
    })
end

load_state()
plugin.register_list_item("playback", "Play Through", open_settings)
