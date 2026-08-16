-- Sound profile switcher, adds a "Sound Profile" row to Settings.
-- Reference implementation for the plugin.eq_*() API (see PLUGINS.md):
-- ships a few curated EQ presets on top of this app's own 10-band
-- parametric EQ (src/audio/peq.c), switchable with one tap, persisted
-- across reboots the same way every other example plugin in this folder
-- persists its own state -- a plain file under .plugins/, re-applied at
-- the top of this script on every boot.

local STATE_PATH = plugin.sd_root() .. "/.plugins/.eq_profile_state"

-- Matches peq.c's own set_defaults() exactly (ISO-standard 10-band layout,
-- band 0 a low shelf, band 9 a high shelf, the rest peaking bells) so a
-- profile only needs to say which bands it touches and by how much -- every
-- other band stays at this app's own normal default frequency/Q, just like
-- Flat does.
local BAND_FREQS = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 }
local BAND_Q     = { 0.2, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.2 }
local BAND_TYPE  = { "low_shelf", "peaking", "peaking", "peaking", "peaking",
                      "peaking", "peaking", "peaking", "peaking", "high_shelf" }

-- gains is sparse: only the bands a profile actually boosts/cuts (1-based,
-- matching plugin.eq_set_band()'s own convention) need an entry -- every
-- other band is left disabled at its default gain (0dB), the same "skip
-- disabled bands" fast path peq_process() already takes.
local PROFILES = {
    { key = "bass_boost",   name = "Bass Boost",   gains = { [1] = 6, [2] = 4, [3] = 2 } },
    { key = "vocal",        name = "Vocal",         gains = { [1] = -2, [6] = 3, [7] = 4, [8] = 3 } },
    { key = "treble_boost", name = "Treble Boost", gains = { [8] = 3, [9] = 4, [10] = 5 } },
}

local function profile_path(key)
    return plugin.sd_root() .. "/.plugins/.eq_" .. key .. ".peq"
end

local function read_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return "flat" end
    local s = f:read("*l")
    f:close()
    return s or "flat"
end

local function write_state(key)
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write(key)
    f:close()
end

-- Generates profile's .peq file the first time it's ever selected (peq.c
-- has no "build a profile without applying it live" API -- eq_save_profile()
-- just persists whatever's currently active -- so generating one means
-- briefly driving the live EQ through eq_set_band(), then snapshotting it).
-- Every later selection just loads the already-generated file. Returns the
-- path either way.
local function ensure_profile_file(profile)
    local path = profile_path(profile.key)
    local f = io.open(path, "r")
    if f then
        f:close()
        return path
    end

    plugin.eq_reset()
    for i = 1, 10 do
        local gain = profile.gains[i] or 0
        plugin.eq_set_band(i, BAND_FREQS[i], gain, BAND_Q[i])
        plugin.eq_set_band_type(i, BAND_TYPE[i])
        plugin.eq_set_band_enabled(i, gain ~= 0)
    end
    plugin.eq_save_profile(path)
    return path
end

local function apply_profile(key)
    if key == "flat" then
        plugin.eq_reset()
        return
    end
    for _, p in ipairs(PROFILES) do
        if p.key == key then
            local path = ensure_profile_file(p)
            if not plugin.eq_load_profile(path) then
                plugin.eq_reset() -- file vanished/unreadable -- fall back to flat rather than leaving the previous profile active
            end
            return
        end
    end
    plugin.eq_reset() -- unrecognized saved state (e.g. an old version's key) -- same fallback
end

local current_key = read_state()
apply_profile(current_key)

plugin.register_list_item("settings", "Sound Profile", function()
    local names = { "Flat (Default)" }
    for _, p in ipairs(PROFILES) do table.insert(names, p.name) end

    plugin.show_list("Sound Profile", names, function(index)
        local key = (index == 1) and "flat" or PROFILES[index - 1].key
        if key == current_key then return end
        current_key = key
        write_state(key)
        apply_profile(key)
        plugin.show_toast("Sound profile applied")
    end)
end)
