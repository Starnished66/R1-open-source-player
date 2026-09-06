plugin.define({ id = "example.lastfm_scrobbler", name = "Last.fm Scrobbler", version = "1.3", api_min = 1 })

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
-- The queue is intentionally DISK-BACKED. No offline history is loaded into
-- RAM. Each qualified track is appended as one line to QUEUE_PATH.
-- When connectivity returns, at most 50 records are read into RAM for a single
-- Last.fm batch request, then those submitted records are removed from disk.
-- This keeps RAM use essentially constant even after very long offline periods.

local QUEUE_BATCH_SIZE = 50
local QUEUE_RETENTION_SECONDS = 13 * 24 * 60 * 60
local queue_sync_in_flight = false

local function queue_encode(value)
  return url_encode(value or "")
end

local function queue_decode(value)
  value = value or ""
  return value:gsub("%%(%x%x)", function(hex)
    return string.char(tonumber(hex, 16))
  end)
end

local function purge_expired_queue_records()
  local cutoff = os.time() - QUEUE_RETENTION_SECONDS
  local input = io.open(QUEUE_PATH, "r")
  if not input then return end

  local output = io.open(QUEUE_TMP_PATH, "w")
  if not output then
    input:close()
    return
  end

  for line in input:lines() do
    local timestamp = line:match("^([^\t]*)\t")
    local ts = tonumber(timestamp or "0")
    if ts > 0 and ts >= cutoff then
      output:write(line, "\n")
    end
  end

  input:close()
  output:close()
  os.rename(QUEUE_TMP_PATH, QUEUE_PATH)
end


local function append_scrobble_to_disk(timestamp, artist, title, album, duration)
  purge_expired_queue_records()
  if timestamp <= 0 or artist == "" or title == "" then return false end

  local f = io.open(QUEUE_PATH, "a")
  if not f then return false end

  f:write(
    tostring(timestamp), "\t",
    tostring(duration or 0), "\t",
    queue_encode(artist), "\t",
    queue_encode(title), "\t",
    queue_encode(album or ""), "\n"
  )
  f:close()
  return true
end

-- Reads only the first QUEUE_BATCH_SIZE records. The complete offline queue
-- never exists in RAM at once.
local function read_queue_batch()
  local batch = {}
  local f = io.open(QUEUE_PATH, "r")
  if not f then return batch end

  for line in f:lines() do
    local timestamp, duration, artist, title, album =
        line:match("^([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)$")

    if timestamp and duration and artist and title and album then
      table.insert(batch, {
        timestamp = tonumber(timestamp) or 0,
        duration = tonumber(duration) or 0,
        artist = queue_decode(artist),
        title = queue_decode(title),
        album = queue_decode(album),
      })

      if #batch >= QUEUE_BATCH_SIZE then
        break
      end
    end
  end

  f:close()
  return batch
end

local function queue_has_data()
  local f = io.open(QUEUE_PATH, "r")
  if not f then return false end

  local line = f:read("*l")
  f:close()
  return line ~= nil and line ~= ""
end

-- Remove exactly the first count records without loading the remainder into
-- memory. The records after the submitted batch stay on disk untouched.
local function remove_first_queue_records(count)
  if count <= 0 then return true end

  local input = io.open(QUEUE_PATH, "r")
  if not input then return false end

  local output = io.open(QUEUE_TMP_PATH, "w")
  if not output then
    input:close()
    return false
  end

  local skipped = 0
  for line in input:lines() do
    if skipped < count then
      skipped = skipped + 1
    else
      output:write(line, "\n")
    end
  end

  input:close()
  output:close()

  -- If fewer records existed than expected, removing everything is still
  -- correct: those are the records that were submitted successfully.
  local ok = os.rename(QUEUE_TMP_PATH, QUEUE_PATH)
  return ok ~= nil
end

local function sync_queue()
  -- Last.fm does not reliably accept very old timestamps, so keep only the
  -- last 13 days of offline history on disk.
  purge_expired_queue_records()

  if queue_sync_in_flight then return end
  if not (state.enabled and state.session_key) then return end
  if not queue_has_data() then return end

  local batch = read_queue_batch()
  if #batch == 0 then return end

  -- Last.fm supports up to 50 scrobbles in one track.scrobble request.
  local params = {
    method = "track.scrobble",
    sk = state.session_key,
  }

  for i, item in ipairs(batch) do
    params["track[" .. (i - 1) .. "]"] = item.title
    params["artist[" .. (i - 1) .. "]"] = item.artist
    if item.album ~= "" then
      params["album[" .. (i - 1) .. "]"] = item.album
    end
    params["timestamp[" .. (i - 1) .. "]"] = tostring(item.timestamp)
    params["duration[" .. (i - 1) .. "]"] = tostring(math.floor(item.duration))
  end

  queue_sync_in_flight = true

  local handle = api_call(params, function(status, body, request_error)
    queue_sync_in_flight = false

    if not request_error and status == 200 and body and body:match('status="ok"') then
      -- Last.fm has processed the entire submitted batch. Some individual
      -- entries may be reported as ignored (for example because of age or
      -- invalid metadata), but they must still be removed or they would
      -- be retried forever. Entries not part of this batch remain on disk.
      remove_first_queue_records(#batch)
    end
    -- Any transport/API failure leaves the entire batch on disk unchanged.
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
local track_queued_this_track = false
local track_generation = 0

local function update_now_playing()
  local handle = api_call({
    method = "track.updateNowPlaying",
    sk = state.session_key,
    track = current_title,
    artist = current_artist,
    album = (current_album ~= "" and current_album) or nil,
    duration = tostring(math.floor(current_duration)),
  }, function(status, body, request_error)
    -- Now-playing is advisory. Network errors are intentionally ignored.
  end)
  -- A failed now-playing request must never affect playback or queueing.
  return handle
end

local function queue_current_track()
  if track_queued_this_track then return true end
  if not current_title or not current_artist then return false end

  local ok = append_scrobble_to_disk(
    track_start_time,
    current_artist,
    current_title,
    current_album or "",
    current_duration
  )

  if ok then
    track_queued_this_track = true
  end
  return ok
end

plugin.on("track_started", function(title, artist, album, duration_seconds)
  current_title, current_artist, current_album, current_duration = title, artist, album, duration_seconds
  track_start_time = os.time()
  scrobbled_this_track = false
  track_queued_this_track = false
  track_generation = track_generation + 1

  if state.enabled and state.session_key then
    update_now_playing()
    sync_queue()
  end
end)

-- Last.fm's own scrobble rule: a track under 30s is never scrobbled; a
-- longer one scrobbles once it's been played past 50% or 4 minutes,
-- whichever comes first. Checking against get_position() naturally respects
-- pausing because playback position stops advancing while paused.
plugin.set_interval(15, function()
  if not state.enabled then return end

  -- Always try to drain previously cached offline scrobbles first.
  sync_queue()

  if not (state.session_key and current_title) then return end
  if scrobbled_this_track or track_queued_this_track then return end
  if not plugin.is_playing() then return end
  if current_duration < 30 then return end

  local threshold = math.min(current_duration / 2, 240)
  if plugin.get_position() >= threshold then
    -- Persist before attempting the network submission. This guarantees
    -- that a play is not lost when connectivity disappears at the exact
    -- moment the scrobble would normally be sent.
    if queue_current_track() then
      -- It is now safely cached. Try uploading immediately if possible.
      sync_queue()
    end
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
