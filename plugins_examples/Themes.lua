plugin.define({ id = "example.themes", name = "Themes", version = "1.0", api_min = 1 })

-- Light/Dark theme switcher, adds a "Theme" row to Settings -> Display.
-- White applies a soft blue-white color palette without replacing any of
-- the player's icons. Dark restores this app's own default look (theme2
-- icons, near-black backgrounds).
--
-- The chosen theme is written to a small state file under .plugins/ and
-- reapplied at the very top of this script on every boot -- the same
-- "plugin re-runs its own top-level code on load" mechanism every other
-- plugin already relies on for persistence, so no native settings.c
-- storage is needed.

local STATE_PATH = plugin.sd_root() .. "/.plugins/.theme_state"
local THEME2_ROOT = "/usr/resource/litegui/theme2/"

local function read_state()
    local f = io.open(STATE_PATH, "r")
    if not f then return "dark" end
    local s = f:read("*l")
    f:close()
    if s == "white" then return "white" end
    return "dark"
end

local function write_state(state)
    local f = io.open(STATE_PATH, "w")
    if not f then return end
    f:write(state)
    f:close()
end

-- Every theme2-relative asset path this app actually resolves via
-- asset_path() (src/ui/assets.c) -- collected by grepping src/ui/*.c for
-- every literal ".png" path plus the digit/wifi-signal paths built
-- dynamically with snprintf. Dark copies these from the stock theme2 pack
-- to restore the default icon set. White deliberately never touches them.
local ASSETS = {
    "boot_animation/en/0.png", "bt/bt.png",
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
    "touch_list/a_z_result_bg.png", "touch_list/del.png", "touch_list/item_bg.png",
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
    "settings/off.png", "settings/on.png",
    "wireless/wifi.png", "wireless/wifi_s.png",
    "wireless/bt.png", "wireless/bt_s.png",
    "wireless/airplay.png", "wireless/airplay_s.png",
    "wireless/dlna.png", "wireless/dlna_s.png",
    "wireless/hibylink.png", "wireless/hibylink_s.png",
    "wireless/via.png", "wireless/via_s.png",
}

-- "dark" values match this app's own built-in defaults (screen_builders.c's
-- screen_builders_init_list_row_style()) so reverting to Dark restores the
-- exact original look, not an approximation.
local COLORS = {
    dark  = { screen = 0x000000, card = 0x202020, list_row = 0x1C1C1E,
              text_primary = 0xE6E6E6, text_muted = 0xA0A0A0 },
    white = { screen = 0xE6F6FF, card = 0xF2F2F2, list_row = 0xECECEC,
              text_primary = 0x1A1A1A, text_muted = 0x6E6E6E },
}

-- Real-device bug report: booting straight into the open player with this
-- plugin already set to "white" left the UI in a half-and-half state --
-- e.g. the screen background switched to white but list rows stayed dark,
-- with text unreadable against whichever background it landed on -- and
-- the only fix that stuck was reflashing stock, factory-resetting, and
-- reflashing the open player again. Root cause: this function used to do
-- the ~90-icon copy loop FIRST and the 5 color calls LAST, so any single
-- color call failing partway through (or something interrupting the script
-- between them) left an inconsistent mix of old/new colors -- and neither
-- pcall'd loop iteration nor an uncaught error in one call protected the
-- calls after it. Colors now go FIRST, each individually pcall'd so one
-- failure can't skip the rest, and are applied in a fixed "backgrounds
-- before text" order so if something still does interrupt this mid-way,
-- the worst case is unstyled text on a correctly-recolored background
-- (readable, if plain) rather than text and background from two different
-- themes (unreadable). Dark's icon-restoration loop -- slower, more
-- failure-prone (each one's own file copy), and merely cosmetic if some
-- icons stay stale -- runs afterward. White skips it completely.
local function apply_theme(state)
    local c = COLORS[state]
    pcall(plugin.set_background_color, "screen", c.screen)
    pcall(plugin.set_background_color, "card", c.card)
    pcall(plugin.set_background_color, "list_row", c.list_row)
    pcall(plugin.set_text_color, "primary", c.text_primary)
    pcall(plugin.set_text_color, "muted", c.text_muted)

    if state == "dark" then
        for _, rel in ipairs(ASSETS) do
            pcall(plugin.set_icon, rel, THEME2_ROOT .. rel)
        end
    end
end

local current_state = read_state()
apply_theme(current_state)

plugin.register_list_item("display", "Theme", function()
    plugin.show_list("Theme", { "Dark (Default)", "White" }, function(index)
        local new_state = (index == 2) and "white" or "dark"
        if new_state == current_state then return end
        current_state = new_state
        write_state(new_state)
        apply_theme(new_state)
        if new_state == "dark" then
            plugin.show_toast("Theme saved -- restart the player to fully apply icons")
        else
            plugin.show_toast("White theme applied without changing icons")
        end
    end)
end)
