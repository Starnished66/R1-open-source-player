plugin.define({ id = "example.audiobooks", name = "Audiobooks", version = "2.0", api_min = 1 })

-- Audiobook library for <SD>/Audiobooks. Version 2 adds durable per-book
-- resume, Continue Listening, natural chapter ordering, completion state,
-- periodic progress saves, and position bookmarks using only API 1.

local ROOT = plugin.sd_root() .. "/Audiobooks"
local STATE_PATH = plugin.sd_root() .. "/.plugins/.audiobooks_state_v2"
local state = {}
local pending_seek = nil
local last_saved_position = -1

local function encode(s)
    return (tostring(s or ""):gsub("([^%w%._%- ])", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

local function decode(s)
    return ((s or ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

local function split_tabs(line)
    local out = {}
    for value in (line .. "\t"):gmatch("(.-)\t") do out[#out + 1] = value end
    return out
end

local function load_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return end
    for line in f:lines() do
        local p = split_tabs(line)
        if p[1] == "P" and p[2] then
            local key = decode(p[2])
            state[key] = state[key] or { bookmarks = {} }
            local s = state[key]
            s.chapter = decode(p[3])
            s.position = tonumber(p[4]) or 0
            s.duration = tonumber(p[5]) or 0
            s.finished = p[6] == "1"
            s.last_played = tonumber(p[7]) or 0
        elseif p[1] == "B" and p[2] then
            local key = decode(p[2])
            state[key] = state[key] or { bookmarks = {} }
            local s = state[key]
            s.bookmarks = s.bookmarks or {}
            s.bookmarks[#s.bookmarks + 1] = {
                chapter = decode(p[3]), position = tonumber(p[4]) or 0, label = decode(p[5]),
            }
        end
    end
    f:close()
end

local function save_state()
    local tmp = STATE_PATH .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false end
    for key, s in pairs(state) do
        f:write("P\t", encode(key), "\t", encode(s.chapter), "\t", tostring(math.floor(s.position or 0)),
            "\t", tostring(math.floor(s.duration or 0)), "\t", s.finished and "1" or "0", "\t",
            tostring(s.last_played or 0), "\n")
        for _, b in ipairs(s.bookmarks or {}) do
            f:write("B\t", encode(key), "\t", encode(b.chapter), "\t", tostring(math.floor(b.position or 0)),
                "\t", encode(b.label), "\n")
        end
    end
    f:close()
    os.remove(STATE_PATH)
    return os.rename(tmp, STATE_PATH) ~= nil
end

local function is_audio(name)
    local ext = name:match("%.([%a%d]+)$")
    if not ext then return false end
    ext = ext:lower()
    return ext == "mp3" or ext == "m4a" or ext == "m4b" or ext == "flac" or ext == "ogg"
        or ext == "opus" or ext == "wav" or ext == "aac" or ext == "aiff" or ext == "ape" or ext == "wma"
end

local function natural_key(s)
    return s:lower():gsub("%d+", function(n) return string.format("%012d", tonumber(n)) end)
end

local function natural_less(a, b) return natural_key(a.name or a) < natural_key(b.name or b) end

local function chapters_for(key)
    local dir = ROOT .. "/" .. key
    local chapters = {}
    for _, e in ipairs(plugin.list_dir(dir)) do
        if not e.dir and is_audio(e.name) then
            chapters[#chapters + 1] = { name = e.name, path = dir .. "/" .. e.name }
        end
    end
    table.sort(chapters, natural_less)
    return chapters
end

local function path_info(path)
    if not path then return nil end
    local prefix = ROOT .. "/"
    if path:sub(1, #prefix) ~= prefix then return nil end
    local relative = path:sub(#prefix + 1)
    local key, chapter = relative:match("^([^/]+)/(.+)$")
    if not key or not chapter then return nil end
    return key, chapter
end

local function format_time(seconds)
    seconds = math.max(0, math.floor(seconds or 0))
    local h, m, s = math.floor(seconds / 3600), math.floor(seconds / 60) % 60, seconds % 60
    if h > 0 then return string.format("%d:%02d:%02d", h, m, s) end
    return string.format("%d:%02d", m, s)
end

local function progress_label(key)
    local s = state[key]
    if not s or not s.chapter or s.chapter == "" then return key end
    if s.finished then return key .. "  [Finished]" end
    return key .. "  [" .. format_time(s.position) .. "]"
end

local function play_at(key, chapter_name, position)
    local chapters = chapters_for(key)
    if #chapters == 0 then plugin.show_toast("No chapters found in " .. key); return end
    local paths, index = {}, 1
    for i, chapter in ipairs(chapters) do
        paths[i] = chapter.path
        if chapter.name == chapter_name then index = i end
    end
    pending_seek = position and position > 2 and position or nil
    state[key] = state[key] or { bookmarks = {} }
    state[key].finished = false
    state[key].last_played = os.time()
    save_state()
    plugin.play_list(paths, index)
end

local function add_bookmark()
    local key, chapter = path_info(plugin.get_current_track_path())
    if not key then plugin.show_toast("Play an audiobook before adding a bookmark"); return end
    local position = plugin.get_position()
    local s = state[key] or { bookmarks = {} }
    state[key] = s
    s.bookmarks = s.bookmarks or {}
    s.bookmarks[#s.bookmarks + 1] = {
        chapter = chapter, position = position, label = chapter .. " @ " .. format_time(position),
    }
    save_state()
    plugin.show_toast("Bookmark added at " .. format_time(position))
end

local function open_bookmarks(key)
    local marks = (state[key] and state[key].bookmarks) or {}
    if #marks == 0 then plugin.show_toast("No bookmarks for " .. key); return end
    local labels = {}
    for i, b in ipairs(marks) do labels[i] = b.label end
    plugin.show_list("Bookmarks", labels, function(index)
        local b = marks[index]
        play_at(key, b.chapter, b.position)
    end, { width = 440 })
end

local function open_chapters(key)
    local chapters = chapters_for(key)
    if #chapters == 0 then plugin.show_toast("No chapters found in " .. key); return end
    local labels = {}
    local saved = state[key]
    for i, chapter in ipairs(chapters) do
        labels[i] = (saved and saved.chapter == chapter.name and "▶ " or "") .. chapter.name
    end
    plugin.show_list(key, labels, function(index) play_at(key, chapters[index].name, 0) end, { width = 448 })
end

local function open_book(key)
    local s = state[key]
    local rows = {}
    if s and s.chapter and s.chapter ~= "" and not s.finished then
        rows[#rows + 1] = { type = "row", label = "Resume at " .. format_time(s.position),
            on_select = function() play_at(key, s.chapter, s.position) end }
    end
    rows[#rows + 1] = { type = "row", label = "Chapters", on_select = function() open_chapters(key) end }
    rows[#rows + 1] = { type = "row", label = "Add Current Bookmark", on_select = add_bookmark }
    rows[#rows + 1] = { type = "row", label = "Bookmarks", on_select = function() open_bookmarks(key) end }
    rows[#rows + 1] = { type = "toggle", label = "Finished", value = s and s.finished or false,
        on_change = function(value)
            state[key] = state[key] or { bookmarks = {} }
            state[key].finished = value
            save_state()
        end }
    plugin.show_settings_list(key, rows)
end

local function discover_books()
    local books = {}
    for _, e in ipairs(plugin.list_dir(ROOT)) do
        if e.dir then books[#books + 1] = e.name end
    end
    table.sort(books, function(a, b) return natural_key(a) < natural_key(b) end)
    return books
end

local function open_continue()
    local books = {}
    for key, s in pairs(state) do
        if s.chapter and s.chapter ~= "" and not s.finished then books[#books + 1] = key end
    end
    table.sort(books, function(a, b) return (state[a].last_played or 0) > (state[b].last_played or 0) end)
    if #books == 0 then plugin.show_toast("No books in progress"); return end
    local labels = {}
    for i, key in ipairs(books) do labels[i] = progress_label(key) end
    plugin.show_list("Continue Listening", labels, function(index) open_book(books[index]) end, { width = 448 })
end

local function open_library()
    local books = discover_books()
    if #books == 0 then plugin.show_toast("Add book folders under Audiobooks on the SD card"); return end
    local labels = { "Continue Listening" }
    for _, key in ipairs(books) do labels[#labels + 1] = progress_label(key) end
    plugin.show_list("Audiobooks", labels, function(index)
        if index == 1 then open_continue() else open_book(books[index - 1]) end
    end, { width = 448 })
end

local function save_current_progress(force)
    local path = plugin.get_current_track_path()
    local key, chapter = path_info(path)
    if not key then return end
    local position, duration = plugin.get_position(), plugin.get_duration()
    if not force and math.abs(position - last_saved_position) < 5 then return end
    local s = state[key] or { bookmarks = {} }
    state[key] = s
    s.chapter, s.position, s.duration = chapter, position, duration
    s.last_played = os.time()
    if duration > 0 and position >= duration - 3 then
        local chapters = chapters_for(key)
        if #chapters > 0 and chapters[#chapters].name == chapter then s.finished = true end
    else
        s.finished = false
    end
    last_saved_position = position
    save_state()
end

load_state()

plugin.on("track_started", function()
    last_saved_position = -1
    if pending_seek then
        local seek_to = pending_seek
        pending_seek = nil
        plugin.seek(seek_to)
    end
    save_current_progress(true)
end)
plugin.on("paused", function() save_current_progress(true) end)
plugin.on("stopped", function() save_current_progress(true) end)
plugin.set_interval(10, function() save_current_progress(false) end)

-- No layout overrides here: an entry inserted into a native list should
-- inherit that list's exact row dimensions and sprite styling.
plugin.register_list_item("books", "Audiobooks", open_library)
