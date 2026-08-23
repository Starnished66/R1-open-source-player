plugin.define({ id = "example.home_themes", name = "Home Themes", version = "1.0", api_min = 1 })

-- Reference implementation for plugin.set_home_layout()'s full style
-- surface (PLUGINS.md): 10 ready-made Home looks, picked from a Settings
-- row, spanning both tile and list mode and every per-tile/list-wide
-- knob (bg/text color, radius, size, alignment, chevron, icon, text_size
-- including "mono", row_gap/tile_gap).
--
-- set_home_layout() only takes effect at load time (see PLUGINS.md) --
-- same constraint plugin.set_icon() has -- so, same "persisted choice,
-- re-applied on boot" pattern plugins_examples/Themes.lua already uses
-- for icon reskinning, picking a new theme here saves it and asks for a
-- restart rather than live-switching.

local STATE_PATH = plugin.sd_root() .. "/.plugins/.home_theme_state"
local KEYS = { "music", "stream_media", "wireless", "books", "system", "dac" }

local function read_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return "native" end
    local s = f:read("*l")
    f:close()
    return s or "native"
end

local function write_state(name)
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write(name)
    f:close()
end

-- Builds a tile_keys array from a per-key color table plus one shared
-- style (the "string or table" shape set_home_layout() itself uses,
-- PLUGINS.md) -- every preset below is just data, not custom layout code.
local function tiles(colors, style)
    local t = {}
    for _, key in ipairs(KEYS) do
        local row = { key = key, bg_color = colors[key][1], text_color = colors[key][2] }
        for k, v in pairs(style) do row[k] = v end
        table.insert(t, row)
    end
    return t
end

local THEMES = {}

-- 1) Game Boy -- navy pixel boot-menu: tight rows, square corners, no
-- chevron, mono font, flush left.
THEMES.gameboy = {
    label = "Game Boy",
    tiles = tiles({
        music = { 0x0f1420, 0xf4d35e }, stream_media = { 0x0f1420, 0xeaeaea },
        wireless = { 0x0f1420, 0x7fd1c9 }, books = { 0x0f1420, 0xf4d35e },
        system = { 0x0f1420, 0xee6c8b }, dac = { 0x0f1420, 0xee6c8b },
    }, { radius = 0, height = 56, align = "left", accessory = false, text_size = "mono", icon = false }),
    options = { mode = "list", row_gap = 6 },
}

-- 2) Terminal -- classic green-on-black CRT.
THEMES.terminal = {
    label = "Terminal",
    tiles = tiles({
        music = { 0x000000, 0x33ff33 }, stream_media = { 0x000000, 0x33ff33 },
        wireless = { 0x000000, 0x33ff33 }, books = { 0x000000, 0x33ff33 },
        system = { 0x000000, 0x33ff33 }, dac = { 0x000000, 0x33ff33 },
    }, { radius = 0, height = 56, align = "left", accessory = false, text_size = "mono", icon = false }),
    options = { mode = "list", row_gap = 2 },
}

-- 3) Retro -- warm amber CRT, an 80s-computer alternative to Terminal's
-- green phosphor.
THEMES.retro = {
    label = "Retro",
    tiles = tiles({
        music = { 0x140f08, 0xffb000 }, stream_media = { 0x140f08, 0xffb000 },
        wireless = { 0x140f08, 0xffb000 }, books = { 0x140f08, 0xffb000 },
        system = { 0x140f08, 0xffb000 }, dac = { 0x140f08, 0xffb000 },
    }, { radius = 0, height = 64, align = "left", accessory = false, text_size = "mono", icon = false }),
    options = { mode = "list", row_gap = 4 },
}

-- 4) Vaporwave -- synthwave sunset grid: deep purple tiles fading to hot
-- pink, big soft radius.
THEMES.vaporwave = {
    label = "Vaporwave",
    tiles = tiles({
        music = { 0x3a1c71, 0xffd1f7 }, stream_media = { 0x6a2c8f, 0xffc2ec },
        wireless = { 0x9b3aa0, 0xffe0f5 }, books = { 0xc23f9e, 0xfff0ff },
        system = { 0xe14f95, 0xfff5fb }, dac = { 0xff6f9c, 0xffffff },
    }, { radius = 26 }),
    options = { tile_gap = 10 },
}

-- 5) Monastic -- austere, calm: undyed-wool/stone tones, generous
-- centered rows, no chevron -- a "quiet room" feel.
THEMES.monastic = {
    label = "Monastic",
    tiles = tiles({
        music = { 0xe4ddd0, 0x4a4438 }, stream_media = { 0xd9cfc0, 0x4a4438 },
        wireless = { 0xe4ddd0, 0x4a4438 }, books = { 0xd9cfc0, 0x4a4438 },
        system = { 0xe4ddd0, 0x4a4438 }, dac = { 0xd9cfc0, 0x4a4438 },
    }, { radius = 6, height = 96, align = "center", accessory = false, text_size = "medium", icon = false }),
    options = { mode = "list", row_gap = 14 },
}

-- 6) Wavy -- ocean gradient tiles, teal into deep navy.
THEMES.wavy = {
    label = "Wavy",
    tiles = tiles({
        music = { 0x0a2540, 0x8ecae6 }, stream_media = { 0x0d3050, 0x90e0ef },
        wireless = { 0x103a60, 0xa8dadc }, books = { 0x134570, 0x8ecae6 },
        system = { 0x165080, 0x90e0ef }, dac = { 0x195a90, 0xa8dadc },
    }, { radius = 24 }),
    options = { tile_gap = 12 },
}

-- 7) Earthy -- rich soil/clay/moss tile grid, big radius.
THEMES.earthy = {
    label = "Earthy",
    tiles = tiles({
        music = { 0x8a5a3c, 0xf5ead9 }, stream_media = { 0x6b7c4f, 0xf5ead9 },
        wireless = { 0xa87851, 0x3a2a1e }, books = { 0x5c4632, 0xf5ead9 },
        system = { 0x7d8f5c, 0x3a2a1e }, dac = { 0x9c6b46, 0xf5ead9 },
    }, { radius = 20 }),
    options = { tile_gap = 8 },
}

-- 8) Trees -- deep forest greens and bark browns, list mode.
THEMES.trees = {
    label = "Trees",
    tiles = tiles({
        music = { 0x1b2e1f, 0xd8c9a3 }, stream_media = { 0x24391f, 0xd8c9a3 },
        wireless = { 0x1b2e1f, 0xd8c9a3 }, books = { 0x2e2418, 0xd8c9a3 },
        system = { 0x24391f, 0xd8c9a3 }, dac = { 0x1b2e1f, 0xd8c9a3 },
    }, { radius = 12, height = 84, align = "left", text_size = "medium" }),
    options = { mode = "list", row_gap = 8 },
}

-- 9) Swamp -- murky olive/moss/mud, moody and dim.
THEMES.swamp = {
    label = "Swamp",
    tiles = tiles({
        music = { 0x2a2e1c, 0x9caf7a }, stream_media = { 0x333a20, 0x8a9c6a },
        wireless = { 0x2a2e1c, 0x9caf7a }, books = { 0x3a3524, 0x8a9c6a },
        system = { 0x333a20, 0x9caf7a }, dac = { 0x2a2e1c, 0x8a9c6a },
    }, { radius = 0, height = 60, align = "left", accessory = false, text_size = "small", icon = false }),
    options = { mode = "list", row_gap = 3 },
}

-- 10) Mountain Sunset -- warm dusk-over-peaks gradient, orange into indigo.
THEMES.mountain_sunset = {
    label = "Mountain Sunset",
    tiles = tiles({
        music = { 0xff9d5c, 0x3a1a2e }, stream_media = { 0xf07858, 0x3a1a2e },
        wireless = { 0xc25b7a, 0xffe8d6 }, books = { 0x8a4f8f, 0xffe8d6 },
        system = { 0x5a4a9f, 0xffe8d6 }, dac = { 0x2f3a7a, 0xffe8d6 },
    }, { radius = 22 }),
    options = { tile_gap = 10 },
}

local ORDER = {
    "native", "gameboy", "terminal", "retro", "vaporwave",
    "monastic", "wavy", "earthy", "trees", "swamp", "mountain_sunset",
}
local LABELS = { native = "Native (Default)" }
for name, t in pairs(THEMES) do LABELS[name] = t.label end

local function apply_theme(name)
    local t = THEMES[name]
    if not t then return end -- "native" -- no set_home_layout() call, today's default stands
    plugin.set_home_layout(t.tiles, t.options)
end

local current = read_state()
apply_theme(current)

plugin.register_list_item("system", "Home Theme", function()
    local display = {}
    for _, name in ipairs(ORDER) do table.insert(display, LABELS[name]) end
    plugin.show_list("Home Theme", display, function(index)
        local chosen = ORDER[index]
        if chosen == current then return end
        current = chosen
        write_state(chosen)
        plugin.show_toast("Home theme saved -- restart the player to apply")
    end)
end)
