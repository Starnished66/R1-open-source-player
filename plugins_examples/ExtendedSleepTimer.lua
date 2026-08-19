plugin.define({
    id = "example.extended_sleep_timer",
    name = "Extended Sleep Timer",
    version = "1.0.0",
    api_min = 1,
})

-- An extended, plugin-owned sleep timer with a maximum of three hours.
-- The preferred duration persists; the armed countdown deliberately does
-- not survive a player restart, matching the native sleep timer's behavior.

local STATE_PATH = plugin.sd_root() .. "/.plugins/.extended_sleep_timer_state"
local MIN_MINUTES = 15
local MAX_MINUTES = 180

local duration_minutes = 60
local armed = false
local deadline = 0

local function clamp(value)
    value = math.floor(tonumber(value) or 60)
    if value < MIN_MINUTES then return MIN_MINUTES end
    if value > MAX_MINUTES then return MAX_MINUTES end
    return value
end

local function load_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return end
    duration_minutes = clamp(f:read("*l"))
    f:close()
end

local function save_state()
    local tmp = STATE_PATH .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then
        plugin.show_toast("Could not save sleep timer duration")
        return
    end
    f:write(tostring(duration_minutes), "\n")
    f:close()
    os.remove(STATE_PATH)
    os.rename(tmp, STATE_PATH)
end

local function format_duration(minutes)
    local hours = math.floor(minutes / 60)
    local remainder = minutes % 60
    if hours == 0 then return string.format("%d min", remainder) end
    if remainder == 0 then return string.format("%d hr", hours) end
    return string.format("%d hr %d min", hours, remainder)
end

local function remaining_seconds()
    if not armed then return 0 end
    return math.max(0, deadline - os.time())
end

local function format_remaining(seconds)
    seconds = math.max(0, math.floor(seconds))
    local hours = math.floor(seconds / 3600)
    local minutes = math.floor(seconds / 60) % 60
    local secs = seconds % 60
    if hours > 0 then return string.format("%d:%02d:%02d", hours, minutes, secs) end
    return string.format("%d:%02d", minutes, secs)
end

local function set_armed(value)
    armed = value
    if armed then
        deadline = os.time() + duration_minutes * 60
        plugin.show_toast("Sleep timer set for " .. format_duration(duration_minutes))
    else
        deadline = 0
        plugin.show_toast("Extended sleep timer cancelled")
    end
end

local function show_status()
    if not armed then
        plugin.show_toast("Extended sleep timer is off")
        return
    end
    plugin.show_toast("Time remaining: " .. format_remaining(remaining_seconds()))
end

local function open_about()
    plugin.show_list("About Extended Timer", {
        "Stops playback when the countdown reaches zero.",
        "Choose any duration from 15 minutes to 3 hours.",
        "Changing duration while armed restarts the countdown.",
        "The countdown resets when the player restarts.",
    }, function() end, { width = 448 })
end

local function open_settings()
    plugin.show_settings_list("Extended Sleep Timer", {
        {
            type = "toggle",
            label = "Timer Enabled",
            value = armed,
            on_change = set_armed,
        },
        {
            type = "slider",
            label = "Duration (minutes)",
            min = MIN_MINUTES,
            max = MAX_MINUTES,
            value = duration_minutes,
            on_change = function(value)
                duration_minutes = clamp(value)
                save_state()
                if armed then
                    deadline = os.time() + duration_minutes * 60
                    plugin.show_toast("Timer restarted: " .. format_duration(duration_minutes))
                else
                    plugin.show_toast("Duration: " .. format_duration(duration_minutes))
                end
            end,
        },
        {
            type = "row",
            label = "Show Time Remaining",
            on_select = show_status,
        },
        {
            type = "row",
            label = "How It Works",
            on_select = open_about,
        },
    })
end

load_state()

plugin.set_interval(1, function()
    if not armed or remaining_seconds() > 0 then return end
    armed = false
    deadline = 0
    plugin.stop()
    plugin.show_toast("Sleep timer finished")
end)

plugin.register_list_item("playback", "Extended Sleep Timer", open_settings)
