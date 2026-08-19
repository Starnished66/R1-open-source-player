plugin.define({ id = "example.lastfm_scrobbler", name = "Last.fm Scrobbler", version = "1.1", api_min = 1 })

-- Last.fm scrobbler. Reference implementation for plugin.on(), set_interval()/
-- clear_interval(), get_now_playing(), async http_request(), md5(), and
-- show_text_input() together (see PLUGINS.md).
--
-- Requires a free Last.fm API account: https://www.last.fm/api/account/create
-- Fill in API_KEY/API_SECRET below with the values from that page before
-- using this plugin -- these are tied to a developer account and can't be
-- shipped pre-filled.
--
-- Auth uses Last.fm's "mobile session" method (username + password, signed,
-- sent once) rather than the browser-based token/approval flow, since this
-- device has no browser to approve a token with. The password is sent once
-- to obtain a session key; only that session key is persisted afterward.

local API_KEY = "YOUR_LASTFM_API_KEY"
local API_SECRET = "YOUR_LASTFM_API_SECRET"

local API_URL = "https://ws.audioscrobbler.com/2.0/"
local STATE_PATH = plugin.sd_root() .. "/.plugins/.lastfm_scrobbler_state"

local function read_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return { enabled = false, session_key = nil, username = nil } end
    local enabled_line = f:read("*l")
    local session_line = f:read("*l")
    local username_line = f:read("*l")
    f:close()
    return {
        enabled = enabled_line == "1",
        session_key = (session_line and session_line ~= "") and session_line or nil,
        username = (username_line and username_line ~= "") and username_line or nil,
    }
end

local function write_state(state)
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write(state.enabled and "1" or "0", "\n", state.session_key or "", "\n", state.username or "")
    f:close()
end

local state = read_state()

-- Percent-encodes everything except unreserved characters -- standard
-- x-www-form-urlencoded body encoding. Last.fm accepts %20 for space same
-- as '+', so no special-casing needed there.
local function url_encode(value)
    return (tostring(value):gsub("([^%w%-%.%_%~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

local function build_query(params)
    local parts = {}
    for k, v in pairs(params) do
        table.insert(parts, url_encode(k) .. "=" .. url_encode(v))
    end
    return table.concat(parts, "&")
end

-- Last.fm's api_sig scheme: sort every param (excluding api_sig itself) by
-- key, concatenate as key1value1key2value2..., append the shared secret,
-- MD5 the result.
local function sign(params)
    local keys = {}
    for k in pairs(params) do table.insert(keys, k) end
    table.sort(keys)

    local concat = ""
    for _, k in ipairs(keys) do
        concat = concat .. k .. tostring(params[k])
    end
    concat = concat .. API_SECRET
    return plugin.md5(concat)
end

-- Adds api_key + api_sig to params (nil-valued fields, like an empty album,
-- are simply absent from a Lua table and get skipped by both sign() and
-- build_query() automatically) and POSTs to the Last.fm REST endpoint.
local function api_call(params, callback)
    params.api_key = API_KEY
    params.api_sig = sign(params)
    return plugin.http_request({
        url = API_URL,
        method = "POST",
        body = build_query(params),
        content_type = "application/x-www-form-urlencoded",
        verify_tls = true,
        max_response_bytes = 262144,
    }, callback)
end

local login_in_flight = false

local function do_login(username, password)
    if login_in_flight then return end
    login_in_flight = true
    plugin.show_toast("Logging in to Last.fm...")
    local handle, start_error = api_call(
        { method = "auth.getMobileSession", username = username, password = password },
        function(status, body, request_error)
            login_in_flight = false
            if not request_error and status == 200 and body and body:match('status="ok"') then
                local key = body:match("<key>([^<]+)</key>")
                if key then
                    state.session_key = key
                    state.username = username
                    write_state(state)
                    plugin.show_toast("Logged in to Last.fm as " .. username)
                    return
                end
            end

            local api_error = body and body:match("<error[^>]*>([^<]+)</error>")
            local detail = api_error or request_error or (status and ("HTTP " .. status))
            plugin.show_toast("Last.fm login failed" .. (detail and (": " .. detail) or ""))
        end
    )
    if not handle then
        login_in_flight = false
        plugin.show_toast("Could not start Last.fm login: " .. (start_error or "unknown error"))
    end
end

local function start_login()
    if API_KEY == "YOUR_LASTFM_API_KEY" or API_SECRET == "YOUR_LASTFM_API_SECRET" then
        plugin.show_toast("Configure API_KEY and API_SECRET first")
        return
    end
    local ok, input_error = plugin.show_text_input("Last.fm Username", state.username, false, function(username)
        if username == "" then return end
        local password_ok, password_error = plugin.show_text_input("Last.fm Password", nil, true, function(password)
            if password == "" then return end
            do_login(username, password)
        end)
        if not password_ok then plugin.show_toast(password_error or "Text input unavailable") end
    end)
    if not ok then plugin.show_toast(input_error or "Text input unavailable") end
end

-- Current-track bookkeeping, refreshed on every "track_started" event --
-- see the plugin.on() call near the bottom of this file.
local current_title, current_artist, current_album, current_duration = nil, nil, nil, 0
local track_start_time = 0
local scrobbled_this_track = false
local scrobble_in_flight = false
local track_generation = 0

local function update_now_playing()
    local handle, start_error = api_call({
        method = "track.updateNowPlaying",
        sk = state.session_key,
        track = current_title,
        artist = current_artist,
        album = (current_album ~= "" and current_album) or nil,
        duration = tostring(math.floor(current_duration)),
    }, function(status, body, request_error)
        -- Now-playing is advisory. Avoid interrupting playback with transient
        -- network errors, but surface an immediate pool/start failure below.
    end)
    if not handle then plugin.show_toast("Last.fm update failed: " .. (start_error or "request busy")) end
end

local function scrobble()
    if scrobble_in_flight then return end
    scrobble_in_flight = true
    local generation = track_generation
    local handle, start_error = api_call({
        method = "track.scrobble",
        sk = state.session_key,
        track = current_title,
        artist = current_artist,
        album = (current_album ~= "" and current_album) or nil,
        timestamp = tostring(track_start_time),
        duration = tostring(math.floor(current_duration)),
    }, function(status, body, request_error)
        if generation ~= track_generation then return end
        scrobble_in_flight = false
        if not request_error and status == 200 and body and body:match('status="ok"') then
            scrobbled_this_track = true
        end
        -- A failure deliberately leaves scrobbled_this_track false, allowing
        -- the 15-second timer to retry instead of silently losing the play.
    end)
    if not handle then
        scrobble_in_flight = false
        plugin.show_toast("Could not scrobble: " .. (start_error or "request busy"))
    end
end

plugin.on("track_started", function(title, artist, album, duration_seconds)
    current_title, current_artist, current_album, current_duration = title, artist, album, duration_seconds
    track_start_time = os.time()
    scrobbled_this_track = false
    scrobble_in_flight = false
    track_generation = track_generation + 1

    if state.enabled and state.session_key then
        update_now_playing()
    end
end)

-- Last.fm's own scrobble rule: a track under 30s is never scrobbled; a
-- longer one scrobbles once it's been played past 50% or 4 minutes,
-- whichever comes first. Checking against get_position() (rather than, say,
-- real elapsed clock time since track_start_time) naturally respects
-- pausing -- position stops advancing while paused, so there's no need to
-- separately subscribe to "paused"/"resumed" for this to be correct.
plugin.set_interval(15, function()
    if not (state.enabled and state.session_key and current_title) then return end
    if scrobbled_this_track then return end
    if not plugin.is_playing() then return end
    if current_duration < 30 then return end

    local threshold = math.min(current_duration / 2, 240)
    if plugin.get_position() >= threshold then
        scrobble()
    end
end)

local function open_menu()
    local rows = {
        {
            type = "toggle",
            label = "Enabled",
            value = state.enabled,
            on_change = function(new_value)
                state.enabled = new_value
                write_state(state)
            end,
        },
    }

    if state.session_key then
        table.insert(rows, {
            type = "row",
            label = "Log Out (" .. (state.username or "logged in") .. ")",
            on_select = function()
                state.session_key = nil
                state.username = nil
                write_state(state)
                plugin.show_toast("Logged out of Last.fm")
            end,
        })
    else
        table.insert(rows, {
            type = "row",
            label = "Log In",
            on_select = start_login,
        })
    end

    plugin.show_settings_list("Last.fm Scrobbler", rows)
end

plugin.register_list_item("playback", "Last.fm Scrobbler", open_menu)
