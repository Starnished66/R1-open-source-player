-- Reference implementation for plugin.register_list_item("playback", ...)
-- and plugin.show_settings_list() (see PLUGINS.md): adds a "Loudness Boost"
-- row to Settings -> Playback, opening a nested submenu with a real toggle
-- switch, a real slider, and a nested row (a submenu inside a submenu).
--
-- The toggle/slider directly drive plugin.eq_set_preamp() -- the same
-- whole-EQ preamp the native Equalizer screen's own Preamp slider controls
-- -- so turning this on and adjusting the amount here will visibly move
-- that same slider on the native EQ screen too (and vice versa: adjusting
-- the native preamp while this is "on" will look like it silently reverted
-- next time this plugin re-applies its own value). That shared-state
-- interaction is expected and left as-is here rather than papering over it
-- with extra bookkeeping -- this file's job is demonstrating the API
-- surface, not shipping a polished "loudness boost that plays nicely with
-- manual EQ tweaks" feature.

local STATE_PATH = plugin.sd_root() .. "/.plugins/.playback_extras_state"

local function read_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return { enabled = false, boost_db = 4 } end
    local enabled_line = f:read("*l")
    local boost_line = f:read("*l")
    f:close()
    return {
        enabled = enabled_line == "1",
        boost_db = tonumber(boost_line) or 4,
    }
end

local function write_state(state)
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write(state.enabled and "1" or "0", "\n", tostring(state.boost_db))
    f:close()
end

local state = read_state()

local function apply_state()
    plugin.eq_set_preamp(state.enabled and state.boost_db or 0)
end

apply_state()

local function open_about_menu()
    plugin.show_settings_list("About Loudness Boost", {
        {
            type = "row",
            label = "What does this do?",
            on_select = function()
                plugin.show_toast("Raises the whole-EQ preamp by the chosen amount while enabled.")
            end,
        },
    })
end

local function open_menu()
    plugin.show_settings_list("Loudness Boost", {
        {
            type = "toggle",
            label = "Enable Boost",
            value = state.enabled,
            on_change = function(new_value)
                state.enabled = new_value
                write_state(state)
                apply_state()
            end,
        },
        {
            type = "slider",
            label = "Boost Amount (dB)",
            min = 0,
            max = 12,
            value = state.boost_db,
            on_change = function(new_value)
                state.boost_db = new_value
                write_state(state)
                apply_state()
            end,
        },
        {
            type = "row",
            label = "About",
            on_select = open_about_menu,
        },
    })
end

plugin.register_list_item("playback", "Loudness Boost", open_menu)
