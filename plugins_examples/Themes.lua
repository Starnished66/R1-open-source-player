plugin.define({ id = "example.themes", name = "Themes", version = "3.3", api_min = 7 })

-- Loader/switcher for theme DEFINITION FILES, not a hardcoded theme list --
-- every *.theme file under SD/Themes/ becomes its own selectable entry,
-- discovered fresh on startup and whenever the Theme list opens. This
-- plugin owns only: the fixed icon-asset inventory every theme reskins the
-- same way, the tiny state file remembering which theme is selected, and
-- the "Theme" settings row, plus a built-in "Default" entry that needs no
-- file at all (see DEFAULT_THEME's own comment). A .theme file owns
-- nothing but its own colors/icon-root/optional Home layout -- see
-- plugins_examples/Themes/*.theme for the reference examples this loader
-- ships with.
--
-- Formerly two separate example plugins (this one, app-wide colors/icons
-- only; a second "HomeThemes.lua", 11 hardcoded presets covering plugin.
-- set_home_layout()'s full per-tile style surface too). Consolidated into
-- one here since both called the exact same set_background_color()/
-- set_text_color() pair for the exact same "cohesive app-wide look" goal
-- -- keeping them separate meant two settings rows, two state files, and
-- two copies of that overlapping logic for no real benefit. Every
-- capability of both is preserved: a .theme file can optionally include
-- `tile.<key>.<field>=value` lines (see load_theme_file()'s own comment)
-- for the exact same per-tile bg_color/text_color/radius/height/width/
-- align/accessory/text_size/icon surface HomeThemes.lua demonstrated,
-- plus `home_mode`/`home_tile_gap`/`home_row_gap` for tile-vs-list mode
-- and spacing.
--
-- api_min = 6: plugin.refresh_theme() refreshes decoded images and rebuilds
-- Home only; plugins, screens, playback, and live network services stay up.
--
-- api_min = 7: set_home_layout()'s `options.order` lets a .theme file's
-- `home_order=` line reorder, drop, or (together with another plugin's own
-- plugin.register_home_tile() call) add a Home tile -- see load_theme_
-- file()'s own comment on the `tile.<key>.*` block, now open to any tile
-- key, not just the 6 native ones.

local THEMES_DIR = plugin.sd_root() .. "/Themes"
local STATE_PATH = plugin.sd_root() .. "/.plugins/.theme_state"
local THEME1_ROOT = "/usr/resource/litegui/theme1/"
local THEME2_ROOT = "/usr/resource/litegui/theme2/"

-- Empty string (not "dark"/"white") means Default -- see apply_theme()'s
-- own comment on why Default has no .theme file of its own to point at.
local function read_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return "" end
    local s = f:read("*l") or ""
    f:close()
    return s
end

local function write_state(filename)
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write(filename)
    f:close()
end

-- Every theme2-relative asset path this app actually resolves via
-- asset_path() (src/ui/assets.c) -- collected by grepping src/ui/*.c for
-- every literal asset_path("...") call (including inside ternaries, e.g.
-- topbar/am.png vs pm.png) plus the digit/wifi-signal/codec paths built
-- dynamically at runtime. Deliberately NOT a full mirror of theme1's ~620
-- files (2.7MB): the device's /usr/data partition (where plugin.set_icon()'s
-- overrides land) typically has only a few MB free, and this app never
-- resolves most of theme1/theme2 anyway. Shared by every theme this loader
-- applies -- a .theme file names WHICH root to copy these from (or none at
-- all), never its own asset list. Re-verified against every asset_path()
-- call site in src/ui/*.c (not just literal ".png" strings) after a review
-- found this list missing codec icons, am/pm, usb, the Subsonic download
-- button, quality badges, a second default-cover asset, and the AirPlay
-- overlay's own logo -- all added below -- plus one longstanding wrong
-- extension (boot_animation/en/0.png, corrected to the real .jpg this app
-- actually resolves; the old entry silently copied to a filename asset_
-- path() never requested, a no-op that never restored the real boot splash).
local ASSETS = {
    "boot_animation/en/0.jpg", "bt/bt.png",
    "keyboard/char.png", "keyboard/del.png", "keyboard/dot.png", "keyboard/enter.png",
    "keyboard/left.png", "keyboard/num.png", "keyboard/psk_show.png", "keyboard/psk_hide.png",
    "keyboard/right.png", "keyboard/space2.png", "keyboard/symbol.png", "keyboard/upper.png",
    "launcher/hor_line.png", "launcher/ver_line.png",
    "launcher/music.png", "launcher/music_s.png",
    "launcher/stream_media.png", "launcher/stream_media_s.png",
    "launcher/wireless.png", "launcher/wireless_s.png",
    "launcher/book.png", "launcher/book_s.png",
    "launcher/sys_set.png", "launcher/sys_set_s.png",
    "launcher/dac.png", "launcher/dac_s.png",
    "playing_plane/btn_next.png", "playing_plane/btn_next_s.png",
    "playing_plane/btn_play.png", "playing_plane/btn_pause.png",
    "playing_plane/btn_prev.png", "playing_plane/btn_prev_s.png",
    "playing_plane/buttom.png", "playing_plane/collect_out.png", "playing_plane/collect_in.png",
    "playing_plane/cursor.png", "playing_plane/default_cover_565.png", "playing_plane/ic_more.png",
    "playing_plane/progress_bg.png", "playing_plane/progress.png",
    "playing_plane/loop.png", "playing_plane/single.png", "playing_plane/random.png", "playing_plane/order.png",
    "playing_plane/airplay_logo_white.png",
    "pull_down/bg.png", "pull_down/blk.png", "pull_down/bt.png", "pull_down/bt_s.png",
    "pull_down/sleep_switch.png", "pull_down/sleep_switch_s.png",
    "pull_down/wifi.png", "pull_down/wifi_s.png", "pull_down/fade.png", "pull_down/fade_s.png",
    "sub_back/bg_search.png", "sub_back/btn_back.png", "sub_back/btn_search.png", "sub_back/close.png",
    "topbar/0.png", "topbar/1.png", "topbar/2.png", "topbar/3.png", "topbar/4.png",
    "topbar/5.png", "topbar/6.png", "topbar/7.png", "topbar/8.png", "topbar/9.png",
    "topbar/colon.png", "topbar/a2dp.png",
    "topbar/battery_bg.png", "topbar/battery_charge_bg.png", "topbar/battery_low_bg.png", "topbar/battery.png",
    "topbar/bluetooth.png", "topbar/bluetooth_unconnect.png",
    "topbar/pause.png", "topbar/percent.png", "topbar/play.png", "topbar/po.png", "topbar/speaker.png",
    "topbar/wifi_connect_0.png", "topbar/wifi_connect_1.png", "topbar/wifi_connect_2.png", "topbar/wifi_connect_3.png",
    "topbar/wifi_unconnect.png",
    "topbar/am.png", "topbar/pm.png", "topbar/usb.png",
    "topbar/sbc.png", "topbar/aac.png", "topbar/aptx.png", "topbar/aptx_hd.png", "topbar/ldac.png", "topbar/uat.png",
    "touch_list/a_z_result_bg.png", "touch_list/del.png", "touch_list/item_bg.png",
    "touch_list/list_default_album.png",
    "touch_list/quality_hr.png", "touch_list/quality_high.png", "touch_list/quality_nomal.png",
    "usb/usb.png",
    "volume/bg.png", "volume/cursor.png", "volume/vol_bg.png", "volume/vol.png", "volume/vol_progress.png",
    "category/explorer.png", "category/explorer_s.png",
    "category/artist.png", "category/artist_s.png",
    "category/album.png", "category/album_s.png",
    "category/album_artist.png", "category/album_artist_s.png",
    "category/all.png", "category/all_s.png",
    "category/genre.png", "category/genre_s.png",
    "stream_media/subsonic.png", "stream_media/subsonic_s.png",
    "stream_media/radio.png", "stream_media/radio_s.png",
    "stream_media/download.png",
    "settings/off.png", "settings/on.png",
    "wireless/wifi.png", "wireless/wifi_s.png",
    "wireless/bt.png", "wireless/bt_s.png",
    "wireless/airplay.png", "wireless/airplay_s.png",
    "wireless/dlna.png", "wireless/dlna_s.png",
    "wireless/hibylink.png", "wireless/hibylink_s.png",
    "wireless/via.png", "wireless/via_s.png",
}

-- Default's colors match this app's own built-in defaults (screen_
-- builders.c's screen_builders_init_list_row_style()) exactly, so
-- selecting it restores the original look, not an approximation. It has
-- no .theme file of its own on purpose -- unlike every discovered theme,
-- it must always be selectable even with an empty or missing SD/Themes/
-- folder (a removed card, a fresh install, a typo'd filename), so it can't
-- depend on a file existing at all.
-- home_tiles = {} + home_options = {} is a deliberate, explicit
-- plugin.set_home_layout({}, {}) call (see apply_theme()), not "skip
-- calling it" -- every tile's fields all being unset falls back to
-- native for each one independently, which is what actually resets Home
-- away from a PREVIOUSLY selected theme's custom layout within the same
-- session (home_layout_config has no "unconfigure" -- it only ever holds
-- whatever the last set_home_layout() call this process passed in, so an
-- explicit reset call is the only way back to native once anything else
-- has called it this session).
local DEFAULT_THEME = {
    name = "Default",
    colors = { screen = 0x000000, card = 0x202020, list_row = 0x1C1C1E,
               text_primary = 0xE6E6E6, text_muted = 0xA0A0A0 },
    icon_root = "theme2",
    icon_overrides = {},
    home_tiles = {},
    home_options = {},
}

local function ensure_trailing_slash(path)
    if path:sub(-1) ~= "/" then return path .. "/" end
    return path
end

-- Resolves a .theme file's icon_root field to the actual directory to copy
-- every ASSETS entry from in bulk. Three shapes: "theme1"/"theme2" (the
-- stock packs); an absolute path (a fully custom icon pack, anywhere
-- readable); or any other bare name, treated as a SUBFOLDER of SD/Themes/
-- itself -- e.g. icon_root=MyIconPack for a theme shipping its icons at
-- SD/Themes/MyIconPack/launcher/music.png etc., alongside its own .theme
-- file. Returns nil (only when icon_root itself is nil/omitted) for "don't
-- touch icons at all".
local function resolve_icon_root(icon_root)
    if not icon_root then return nil end
    if icon_root == "theme1" then return THEME1_ROOT end
    if icon_root == "theme2" then return THEME2_ROOT end
    if icon_root:sub(1, 1) == "/" then return ensure_trailing_slash(icon_root) end
    return ensure_trailing_slash(THEMES_DIR .. "/" .. icon_root)
end

-- Same three shapes as resolve_icon_root() above, but for ONE
-- `icon.<relpath>=<value>` override line (see load_theme_file()'s own
-- comment): "theme1"/"theme2" resolve to that stock root PLUS this
-- specific relpath (since, unlike icon_root, there's no whole-ASSETS loop
-- to prefix here); an absolute value is a literal full source path,
-- unrelated to relpath; anything else is a path relative to SD/Themes/ --
-- e.g. icon.launcher/music.png=MyIconPack/music.png for a theme shipping
-- that one custom icon at SD/Themes/MyIconPack/music.png (the value's own
-- path doesn't have to match relpath's shape at all, unlike icon_root's
-- bulk copy, which always does).
local function resolve_icon_override_source(relpath, value)
    if value == "theme1" then return THEME1_ROOT .. relpath end
    if value == "theme2" then return THEME2_ROOT .. relpath end
    if value:sub(1, 1) == "/" then return value end
    return THEMES_DIR .. "/" .. value
end

-- Required numeric keys a .theme file must define -- also doubles as the
-- exact validity check in load_theme_file() below (a def missing even one
-- of these is rejected outright, never partially applied).
local COLOR_KEYS = { "screen", "card", "list_row", "text_primary", "text_muted" }


-- Loads one *.theme file -- plain "key=value" lines, same shape and parser
-- style as settings.c's own on-disk format, deliberately NOT executable
-- Lua: load()/loadstring()/loadfile()/dofile() are all removed by this
-- plugin's own sandbox (sandbox_plugin_lua_state(), plugin_manager.c) --
-- for good reason, since letting a plugin dynamically execute arbitrary
-- code would defeat the entire point of only exposing vetted plugin.*
-- APIs. A theme file only ever needs to describe data (five colors, an
-- optional icon root, an optional per-tile Home layout), never logic, so
-- a tiny line parser is both simpler and the actually-correct amount of
-- power to give it.
--
--   name=White
--   screen=0xE6F6FF
--   card=0xF2F2F2
--   list_row=0xECECEC
--   text_primary=0x1A1A1A
--   text_muted=0x6E6E6E
--   icon_root=theme1                 -- optional; bulk-swaps every ASSETS
--                                     -- entry from this root ("theme1",
--                                     -- "theme2", or a literal path);
--                                     -- omit to leave icons untouched
--   drawer_root=theme1               -- optional; overrides the complete
--                                     -- pull_down/ family after icon_root
--
--   icon.launcher/music.png=theme2   -- optional, any number of these --
--   icon.stream_media/subsonic.png=/data/mnt/sd_0/Themes/MyIcons/subsonic.png
--                                     -- one-off overrides for individual
--                                     -- icons, independent of icon_root
--                                     -- (or layered on top of it -- these
--                                     -- apply AFTER the bulk copy, so they
--                                     -- can refine specific icons a bulk
--                                     -- icon_root swap already set).
--                                     -- Value is "theme1"/"theme2" (that
--                                     -- stock root's copy of this SAME
--                                     -- relpath) or a literal full source
--                                     -- path -- never itself restricted to
--                                     -- ASSETS, so a theme can introduce an
--                                     -- icon this app doesn't otherwise
--                                     -- resolve, same as plugin.set_icon()
--                                     -- always could.
--
--   home_mode=list                   -- optional; "tile" (default) or "list"
--   home_tile_gap=6                  -- optional; tile mode only; 0 is a
--                                     -- real, honored "no gap" value here
--   home_row_gap=10                  -- optional; list mode only -- NOTE:
--                                     -- native (home_layout.h) treats 0 as
--                                     -- "keep the built-in default of 6px",
--                                     -- not literal zero -- there is no way
--                                     -- to request a true 0px row_gap
--                                     -- through this option; use 1 for the
--                                     -- closest achievable "tight" look
--                                     -- (see GameBoy.theme, which wants
--                                     -- exactly this and sets 1, not 0)
--   home_order=dac,music,books       -- optional, comma-separated; which
--                                     -- tiles Home shows and in what
--                                     -- position -- a native key left out
--                                     -- entirely is not shown at all. Names
--                                     -- either a native key (music/stream_
--                                     -- media/wireless/books/system/dac) or
--                                     -- a plugin.register_home_tile() id
--                                     -- another plugin registered. Omit
--                                     -- entirely to keep today's fixed
--                                     -- native order and show all 6.
--
--   tile.music.bg_color=0x1e3524     -- optional, one block per tile key --
--   tile.music.text_color=0xd8c9a3   -- "tile key" is any native key above
--   tile.music.radius=24             -- OR any id a plugin passed to its own
--   tile.music.height=92             -- register_home_tile() call -- every
--   tile.music.width=440             -- field independently optional, same
--   tile.music.align=center          -- "has_X" semantics plugin.set_home_
--   tile.music.accessory=true        -- layout() itself has (PLUGINS.md): an
--   tile.music.text_size=medium      -- omitted field keeps that ONE
--   tile.music.icon=true             -- property at its native default
--                                     -- rather than the whole tile.
--
-- Returns nil (never raises) on any failure -- a missing file, a name or
-- any one of the five colors missing/unparseable, or a MALFORMED (present
-- but not a valid number/"true"/"false") tile.*/home_tile_gap/home_row_gap
-- value -- so a broken .theme file is skipped, not fatal to the whole
-- list, same tolerance plugin_manager.c itself has for a broken plugin
-- file. This matters specifically for numbers/booleans because the
-- alternative is silent, not loud: tonumber() on a bad string returns nil,
-- indistinguishable from the field never being set at all, and a bad
-- string compared against the literal "true" just evaluates to false --
-- both would otherwise reach plugin.set_home_layout() looking like "this
-- field wasn't specified"/"explicitly false" instead of the typo they
-- actually are. align/text_size are the one exception, still passed
-- straight through as raw strings: plugin.set_home_layout() already
-- validates those itself and raises a clear Lua error naming the bad
-- value, which apply_theme()'s pcall() around that whole call turns into
-- "this theme's tiles array is rejected, colors/icons still applied"
-- rather than a silent misapplication -- validating them a second time
-- here would just duplicate that native check.
local function load_theme_file(path)
    local f = io.open(path, "r")
    if not f then return nil end

    local fields = {}
    local tile_fields = {}
    -- Keys actually seen in this file, in first-seen (file) order -- lets a
    -- theme style ANY tile key, not just the 6 native ones: a
    -- tile.<id>.*=value block styles a plugin.register_
    -- home_tile()-provided tile the exact same way tile.<native_key>.*=value
    -- styles a native one. Tracked as a plain array (not pairs() over
    -- tile_fields) because pairs()'s iteration order is unspecified --
    -- relying on it would make a theme's own tile ordering non-reproducible.
    local tile_keys_seen = {}
    local icon_overrides = {}

    for line in f:lines() do
        local tile_key, tile_field, tile_value = line:match("^tile%.(%a[%a_]*)%.(%a[%a_]*)=(.*)$")
        if tile_key then
            if not tile_fields[tile_key] then
                tile_fields[tile_key] = {}
                table.insert(tile_keys_seen, tile_key)
            end
            tile_fields[tile_key][tile_field] = tile_value
        else
            local icon_relpath, icon_value = line:match("^icon%.(.-)=(.*)$")
            if icon_relpath and icon_relpath ~= "" then
                icon_overrides[icon_relpath] = icon_value
            else
                local key, value = line:match("^(%a[%a_]*)=(.*)$")
                if key then fields[key] = value end
            end
        end
    end
    f:close()

    if type(fields.name) ~= "string" or fields.name == "" then return nil end

    local colors = {}
    for _, key in ipairs(COLOR_KEYS) do
        local n = tonumber(fields[key])
        if not n then return nil end
        colors[key] = n
    end

    -- A present-but-malformed numeric/boolean value rejects the WHOLE file,
    -- same as a malformed top-level color above -- NOT silently dropped to
    -- "field omitted" (tonumber() returning nil for a bad string) or
    -- silently coerced to false (a boolean check against the literal
    -- string "true" makes every OTHER string, including a typo like "True"
    -- or "yes", equal false). Both would previously reach plugin.set_home_
    -- layout() looking identical to "this field was never set" or
    -- "explicitly false", never as the error a typo actually is. align/
    -- text_size are still passed straight through unvalidated here -- see
    -- their own note below.
    local function parse_required_number(s)
        if s == nil then return nil, true end -- field simply absent -- not an error
        local n = tonumber(s)
        if not n then return nil, false end -- present but malformed -- reject the file
        return n, true
    end
    local function parse_required_bool(s)
        if s == nil then return nil, true end
        if s == "true" then return true, true end
        if s == "false" then return false, true end
        return nil, false
    end

    local home_tiles = {}
    for _, key in ipairs(tile_keys_seen) do
        local tf = tile_fields[key]
        if next(tf) then
            local row = { key = key }
            for _, numfield in ipairs({ "bg_color", "text_color", "radius", "height", "width" }) do
                local n, ok = parse_required_number(tf[numfield])
                if not ok then return nil end
                if n ~= nil then row[numfield] = n end
            end
            if tf.align then row.align = tf.align end
            if tf.text_size then row.text_size = tf.text_size end
            for _, boolfield in ipairs({ "accessory", "icon" }) do
                local b, ok = parse_required_bool(tf[boolfield])
                if not ok then return nil end
                if b ~= nil then row[boolfield] = b end
            end
            table.insert(home_tiles, row)
        end
    end

    local home_options = {}
    if fields.home_mode then home_options.mode = fields.home_mode end
    do
        local n, ok = parse_required_number(fields.home_tile_gap)
        if not ok then return nil end
        if n ~= nil then home_options.tile_gap = n end
    end
    do
        local n, ok = parse_required_number(fields.home_row_gap)
        if not ok then return nil end
        if n ~= nil then home_options.row_gap = n end
    end
    if fields.home_order and fields.home_order ~= "" then
        local order = {}
        for entry in fields.home_order:gmatch("[^,]+") do table.insert(order, entry) end
        home_options.order = order
    end

    return {
        name = fields.name,
        colors = colors,
        icon_root = (fields.icon_root ~= "" and fields.icon_root) or nil,
        drawer_root = (fields.drawer_root ~= "" and fields.drawer_root) or nil,
        icon_overrides = icon_overrides,
        home_tiles = home_tiles,
        home_options = home_options,
    }
end

-- Scans THEMES_DIR fresh every call --
-- never cached, so dropping/removing a .theme file on the SD card takes
-- effect the next time the Theme list is opened, no reload needed just for
-- that. Sorted by filename (plugin_manager.c's own plugin-file-load order
-- uses the same reasoning: raw directory order is filesystem-dependent,
-- sorting makes the list reproducible instead of arbitrary).
local function scan_themes()
    local entries = plugin.list_dir(THEMES_DIR)
    local themes = {}
    if not entries then return themes end

    local filenames = {}
    for _, e in ipairs(entries) do
        if not e.dir and e.name:match("%.theme$") then
            table.insert(filenames, e.name)
        end
    end
    table.sort(filenames)

    for _, filename in ipairs(filenames) do
        local def = load_theme_file(THEMES_DIR .. "/" .. filename)
        if def then
            table.insert(themes, { filename = filename, def = def })
        end
    end
    return themes
end

-- Real-device bug report (kept from the original single-file version):
-- booting straight into the open player with a theme already applied left
-- the UI in a half-and-half state -- e.g. the screen background switched
-- but list rows stayed the old color, with text unreadable against
-- whichever background it landed on -- and the only fix that stuck was
-- reflashing stock, factory-resetting, and reflashing the open player
-- again. Root cause: colors and icons applied in one order with no
-- isolation, so a single failure partway through left an inconsistent
-- mix. Colors go FIRST here, each individually pcall'd so one failure
-- can't skip the rest, in a fixed "backgrounds before text" order so if
-- something still does interrupt this mid-way, the worst case is unstyled
-- text on a correctly-recolored background (readable, if plain) rather
-- than text and background from two different themes (unreadable). The
-- icon copy loop(s) -- slower, more failure-prone (each one's own file
-- copy), and merely cosmetic if some icons stay stale -- run LAST, and the
-- bulk one is skipped entirely when icon_root resolves to nil. Per-icon
-- overrides (icon.<relpath>=<value> lines) always run, even with no
-- icon_root at all, and specifically AFTER the bulk copy so they can
-- refine individual icons a bulk swap already set rather than being
-- overwritten by it. set_home_layout()
-- runs between colors and icons: an independent, atomic native call
-- (validates the whole tiles/options table before applying anything, all
-- or nothing) that can't leave a half-applied Home layout the way the
-- per-property color/icon calls could, so it doesn't need the same
-- one-field-at-a-time pcall isolation -- one pcall around the whole call
-- is enough, and a rejected tiles/options table (bad align/text_size
-- string, negative radius, ...) only means Home's layout isn't set to
-- what this theme wanted; colors and icons still applied normally either
-- side of it.
local function apply_theme(def)
    local c = def.colors
    pcall(plugin.set_background_color, "screen", c.screen)
    pcall(plugin.set_background_color, "card", c.card)
    pcall(plugin.set_background_color, "list_row", c.list_row)
    pcall(plugin.set_text_color, "primary", c.text_primary)
    pcall(plugin.set_text_color, "muted", c.text_muted)

    pcall(plugin.set_home_layout, def.home_tiles, def.home_options)

    local root = resolve_icon_root(def.icon_root)
    if root then
        for _, rel in ipairs(ASSETS) do
            pcall(plugin.set_icon, rel, root .. rel)
        end
    end

    local drawer_root = resolve_icon_root(def.drawer_root)
    if drawer_root then
        for _, rel in ipairs(ASSETS) do
            if rel:sub(1, 10) == "pull_down/" then pcall(plugin.set_icon, rel, drawer_root .. rel) end
        end
    end

    for relpath, value in pairs(def.icon_overrides) do
        pcall(plugin.set_icon, relpath, resolve_icon_override_source(relpath, value))
    end
end

-- Only for this one startup apply below -- the settings row's own callback
-- (register_list_item below) rescans fresh every time it's opened instead
-- of reusing this, so adding/removing a .theme file on the card shows up
-- the next time the list opens, not just after a reload/restart (a real
-- gap this used to have: this exact table used to be the ONLY scan, reused
-- by the callback via closure, so it only ever reflected whatever was on
-- the card at plugin-load time).
local startup_themes = scan_themes()

-- Empty state, or a persisted filename no longer found on the card (SD
-- swapped, file deleted/renamed since) -- both fall back to Default rather
-- than guessing, same "never silently pick something the user didn't
-- choose" reasoning plugin_manager.c's own sd_build_is_newer()-style
-- comparisons use elsewhere in this codebase.
local selected_filename = read_state()
local selected_def = DEFAULT_THEME
for _, t in ipairs(startup_themes) do
    if t.filename == selected_filename then
        selected_def = t.def
        break
    end
end
apply_theme(selected_def)

plugin.register_list_item("display", "Theme", function()
    -- Fresh scan on every open -- see startup_themes' own comment above for
    -- why this can't just reuse that one.
    local themes = scan_themes()

    local labels = { DEFAULT_THEME.name }
    for _, t in ipairs(themes) do
        table.insert(labels, t.def.name)
    end

    plugin.show_list("Theme", labels, function(index)
        local new_filename = (index == 1) and "" or themes[index - 1].filename
        if new_filename == selected_filename then return end

        local new_def = (index == 1) and DEFAULT_THEME or themes[index - 1].def
        selected_filename = new_filename
        write_state(new_filename)
        apply_theme(new_def)
        plugin.show_toast("Theme applied")
        plugin.refresh_theme()
    end)
end)
