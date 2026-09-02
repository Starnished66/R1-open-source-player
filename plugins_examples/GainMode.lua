plugin.define({ id = "example.gain_mode", name = "Gain Mode", version = "1.0", api_min = 1 })

-- Low/High Gain switcher, adds a "Gain Mode" row to Settings -> Playback.
-- Reference implementation for plugin.set_hw_volume_curve() (PLUGIN API
-- 11, see PLUGINS.md) -- reproduces the R1's own stock firmware High/Low
-- Gain curves exactly, extracted from a real device's own
-- /usr/resource/ot_devices.json (VOLUMES[0].Gains[], names "HDB"/"LDB"),
-- not re-derived or guessed. Each is a raw hardware register value
-- (0-255, lower = louder, 0 = the DAC's own loudest point) for UI volume
-- 0%..100%; a uniform 12-step/6dB gap separates High from Low across
-- nearly the whole range, converging back to the same register (0) at
-- 100% -- see PLUGINS.md's own comment on why that means every gain mode
-- reaches the same physical peak loudness, just via a different curve
-- shape below it. The real firmware also defines a third "MDB" (Medium)
-- curve, but it's numerically identical to Low in the real device dump
-- this was extracted from, so it's left out here as a pointless third
-- option -- add it back if a different device/firmware revision turns out
-- to define it distinctly. If you have your own curve (e.g. tuned for a
-- specific pair of IEMs), replace either table below with your own 101
-- values -- that's the whole point of this API being a plugin primitive
-- rather than a fixed native setting.
--
-- api_min is deliberately 1, not 11 (the API version that actually added
-- plugin.set_hw_volume_curve()) -- api_min=11 would make plugin.define()
-- itself raise a native load error and abort the whole script on any
-- older player build, before the has_capability() check below ever ran,
-- making that check's own "needs a newer player build" fallback toast
-- unreachable dead code. Gating purely on has_capability() (what it is
-- for) instead lets this plugin load everywhere and degrade gracefully.
--
-- Also does NOT default a first-time install to either Gain Mode: an
-- unset/corrupted saved state leaves the app's own built-in volume taper
-- untouched until the user explicitly opens Settings -> Playback -> Gain
-- Mode and picks one -- silently defaulting to High (or even Low) would
-- still be an unrequested, potentially startling loudness change on first
-- boot for whatever headphones happen to already be plugged in.

local MODES = {
    {
        key = "high", name = "High Gain",
        curve = {
            255, 176, 166, 156, 146, 142, 138, 134, 130, 126,
            124, 122, 120, 118, 116, 114, 112, 110, 108, 106,
            104, 102, 100, 98, 96, 94, 92, 90, 88, 86,
            84, 82, 80, 78, 76, 74, 72, 70, 68, 66,
            64, 62, 60, 58, 56, 55, 54, 53, 52, 51,
            50, 49, 48, 47, 46, 45, 44, 43, 42, 41,
            40, 39, 38, 37, 36, 35, 34, 33, 32, 31,
            30, 29, 28, 27, 26, 25, 24, 23, 22, 21,
            20, 19, 18, 17, 16, 15, 14, 13, 12, 11,
            10, 9, 8, 7, 6, 5, 4, 3, 2, 1,
            0,
        },
    },
    {
        key = "low", name = "Low Gain",
        curve = {
            255, 188, 178, 168, 158, 154, 150, 146, 142, 138,
            136, 134, 132, 130, 128, 126, 124, 122, 120, 118,
            116, 114, 112, 110, 108, 106, 104, 102, 100, 98,
            96, 94, 92, 90, 88, 86, 84, 82, 80, 78,
            76, 74, 72, 70, 68, 67, 66, 65, 64, 63,
            62, 61, 60, 59, 58, 57, 56, 55, 54, 53,
            52, 51, 50, 49, 48, 47, 46, 45, 44, 43,
            42, 41, 40, 39, 38, 37, 36, 35, 34, 33,
            32, 31, 30, 29, 28, 27, 26, 25, 24, 23,
            22, 21, 20, 19, 18, 17, 16, 15, 14, 13,
            0,
        },
    },
}

local STATE_PATH = plugin.sd_root() .. "/.plugins/.gain_mode_state"

-- nil (not a string default) means "never explicitly chosen" -- both "no
-- state file yet" (first install) and "an unrecognized saved key" (a
-- corrupted file, or an old version's key this version dropped) collapse
-- to that same nil rather than silently picking a mode for the user.
local function read_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return nil end
    local s = f:read("*l")
    f:close()
    return s
end

local function write_state(key)
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write(key)
    f:close()
end

local function find_mode(key)
    if not key then return nil end
    for _, m in ipairs(MODES) do
        if m.key == key then return m end
    end
    return nil
end

if plugin.has_capability("audio.hw_volume_curve") then
    local current_mode = find_mode(read_state())
    if current_mode then
        plugin.set_hw_volume_curve(current_mode.curve)
    end -- else: leave the app's own built-in taper alone until a mode is chosen below

    plugin.register_list_item("playback", "Gain Mode", function()
        local names = {}
        local selected = 0
        for i, m in ipairs(MODES) do
            names[i] = m.name
            if current_mode and m.key == current_mode.key then selected = i end
        end

        plugin.show_list("Gain Mode", names, function(index)
            local mode = MODES[index]
            if current_mode and mode.key == current_mode.key then return end
            current_mode = mode
            write_state(mode.key)
            plugin.set_hw_volume_curve(mode.curve)
            plugin.show_toast(mode.name .. " applied")
        end, selected > 0 and { selected = selected } or nil)
    end)
else
    plugin.show_toast("Gain Mode needs a newer player build")
end
