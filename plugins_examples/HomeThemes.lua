plugin.define({ id = "example.home_themes", name = "Home Themes", version = "1.0", api_min = 1 })

-- Reference implementation for plugin.set_home_layout()'s full style
-- surface (PLUGINS.md): 11 ready-made Home looks, picked from a Settings
-- row, spanning both tile and list mode. Each one deliberately combines a
-- DIFFERENT shape/spacing/alignment/accessory recipe, not just a new
-- color on the same skeleton -- e.g. Game Boy and Swamp are both tight
-- zero-radius mono/small rows, but left- vs right-aligned; Wavy, Mountain
-- Sunset, and Zen Terracotta are all big-radius "pill" lists, but at 3
-- different heights/gaps/text sizes -- so trying each one actually shows
-- a different slice of the API, not just a different palette.
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
-- chevron/icon, mono font, flush left. The "as close to a real GB menu
-- as this API gets" reference.
THEMES.gameboy = {
    label = "Game Boy",
    bg = 0x0a0e17,
    text = 0xeaeaea,
    tiles = tiles({
        music = { 0x0f1420, 0xf4d35e }, stream_media = { 0x0f1420, 0xeaeaea },
        wireless = { 0x0f1420, 0x7fd1c9 }, books = { 0x0f1420, 0xf4d35e },
        system = { 0x0f1420, 0xee6c8b }, dac = { 0x0f1420, 0xee6c8b },
    }, { radius = 0, height = 52, align = "left", accessory = false, text_size = "mono", icon = false }),
    options = { mode = "list", row_gap = 0 },
}

-- 2) Terminal -- green-on-black CRT, but roomier than Game Boy and with
-- its icon + chevron kept -- a "real app that happens to use a mono
-- font" feel rather than a boot menu.
THEMES.terminal = {
    label = "Terminal",
    bg = 0x000000,
    text = 0xe6ffe6,
    tiles = tiles({
        music = { 0x000000, 0x33ff33 }, stream_media = { 0x000000, 0x33ff33 },
        wireless = { 0x000000, 0x33ff33 }, books = { 0x000000, 0x33ff33 },
        system = { 0x000000, 0x33ff33 }, dac = { 0x000000, 0x33ff33 },
    }, { radius = 6, height = 68, width = 440, align = "left", accessory = true, text_size = "mono", icon = true }),
    options = { mode = "list", row_gap = 10 },
}

-- 3) Monastic -- austere, calm: undyed-wool/stone tones, generous
-- centered rows, no chevron -- a "quiet room" feel.
THEMES.monastic = {
    label = "Monastic",
    bg = 0xf0ebe1,
    text = 0x4a4438,
    tiles = tiles({
        music = { 0xe4ddd0, 0x4a4438 }, stream_media = { 0xd9cfc0, 0x4a4438 },
        wireless = { 0xe4ddd0, 0x4a4438 }, books = { 0xd9cfc0, 0x4a4438 },
        system = { 0xe4ddd0, 0x4a4438 }, dac = { 0xd9cfc0, 0x4a4438 },
    }, { radius = 6, height = 96, width = 420, align = "center", accessory = false, text_size = "medium", icon = false }),
    options = { mode = "list", row_gap = 14 },
}

-- 4) Wavy -- deep-to-shallow ocean gradient, big pill rows, right-aligned
-- -- reads like ripples receding to the right.
THEMES.wavy = {
    label = "Wavy",
    bg = 0x051826,
    text = 0xdfeff2,
    tiles = tiles({
        music = { 0x0a2540, 0xa8dadc }, stream_media = { 0x0d3050, 0x9fd8dc },
        wireless = { 0x11406a, 0x97d5dc }, books = { 0x155085, 0x8fd2dc },
        system = { 0x1a63a0, 0x87cfdc }, dac = { 0x1f78bf, 0x7fccdc },
    }, { radius = 40, height = 88, width = 430, align = "right", accessory = false, text_size = "medium", icon = false }),
    options = { mode = "list", row_gap = 8 },
}

-- 5) Trees -- lush forest canopy: rounded cards, chevron + icon kept,
-- centered, roomy -- meant to feel like a proper "outdoorsy app", not
-- just a dark-green recolor.
THEMES.trees = {
    label = "Trees",
    bg = 0x0e1a10,
    text = 0xd8c9a3,
    tiles = tiles({
        music = { 0x1e3524, 0xd8c9a3 }, stream_media = { 0x2a4530, 0xd8c9a3 },
        wireless = { 0x1e3524, 0xd8c9a3 }, books = { 0x35281a, 0xead9b8 },
        system = { 0x2a4530, 0xd8c9a3 }, dac = { 0x1e3524, 0xd8c9a3 },
    }, { radius = 24, height = 92, width = 440, align = "center", accessory = true, text_size = "medium", icon = true }),
    options = { mode = "list", row_gap = 10 },
}

-- 6) Swamp -- murky, tight, right-aligned, no chevron/icon -- the
-- claustrophobic opposite of Trees despite the same green family.
THEMES.swamp = {
    label = "Swamp",
    bg = 0x11140a,
    text = 0x9caf6a,
    tiles = tiles({
        music = { 0x232616, 0x8a9c5a }, stream_media = { 0x2c301c, 0x7c8f4e },
        wireless = { 0x232616, 0x8a9c5a }, books = { 0x332c1a, 0x7c8f4e },
        system = { 0x2c301c, 0x8a9c5a }, dac = { 0x232616, 0x7c8f4e },
    }, { radius = 0, height = 48, align = "right", accessory = false, text_size = "small", icon = false }),
    options = { mode = "list", row_gap = 2 },
}

-- 7) Mountain Sunset -- dusk gradient rows, orange fading to indigo as
-- you go down -- big soft pills, centered, large text.
THEMES.mountain_sunset = {
    label = "Mountain Sunset",
    bg = 0x160f2a,
    text = 0xffe3cf,
    tiles = tiles({
        music = { 0xff9d5c, 0x3a1a2e }, stream_media = { 0xf07858, 0x3a1a2e },
        wireless = { 0xc9587c, 0xffe8d6 }, books = { 0x93508f, 0xffe8d6 },
        system = { 0x5f4a9f, 0xffe8d6 }, dac = { 0x33397f, 0xffe8d6 },
    }, { radius = 36, height = 90, width = 420, align = "center", accessory = false, text_size = "large", icon = false }),
    options = { mode = "list", row_gap = 12 },
}

-- 8) Zen Terracotta -- warm earthy pills, generous spacing, centered
-- large text -- a "slow down" kind of menu.
THEMES.zen_terracotta = {
    label = "Zen Terracotta",
    bg = 0xf5ede1,
    text = 0x3a2a1e,
    tiles = tiles({
        music = { 0xd98868, 0x3a2a1e }, stream_media = { 0x9caf88, 0x2a3320 },
        wireless = { 0xe8c39e, 0x4a3728 }, books = { 0xc98a6b, 0x3a2a1e },
        system = { 0xa9bd93, 0x2a3320 }, dac = { 0xdba876, 0x4a3728 },
    }, { radius = 32, height = 100, width = 420, align = "center", accessory = false, text_size = "large", icon = false }),
    options = { mode = "list", row_gap = 16 },
}

-- 9) Retro -- chunky 80s-computer tile grid: cream/amber/brown, small
-- radius (not soft/glossy), tight gap -- reads mechanical, not modern.
THEMES.retro = {
    label = "Retro",
    bg = 0x140f08,
    text = 0xffdca0,
    tiles = tiles({
        music = { 0xf2c14e, 0x2a1a0a }, stream_media = { 0xe8845c, 0x2a1a0a },
        wireless = { 0xd9a441, 0x2a1a0a }, books = { 0xc46a4a, 0xf2e9d8 },
        system = { 0x8a6b4a, 0xf2e9d8 }, dac = { 0xb5804a, 0xf2e9d8 },
    }, { radius = 8 }),
    options = { tile_gap = 6 },
}

-- 10) Vaporwave -- synthwave sunset grid: deep purple fading to hot
-- pink, big soft radius, roomier gap -- soft and glossy, the opposite of
-- Retro's chunky mechanical grid despite both being tile mode.
THEMES.vaporwave = {
    label = "Vaporwave",
    bg = 0x160a2e,
    text = 0xffe0f5,
    tiles = tiles({
        music = { 0x3a1c71, 0xffd1f7 }, stream_media = { 0x6a2c8f, 0xffc2ec },
        wireless = { 0x9b3aa0, 0xffe0f5 }, books = { 0xc23f9e, 0xfff0ff },
        system = { 0xe14f95, 0xfff5fb }, dac = { 0xff6f9c, 0xffffff },
    }, { radius = 26 }),
    options = { tile_gap = 12 },
}

-- 11) Earthy -- rustic clay/moss/bark tile grid: mid radius, minimal
-- gap -- densely packed and grounded, distinct from both Retro's sharp
-- corners and Vaporwave's airy spacing.
THEMES.earthy = {
    label = "Earthy",
    bg = 0x140f0a,
    text = 0xf0e3d0,
    tiles = tiles({
        music = { 0x8a5a3c, 0xf5ead9 }, stream_media = { 0x6b7c4f, 0xf5ead9 },
        wireless = { 0xa87851, 0x3a2a1e }, books = { 0x5c4632, 0xf5ead9 },
        system = { 0x7d8f5c, 0x3a2a1e }, dac = { 0x9c6b46, 0xf5ead9 },
    }, { radius = 16 }),
    options = { tile_gap = 4 },
}

local ORDER = {
    "native", "gameboy", "terminal", "monastic", "wavy", "trees", "swamp",
    "mountain_sunset", "zen_terracotta", "retro", "vaporwave", "earthy",
}
local LABELS = { native = "Native (Default)" }
for name, t in pairs(THEMES) do LABELS[name] = t.label end

-- Also recolors the app-wide screen background AND primary text
-- (plugin.set_background_color()/set_text_color()) to match each theme's
-- palette -- without this, every theme's rows/tiles would float over the
-- same default near-black backdrop regardless of palette (muddy for the
-- light themes, Monastic/Zen Terracotta, and leaves mismatched gaps
-- around every tile-mode grid's tile_gap), AND the "Home" title/chrome
-- text -- style_theme_text_primary, not something set_home_layout() ever
-- touches -- would stay its default light color, unreadable against a
-- light theme's own light background. Both are live/global (unlike
-- set_home_layout()), so this part DOES apply immediately, before restart
-- -- same as plugins_examples/Themes.lua's own light/dark switcher.
local function apply_theme(name)
    local t = THEMES[name]
    if not t then
        pcall(plugin.set_background_color, "screen", 0x000000) -- native defaults
        pcall(plugin.set_text_color, "primary", 0xe6e6e6)
        return
    end
    plugin.set_home_layout(t.tiles, t.options)
    pcall(plugin.set_background_color, "screen", t.bg)
    pcall(plugin.set_text_color, "primary", t.text)
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
