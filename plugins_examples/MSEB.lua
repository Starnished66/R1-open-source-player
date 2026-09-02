plugin.define({ id = "example.mseb", name = "MSEB", version = "1.0", api_min = 1 })

-- "MSEB" -- an intuitive, mood-based tone-tuning screen on top of this
-- app's own 10-band parametric EQ (src/audio/peq.c), adds an "MSEB" row to
-- Settings -> Playback (plugin.register_list_item(), same hook
-- SoundProfiles.lua already uses for its own row).
--
-- Modeled after the stock HiBy R1 firmware's own "MSEB" (MageSound 8-Ball)
-- feature, reverse-engineered from a decompile of the stock binary: that
-- process recovered the real list and order of its 10 named tuning sliders
-- (confirmed directly in the stock binary's own strings), but NOT the actual
-- frequency/gain mapping formulas -- no such formulas exist anywhere in the
-- decompiled material or the raw strings, only unconfirmed prose guesses.
-- So the 10 axis names/order below are the real, recovered shape of the
-- feature; the mapping onto our own PEQ bands is this plugin's own original
-- design, tuned by ear, not a port of the real firmware's DSP.
--
-- Persists like every other example plugin in this folder: plain files
-- under .plugins/, re-applied at the top of this script on every boot (see
-- SoundProfiles.lua's own header comment for why).

-- Matches peq.c's own set_defaults() exactly (ISO-standard 10-band layout,
-- band 0 a low shelf, band 9 a high shelf, the rest peaking bells) -- same
-- tables SoundProfiles.lua already declares for the same reason.
local BAND_FREQS = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 }
local BAND_Q     = { 0.2, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.2 }
local BAND_TYPE  = { "low_shelf", "peaking", "peaking", "peaking", "peaking",
                      "peaking", "peaking", "peaking", "peaking", "high_shelf" }

-- Each axis is -100..100. `contrib` lists which PEQ band(s) (1-based, matching
-- plugin.eq_set_band()'s own convention) it drives and by how much at full
-- deflection. Only band 1 and band 10 are shared by two axes each (Sound
-- Temperature is a deliberate whole-spectrum tilt, so it shares both end
-- shelves with the dedicated Bass Extension/Air controls) -- every other
-- band belongs to exactly one axis. Shared bands get their contributions
-- summed (see apply_band() below), not overwritten.
local AXES = {
    { key = "temperature",  label = "Sound Temperature",  contrib = { { band = 1, db = 3 }, { band = 10, db = -4 } } },
    { key = "bass_ext",     label = "Bass Extension",     contrib = { { band = 1, db = 6 } } },
    { key = "bass_texture", label = "Bass Texture",       contrib = { { band = 2, db = 5 } } },
    { key = "thickness",    label = "Note Thickness",     contrib = { { band = 4, db = 5 } } },
    { key = "vocal_pos",    label = "Vocal Position",     contrib = { { band = 5, db = 5 } } },
    { key = "female_vocal", label = "Female Vocal",       contrib = { { band = 6, db = 4 } } },
    { key = "instruments",  label = "Instrument Presence", contrib = { { band = 3, db = 3 }, { band = 7, db = 4 } } },
    { key = "bass_bite",    label = "Bass Bite",          contrib = { { band = 8, db = 4 } } },
    { key = "treble_bite",  label = "Treble Bite",        contrib = { { band = 9, db = 4 } } },
    { key = "air",          label = "Air",                contrib = { { band = 10, db = 5 } } },
}

local function axis_by_key(key)
    for _, a in ipairs(AXES) do
        if a.key == key then return a end
    end
    return nil
end

local PLUGIN_DIR = plugin.sd_root() .. "/.plugins"
local STATE_PATH = PLUGIN_DIR .. "/.mseb_state"
-- Snapshot of whatever manual PEQ curve was live right before MSEB was
-- first enabled -- see set_enabled() below. Written/read only via
-- plugin.eq_save_profile()/eq_load_profile(), never parsed here directly.
local BACKUP_PATH = PLUGIN_DIR .. "/.mseb_pre_backup.peq"

local function slot_path(n)
    return PLUGIN_DIR .. "/.mseb_slot" .. n
end

-- Reads a plain key=value\n file into a table via repeated single-line
-- reads (same io pattern SoundProfiles.lua's own read_state() uses) --
-- deliberately not f:lines(), to stay within the exact io usage already
-- demonstrated as safe in this plugin sandbox.
local function read_kv_file(path)
    local out = {}
    local f = io.open(path, "r")
    if not f then return out, false end
    local line = f:read("*l")
    while line do
        local k, v = line:match("^([%w_]+)=(.-)$")
        if k then out[k] = v end
        line = f:read("*l")
    end
    f:close()
    return out, true
end

local function file_exists(path)
    local f = io.open(path, "r")
    if f then f:close() return true end
    return false
end

local values = {}
for _, a in ipairs(AXES) do values[a.key] = 0 end
local enabled = false

do
    local kv, found = read_kv_file(STATE_PATH)
    if found then
        enabled = (kv.enabled == "1")
        for _, a in ipairs(AXES) do
            if kv[a.key] then values[a.key] = tonumber(kv[a.key]) or 0 end
        end
    end
end

local function write_state()
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write("enabled=" .. (enabled and "1" or "0") .. "\n")
    for _, a in ipairs(AXES) do
        f:write(a.key .. "=" .. tostring(values[a.key]) .. "\n")
    end
    f:close()
end

-- Recomputes ONE band from every axis currently contributing to it and
-- issues exactly one plugin.eq_set_band() call for it. plugin.eq_set_band()
-- calls peq_save() internally (a full atomic fsync-heavy write) on every
-- call -- callers below deliberately call this only for the specific
-- band(s) a changed axis actually targets, never all 10 in a loop from a
-- single slider release, to avoid a 10x version of the settings-save-lag
-- bug already found and fixed twice elsewhere in this app.
local function apply_band(index)
    if not enabled then return end
    local gain = 0
    for _, axis in ipairs(AXES) do
        for _, c in ipairs(axis.contrib) do
            if c.band == index then
                gain = gain + (values[axis.key] / 100.0) * c.db
            end
        end
    end
    if gain > 12 then gain = 12 elseif gain < -12 then gain = -12 end
    plugin.eq_set_band(index, BAND_FREQS[index], gain, BAND_Q[index])
    plugin.eq_set_band_type(index, BAND_TYPE[index])
    plugin.eq_set_band_enabled(index, gain ~= 0)
end

-- Only the band(s) this one axis owns -- the cheap, per-slider-release path.
local function apply_axis_by_key(key)
    local axis = axis_by_key(key)
    if not axis then return end
    for _, c in ipairs(axis.contrib) do
        apply_band(c.band)
    end
end

-- All 10 bands -- only for rare, deliberate actions (enable, reset, load slot).
local function apply_all()
    for i = 1, 10 do apply_band(i) end
end

local function set_enabled(new_enabled)
    if new_enabled == enabled then return end
    if new_enabled then
        -- Snapshot the current (manual) PEQ curve before the first
        -- apply_all() overwrites it -- but only if no backup already exists.
        -- An existing backup means a PRIOR enable was never cleanly turned
        -- off (e.g. the app was killed while MSEB was on); that file still
        -- holds the true pre-MSEB curve and must not be clobbered with
        -- whatever the bands currently hold.
        if not file_exists(BACKUP_PATH) then
            plugin.eq_save_profile(BACKUP_PATH)
        end
        enabled = true
        apply_all()
    else
        if file_exists(BACKUP_PATH) then
            if plugin.eq_load_profile(BACKUP_PATH) then
                os.remove(BACKUP_PATH)
            end
        end
        enabled = false
    end
    write_state()
end

local function reset_defaults()
    for _, a in ipairs(AXES) do values[a.key] = 0 end
    write_state()
    if enabled then apply_all() end
end

local function save_slot(n)
    local f = io.open(slot_path(n), "w")
    if not f then return end
    for _, a in ipairs(AXES) do
        f:write(a.key .. "=" .. tostring(values[a.key]) .. "\n")
    end
    f:close()
end

-- Slots store the 10 raw axis values, not the derived PEQ curve, so a later
-- tweak to the mapping table above still re-derives correctly from an old
-- saved slot instead of freezing whatever the mapping happened to produce
-- at save time.
local function load_slot(n)
    local kv, found = read_kv_file(slot_path(n))
    if not found then
        plugin.show_toast("Slot " .. n .. " is empty")
        return
    end
    for _, a in ipairs(AXES) do
        if kv[a.key] then values[a.key] = tonumber(kv[a.key]) or 0 end
    end
    write_state()
    if enabled then apply_all() end
    plugin.show_toast("Slot " .. n .. " loaded")
end

if enabled then apply_all() end

local function open_group(title, entries)
    local rows = {}
    for _, e in ipairs(entries) do
        local key, label = e[1], e[2]
        table.insert(rows, {
            type = "slider",
            label = label,
            min = -100,
            max = 100,
            value = values[key],
            on_change = function(v)
                values[key] = v
                write_state()
                apply_axis_by_key(key)
            end,
        })
    end
    plugin.show_settings_list(title, rows)
end

-- Grouped into 3 sub-screens (3/4/3 sliders) rather than one 10-slider
-- screen: plugin.show_settings_list() silently drops any slider past
-- PLUGIN_SETTINGS_LIST_MAX_SLIDERS (4) in a single call.
plugin.register_list_item("playback", "MSEB", function()
    plugin.show_settings_list("MSEB", {
        {
            type = "toggle",
            label = "Enabled",
            value = enabled,
            on_change = function(v) set_enabled(v) end,
        },
        {
            type = "row",
            label = "Bass & Warmth",
            on_select = function()
                open_group("Bass & Warmth", {
                    { "temperature", "Sound Temperature" },
                    { "bass_ext", "Bass Extension" },
                    { "bass_texture", "Bass Texture" },
                })
            end,
        },
        {
            type = "row",
            label = "Vocals & Instruments",
            on_select = function()
                open_group("Vocals & Instruments", {
                    { "thickness", "Note Thickness" },
                    { "vocal_pos", "Vocal Position" },
                    { "female_vocal", "Female Vocal" },
                    { "instruments", "Instrument Presence" },
                })
            end,
        },
        {
            type = "row",
            label = "Treble & Air",
            on_select = function()
                open_group("Treble & Air", {
                    { "bass_bite", "Bass Bite" },
                    { "treble_bite", "Treble Bite" },
                    { "air", "Air" },
                })
            end,
        },
        {
            type = "row",
            label = "Reset to Defaults",
            on_select = function()
                reset_defaults()
                plugin.show_toast("MSEB reset")
            end,
        },
        {
            type = "row",
            label = "Save to Slot",
            on_select = function()
                plugin.show_list("Save to Slot", { "Slot 1", "Slot 2", "Slot 3" }, function(index)
                    save_slot(index)
                    plugin.show_toast("Saved to Slot " .. index)
                end)
            end,
        },
        {
            type = "row",
            label = "Load from Slot",
            on_select = function()
                plugin.show_list("Load from Slot", { "Slot 1", "Slot 2", "Slot 3" }, function(index)
                    load_slot(index)
                end)
            end,
        },
    })
end)
