--[[
Qobuz streaming -- DRAFT plugin, unverified against a live Qobuz account.

Built against this player's provider-neutral remote-music-provider plugin
API (plugin.play_remote()/plugin.queue_remote_list(), api_min 3) -- see
PLUGINS.md's "plugin.play_remote()" section and ISSUES.md's remote-provider
design notes, which named Qobuz as the intended first target specifically
because it can hand back a plain, unencrypted FLAC/MP3 URL to an
authenticated client. That's a real, load-bearing difference from services
like Spotify or (per ISSUES.md's own note) Tidal: there is no DRM/stream
encryption to work around here, only an unofficial API surface.

IMPORTANT, read before using this:
  Qobuz has no officially published third-party developer API. Every
  existing open-source Qobuz client (streamrip, qobuz-dl, and others)
  authenticates against the same private endpoints the official apps use,
  with an app_id/app_secret pair Qobuz does not issue to third parties.
  This plugin follows that same established request shape below, but the
  exact field names, signing order, and response layout have NOT been
  re-verified against Qobuz's live API this session -- they match the
  commonly-documented pattern as of when this was written. Qobuz can and
  does change this without notice since it's unofficial. If a call fails,
  toast the HTTP status/body first (every callback below does) before
  assuming the whole approach needs rework.

  You need your own app_id/app_secret pair filled in below. Qobuz doesn't
  issue these to third-party apps, so sourcing a pair is on you and
  deliberately out of scope for this file -- this plugin only implements
  the client side once you have one. This is meant for using your own paid
  Qobuz subscription from this player, the same way this app's existing
  Subsonic integration uses your own self-hosted server credentials.

  Playback goes through plugin.play_remote(), which (per its own doc
  section) has no seeking, no reconnect-on-drop, and no expiring-URL
  refresh yet -- a dropped connection mid-track has no automatic recovery,
  and this draft only resolves one track at a time (see "Search" below for
  why queue_remote_list isn't used).
]]

plugin.define({
    id = "community.qobuz",
    name = "Qobuz",
    version = "0.1.0",
    api_min = 3, -- needs plugin.play_remote()/plugin.queue_remote_list()
})

if not plugin.has_capability("playback.remote") then
    plugin.show_toast("Qobuz plugin needs a newer player build (playback.remote)")
    return
end

-- TODO: fill in your own app_id/app_secret pair (see the file header above).
local QOBUZ_APP_ID = ""
local QOBUZ_APP_SECRET = ""

local API_BASE = "https://www.qobuz.com/api.json/0.2"

-- Qobuz format_id: 5 = MP3 320, 6 = FLAC 16-bit/44.1kHz ("CD"), 7 = FLAC
-- 24-bit up to 96kHz, 27 = FLAC 24-bit up to 192kHz. Every output path this
-- app has (internal DAC, USB DAC, Bluetooth) is fixed 16-bit
-- (plugin.media_capabilities().max_bit_depth), so Hi-Res would only cost
-- bandwidth/data for bit depth this hardware discards anyway -- pick
-- Hi-Res only if a future build actually reports a higher cap.
local FORMAT_ID = (plugin.media_capabilities().max_bit_depth or 16) > 16 and 27 or 6

--------------------------------------------------------------------------
-- Small helpers
--------------------------------------------------------------------------

local function url_encode(text)
    return (tostring(text):gsub("[^%w%-%.%_%~]", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

-- plugin.secrets has no get() (by design -- see PLUGINS.md's "plugin.storage
-- / plugin.secrets" section: set/exists/delete only, deliberately no way to
-- enumerate OR read a secret back into Lua). That makes it unusable for a
-- token this plugin needs to actually attach to later requests, so the
-- session token is kept in plugin.storage instead. Per that same section,
-- storage and secrets share the identical on-disk protection (owner-only
-- permissions plus the io.open() path guard against other plugins) -- the
-- only real difference is secrets also hides key names via no list(), which
-- doesn't matter for the one fixed key name used here.
local function save_session(auth_token, user_id)
    plugin.storage.set("auth_token", auth_token)
    plugin.storage.set("user_id", tostring(user_id or ""))
end

local function load_session()
    local token = plugin.storage.get("auth_token")
    if not token or token == "" then return nil end
    return token
end

local function clear_session()
    plugin.storage.delete("auth_token")
    plugin.storage.delete("user_id")
    plugin.show_toast("Logged out of Qobuz")
end

--------------------------------------------------------------------------
-- Login
--------------------------------------------------------------------------

local function do_login(email, password)
    local url = API_BASE .. "/user/login"
        .. "?app_id=" .. url_encode(QOBUZ_APP_ID)
        .. "&username=" .. url_encode(email)
        .. "&password=" .. plugin.md5(password)

    plugin.http_request({ url = url, method = "GET", total_timeout_ms = 15000 },
        function(status, body, err)
            if err then
                plugin.show_toast("Qobuz login failed: " .. err)
                return
            end
            if status ~= 200 then
                plugin.show_toast("Qobuz login failed (HTTP " .. status .. ")")
                return
            end
            local data, decode_err = plugin.json_decode(body)
            if not data or not data.user_auth_token then
                plugin.show_toast("Qobuz login: unexpected response" ..
                                   (decode_err and (" (" .. decode_err .. ")") or ""))
                return
            end
            save_session(data.user_auth_token, data.user and data.user.id)
            plugin.show_toast("Logged in to Qobuz")
        end)
end

local function prompt_login()
    plugin.show_text_input("Qobuz email", nil, false, function(email)
        if not email or email == "" then return end
        plugin.show_text_input("Qobuz password", nil, true, function(password)
            if not password or password == "" then return end
            do_login(email, password)
        end)
    end)
end

--------------------------------------------------------------------------
-- Streaming URL resolution
--------------------------------------------------------------------------

-- Historically-documented Qobuz getFileUrl signing scheme (community
-- clients, not an official spec -- see file header): md5 of the
-- concatenation "trackgetFileUrlformat_id" .. format_id .. "intent" ..
-- intent .. "track_id" .. track_id .. request_ts .. app_secret.
local function signed_file_url_request(track_id, auth_token)
    local request_ts = tostring(os.time())
    local to_sign = "trackgetFileUrlformat_id" .. FORMAT_ID
        .. "intentstreamtrack_id" .. track_id .. request_ts .. QOBUZ_APP_SECRET
    local signature = plugin.md5(to_sign)

    return API_BASE .. "/track/getFileUrl"
        .. "?app_id=" .. url_encode(QOBUZ_APP_ID)
        .. "&user_auth_token=" .. url_encode(auth_token)
        .. "&track_id=" .. url_encode(track_id)
        .. "&format_id=" .. FORMAT_ID
        .. "&intent=stream"
        .. "&request_ts=" .. request_ts
        .. "&request_sig=" .. signature
end

local function play_track(track, auth_token)
    local url = signed_file_url_request(track.id, auth_token)
    plugin.http_request({ url = url, total_timeout_ms = 15000 }, function(status, body, err)
        if err then
            plugin.show_toast("Qobuz stream lookup failed: " .. err)
            return
        end
        if status ~= 200 then
            plugin.show_toast("Qobuz stream lookup failed (HTTP " .. status .. ")")
            return
        end
        local data = plugin.json_decode(body)
        if not data or not data.url then
            plugin.show_toast("Qobuz: track unavailable in this quality/region")
            return
        end

        local codec = "mp3"
        if data.mime_type and tostring(data.mime_type):find("flac") then codec = "flac" end

        local artwork = ""
        if track.album and track.album.image then
            artwork = track.album.image.large or track.album.image.small or ""
        end

        plugin.play_remote({
            provider = "qobuz",
            track_id = tostring(track.id),
            stream_url = data.url,
            title = track.title or "Unknown title",
            artist = (track.performer and track.performer.name)
                or (track.album and track.album.artist and track.album.artist.name)
                or "Unknown artist",
            album = track.album and track.album.title or "",
            duration_ms = (track.duration or 0) * 1000,
            artwork_url = artwork,
            codec = codec,
            sample_rate = math.floor(((data.sampling_rate or 44.1) * 1000) + 0.5),
            bit_depth = data.bit_depth or 16,
        })
    end)
end

--------------------------------------------------------------------------
-- Search
--------------------------------------------------------------------------

local function show_results(tracks, auth_token)
    if #tracks == 0 then
        plugin.show_toast("No Qobuz results")
        return
    end

    local labels = {}
    for i, track in ipairs(tracks) do
        local artist = (track.performer and track.performer.name) or "Unknown artist"
        labels[i] = (track.title or "Unknown title") .. " -- " .. artist
    end

    plugin.show_list("Qobuz Results", labels, function(index)
        play_track(tracks[index], auth_token)
    end)
end

local function run_search(query, auth_token)
    local url = API_BASE .. "/catalog/search"
        .. "?app_id=" .. url_encode(QOBUZ_APP_ID)
        .. "&query=" .. url_encode(query)
        .. "&type=tracks"
        .. "&limit=25"

    plugin.http_request({ url = url, total_timeout_ms = 15000 }, function(status, body, err)
        if err then
            plugin.show_toast("Qobuz search failed: " .. err)
            return
        end
        if status ~= 200 then
            plugin.show_toast("Qobuz search failed (HTTP " .. status .. ")")
            return
        end
        local data = plugin.json_decode(body)
        if not data or not data.tracks or not data.tracks.items then
            plugin.show_toast("Qobuz: unexpected search response")
            return
        end
        show_results(data.tracks.items, auth_token)
    end)
end

local function prompt_search(auth_token)
    plugin.show_text_input("Search Qobuz", nil, false, function(query)
        if not query or query == "" then return end
        run_search(query, auth_token)
    end)
end

--------------------------------------------------------------------------
-- Entry point
--------------------------------------------------------------------------

local function open_qobuz()
    if QOBUZ_APP_ID == "" or QOBUZ_APP_SECRET == "" then
        plugin.show_toast("Qobuz plugin: fill in QOBUZ_APP_ID/QOBUZ_APP_SECRET first")
        return
    end

    local auth_token = load_session()
    if not auth_token then
        prompt_login()
        return
    end

    plugin.show_list("Qobuz", { "Search", "Log out" }, function(index)
        if index == 1 then
            prompt_search(auth_token)
        else
            clear_session()
        end
    end)
end

plugin.register_stream_media_tile("Qobuz", open_qobuz)
