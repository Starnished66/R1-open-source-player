plugin.define({ id = "example.lastfm_scrobbler", name = "Last.fm Scrobbler", version = "1.5", api_min = 1 })

-- Last.fm scrobbler with persistent offline scrobbling.
-- Qualified tracks are written to a small local queue before upload. The queue
-- survives restarts and is drained silently whenever Last.fm is reachable.

local API_KEY = "YOUR_LASTFM_API_KEY"
local API_SECRET = "YOUR_LASTFM_API_SECRET"

local API_URL = "https://ws.audioscrobbler.com/2.0/"
local STATE_PATH = plugin.sd_root() .. "/.plugins/.lastfm_scrobbler_state"
local QUEUE_PATH = plugin.sd_root() .. "/.plugins/.lastfm_scrobbler_queue"
local QUEUE_TMP_PATH = QUEUE_PATH .. ".tmp"

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
-- as '+', so no special-casing is needed here.
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
-- then MD5 the result.
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

-- Adds api_key + api_sig to params and POSTs to the Last.fm REST endpoint.
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

-- --------------------------------------------------------------------------
-- Persistent offline scrobble queue
-- --------------------------------------------------------------------------
-- Disk-backed queue. It is intentionally NOT loaded into a Lua table, so a
-- large offline backlog does not consume the R1's 64 MB RAM.
-- Queue record format (one per line):
-- timestamp<TAB>duration<TAB>artist<TAB>title<TAB>album
-- String fields are percent-encoded for safe line storage.

local queue_sync_in_flight = false
local QUEUE_MAX_AGE = 13 * 24 * 60 * 60

local function queue_encode(value)
  return url_encode(value or "")
end

local function queue_decode(value)
  value = value or ""
  return value:gsub("%%(%x%x)", function(hex)
    return string.char(tonumber(hex, 16))
  end)
end

local function parse_queue_line(line)
  local timestamp, duration, artist, title, album = line:match("^([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)$")
  if not timestamp then return nil end
  timestamp = tonumber(timestamp)
  duration = tonumber(duration)
  if not timestamp or timestamp <= 0 then return nil end
  return {
    timestamp = timestamp,
    duration = duration or 0,
    artist = queue_decode(artist),
    title = queue_decode(title),
    album = queue_decode(album),
  }
end

local function queue_line(item)
  return tostring(item.timestamp) .. "\t"
      .. tostring(item.duration) .. "\t"
      .. queue_encode(item.artist) .. "\t"
      .. queue_encode(item.title) .. "\t"
      .. queue_encode(item.album) .. "\n"
end

-- Remove entries older than 13 days without loading the queue into RAM.
-- A temporary file is built line-by-line and then atomically swapped in.
local function prune_queue()
  local f = io.open(QUEUE_PATH, "r")
  if not f then return true end

  local tmp = io.open(QUEUE_TMP_PATH, "w")
  if not tmp then
    f:close()
    return false
  end

  local cutoff = os.time() - QUEUE_MAX_AGE
  for line in f:lines() do
    local item = parse_queue_line(line)
    if item and item.timestamp >= cutoff then
      tmp:write(queue_line(item))
    end
  end

  f:close()
  tmp:close()

  if not os.rename(QUEUE_TMP_PATH, QUEUE_PATH) then
    os.remove(QUEUE_TMP_PATH)
    return false
  end
  return true
end

local function enqueue_scrobble(timestamp, artist, title, album, duration)
  if not timestamp or timestamp <= 0 or artist == "" or title == "" then
    return false
  end

  -- Keep the queue bounded by age before appending the new item.
  prune_queue()

  local f = io.open(QUEUE_PATH, "a")
  if not f then return false end
  f:write(queue_line({
    timestamp = timestamp,
    duration = duration or 0,
    artist = artist or "",
    title = title or "",
    album = album or "",
  }))
  f:close()
  return true
end

-- Read only the first valid queued item. The rest of the queue remains on
-- disk and never enters Lua memory.
local function read_first_queued()
  local f = io.open(QUEUE_PATH, "r")
  if not f then return nil end

  for line in f:lines() do
    local item = parse_queue_line(line)
    if item then
      f:close()
      return item
    end
  end

  f:close()
  return nil
end

-- Remove exactly the first queued record by streaming the remaining records
-- to a temporary file. No queue-sized Lua table is created.
local function remove_first_queued()
  local f = io.open(QUEUE_PATH, "r")
  if not f then return true end

  local tmp = io.open(QUEUE_TMP_PATH, "w")
  if not tmp then
    f:close()
    return false
  end

  local removed = false
  for line in f:lines() do
    if not removed and parse_queue_line(line) then
      removed = true
    else
      tmp:write(line, "\n")
    end
  end

  f:close()
  tmp:close()

  if not os.rename(QUEUE_TMP_PATH, QUEUE_PATH) then
    os.remove(QUEUE_TMP_PATH)
    return false
  end
  return true
end

local function sync_queue()
  if queue_sync_in_flight then return end
  if not (state.enabled and state.session_key) then return end

  prune_queue()
  local item = read_first_queued()
  if not item then return end

  queue_sync_in_flight = true

  local handle, start_error = api_call({
    method = "track.scrobble",
    sk = state.session_key,
    track = item.title,
    artist = item.artist,
    album = (item.album ~= "" and item.album) or nil,
    timestamp = tostring(item.timestamp),
    duration = tostring(math.floor(item.duration)),
  }, function(status, body, request_error)
    queue_sync_in_flight = false

    if not request_error and status == 200 and body and body:match('status="ok"') then
      -- Delete only after Last.fm accepted the queued record.
      remove_first_queued()
      return
    end

    -- Leave it untouched on network/API failure. The next retry will use the
    -- exact original timestamp and metadata.
  end)

  if not handle then
    queue_sync_in_flight = false
  end
end

-- --------------------------------------------------------------------------
-- Current-track bookkeeping
-- --------------------------------------------------------------------------
local current_title, current_artist, current_album, current_duration = nil, nil, nil, 0
local track_start_time = 0
local scrobbled_this_track = false
local scrobble_request_in_flight = false
local track_generation = 0

local function update_now_playing()
  if not (state.enabled and state.session_key) then return nil end
  if not current_title or not current_artist then return nil end
  return api_call({
    method = "track.updateNowPlaying",
    sk = state.session_key,
    track = current_title,
    artist = current_artist,
    album = (current_album ~= "" and current_album) or nil,
    duration = tostring(math.floor(current_duration)),
  }, function(status, body, request_error)
    -- Now-playing is advisory; failures must never affect playback or queueing.
  end)
end

local function queue_current_track()
  if not current_title or not current_artist then return false end
  return enqueue_scrobble(
    track_start_time,
    current_artist,
    current_title,
    current_album or "",
    current_duration
  )
end

local function scrobble_current_track()
  if scrobbled_this_track or scrobble_request_in_flight then return end
  if not (state.enabled and state.session_key) then return end
  if not current_title or not current_artist then return end

  local generation = track_generation
  scrobble_request_in_flight = true

  local handle, start_error = api_call({
    method = "track.scrobble",
    sk = state.session_key,
    track = current_title,
    artist = current_artist,
    album = (current_album ~= "" and current_album) or nil,
    timestamp = tostring(track_start_time),
    duration = tostring(math.floor(current_duration)),
  }, function(status, body, request_error)
    scrobble_request_in_flight = false

    -- Ignore a late callback from an older track.
    if generation ~= track_generation then return end

    if not request_error and status == 200 and body and body:match('status="ok"') then
      scrobbled_this_track = true
      return
    end

    -- The direct scrobble failed. Persist it now with its original start
    -- timestamp, then let the normal queue sync retry it later.
    if queue_current_track() then
      scrobbled_this_track = true
    end
  end)

  if not handle then
    scrobble_request_in_flight = false
    -- Could not even start the request: treat it as offline and cache it.
    if queue_current_track() then
      scrobbled_this_track = true
    end
  end
end

plugin.on("track_started", function(title, artist, album, duration_seconds)
  current_title, current_artist, current_album, current_duration = title, artist, album, duration_seconds
  track_start_time = os.time()
  scrobbled_this_track = false
  scrobble_request_in_flight = false
  track_generation = track_generation + 1

  if state.enabled and state.session_key then
    update_now_playing()
    sync_queue()
  end
end)

-- Last.fm's own scrobble rule: a track under 30s is never scrobbled; a
-- longer one scrobbles once it has been played past 50% or 4 minutes,
-- whichever comes first. get_position() follows actual playback position,
-- so pausing does not incorrectly advance the threshold.
plugin.set_interval(15, function()
  if not state.enabled then return end

  -- Drain old offline records first whenever connectivity is available.
  sync_queue()

  if not (state.session_key and current_title) then return end
  if scrobbled_this_track or scrobble_request_in_flight then return end
  if not plugin.is_playing() then return end
  if current_duration < 30 then return end

  local threshold = math.min(current_duration / 2, 240)
  if plugin.get_position() >= threshold then
    -- Try the normal direct Last.fm path first. Only if that request fails do
    -- we write the play to the persistent offline queue.
    scrobble_current_track()
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
        if state.enabled and state.session_key then
          sync_queue()
        end
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
