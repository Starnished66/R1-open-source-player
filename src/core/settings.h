#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* Discrete screen-timeout presets. The original useful coarse progression
 * is preserved, with a new 15-second choice added at the beginning. */
extern const int SCREEN_TIMEOUT_STEPS[];
#define SCREEN_TIMEOUT_STEP_COUNT 7
#define SCREEN_TIMEOUT_MIN_SECONDS 15
#define SCREEN_TIMEOUT_MAX_SECONDS 1800

/* Idle-shutdown choices, in minutes -- same discrete-steps reasoning as
 * SCREEN_TIMEOUT_STEPS above. This is a full poweroff (see idle_shutdown.h),
 * not a sleep, so the steps skew longer than the screen timeout. */
extern const int IDLE_SHUTDOWN_STEPS[];
#define IDLE_SHUTDOWN_STEP_COUNT 5

/* Sleep-timer choices, in minutes -- same discrete-steps reasoning as
 * SCREEN_TIMEOUT_STEPS above. This only persists which duration the quick-
 * drawer sleep icon (gui.c) arms next time it's tapped -- whether the timer
 * is currently counting down is live, session-only state, not saved here. */
extern const int SLEEP_TIMER_STEPS[];
#define SLEEP_TIMER_STEP_COUNT 5
#define SETTINGS_SUBSONIC_SAVED_MAX 16

typedef struct {
    float volume;              /* 0.0 - 1.0 */
    char last_track[512];      /* absolute path, empty if none */
    double last_position;      /* seconds into last_track */
    int last_source_kind;      /* 0 = unknown/filesystem, 1 = All Songs, 2 = album */
    char last_source_name[128];/* album title when last_source_kind == 2 */

    /* Settings -> Playback -> Resume Last Track. 0 = off (default), 1 =
     * resume and start playing immediately on a cold boot, 2 = resume
     * (load last_track, seek to last_position, show the player screen) but
     * stay paused -- no audible auto-play, just ready the moment the user
     * hits play. Replaces the old plain auto_resume_enabled bool (defaulted
     * true, defaulting a boot-time auto-play feature to on) -- see gui_init()
     * for the actual resume path, gated on this and reusing the exact same
     * Subsonic-cache guard Car Mode's own separate, always-on resume
     * mechanism already established after a real crash-reboot-loop
     * incident (2026-08-08) traced to resuming into a cached stream file.
     * Independent of car_mode_enabled below -- Car Mode has its own
     * dedicated docking-routine resume (always plays, requires a headphone
     * connected at boot), this is the general-purpose one a normal user
     * opts into by hand. */
    int resume_mode;

    /* Settings -> Playback -> Play/Pause Button. What the physical
     * play/pause button does: 0 = Play/Pause (default, matches the
     * button's own label), 1 = Previous Track, 2 = Play/Pause on a single
     * press, but Previous Track if a second press follows within
     * PLAY_PAUSE_DOUBLE_CLICK_MS (gui.c) -- lets one physical button cover
     * both without a dedicated previous-track button. */
    int play_pause_button_mode;

    uint32_t accent_color;     /* packed 0xRRGGBB, applied to sliders/switches app-wide */
    bool crossfade_enabled;    /* if true, fade into the next queued track near the current one's end */

    /* Settings -> Playback -> ReplayGain. 0 = Off (no gain applied), 1 = Per
     * Track (default -- normalizes every track to the same perceived
     * loudness), 2 = Per Album (preserves intentional relative loudness
     * differences between tracks on the same album, falling back to track
     * gain for any track whose file has no album-level tag -- see gui.c's
     * own resolve_replaygain()). Replaced the old plain on/off replaygain_
     * enabled bool -- see settings.c's load path for the migration from
     * that field's old on-disk key. */
    int replaygain_mode;

    /* Car Mode: matches the stock firmware's own real behavior (confirmed
     * by real-device report) -- unplugging power while something is loaded
     * checkpoints position and powers the device off; plugging power back
     * in powers it back on and resumes automatically (forces the same
     * startup path auto_resume_enabled triggers, independent of that
     * setting). See gui.c's update_timer_cb and gui_init() for the two
     * halves. Off by default: this is an opt-in convenience for a specific
     * docking routine, not a default playback behavior -- and unlike a
     * plain auto-resume toggle, leaving it on outside a car would power the
     * device off every time its charger gets unplugged. */
    bool car_mode_enabled;

    /* Subsonic-compatible (Subsonic/Navidrome/Airsonic/...) server config.
     * Stored in plaintext like every other setting here -- this project has
     * no secure-storage mechanism (keychain, encrypted-at-rest file, etc.)
     * on either host or target, so pretending otherwise would be dishonest;
     * a personal device's own settings file is the same trust boundary as
     * the passwords already sitting in most people's browser profiles. */
    char subsonic_url[256];      /* e.g. "https://music.example.com:4040", no trailing slash */
    char subsonic_username[128];
    char subsonic_password[128];
    bool subsonic_verify_tls;    /* false = accept self-signed certs for this server (opt-in, see http_client.h) */

    /* Saved Subsonic server profiles (the "Saved Servers" list). Tagcache
     * cannot store these; they live here with the rest of credentials so
     * they survive an unmounted SD card. url is the unique key -- saving
     * the same URL again replaces credentials rather than duplicating. */
    struct {
        char url[256];
        char username[128];
        char password[128];
        bool verify_tls;
    } subsonic_saved[SETTINGS_SUBSONIC_SAVED_MAX];
    int subsonic_saved_count;

    /* Bluetooth output settings -- see bluetooth_control.h for what each
     * actually does at the bluealsa/alsa.conf level. Defaults match the
     * stock firmware's own bt_init script (`bluealsa -p a2dp-source
     * --a2dp-volume`, no a2dp-sink, no forced codec). */
    bool bt_volume_sync_enabled; /* --a2dp-volume: HW volume buttons also change the paired device's volume */
    bool bt_dac_mode_enabled;    /* a2dp-sink profile: lets another device stream audio TO this one */
    char bt_codec[16];           /* "auto"/"ldac_hq"/"ldac_sq"/"aptx"/"aac"/"sbc" -- written into alsa.conf */
    /* Real-device feedback: nearby BLE beacons/accessories that don't
     * broadcast a name clutter the Bluetooth screen's "Available Devices"
     * list, showing as raw MAC addresses (see add_bt_device_row()'s own
     * MAC-address fallback in gui.c). Defaults to hiding them -- paired
     * devices are shown regardless of this setting, since a device you've
     * already paired with is never something you'd want hidden. */
    bool bt_hide_unnamed_devices;

    /* AirPlay receive mode -- see airplay_control.h. Mutually exclusive with
     * bt_dac_mode_enabled (both would fight over the same physical audio
     * hardware), enforced in gui.c wherever either gets toggled on. */
    bool wifi_dac_mode_enabled;

    /* DLNA/UPnP-AV MediaRenderer receive mode -- see dlna_control.h. Not
     * mutually exclusive with bt_dac_mode_enabled/wifi_dac_mode_enabled
     * the way those two are with each other: unlike them, this doesn't
     * pipe raw external audio straight to the output hardware, it just
     * downloads a cast track and plays it through the normal local
     * decoder/playback pipeline, same as any other track. */
    bool dlna_renderer_enabled;

    /* Phone remote-control (Now Playing web page) -- see remote_control.h.
     * Same lifecycle shape as dlna_renderer_enabled: a background listener
     * thread started/stopped on toggle and re-applied once at startup if
     * already on. Default false -- this exposes now-playing state (title/
     * artist/album/position, no auth) to anything on the same Wi-Fi
     * network, so it stays opt-in rather than silently on for every user. */
    bool remote_control_enabled;

    /* Auto screen-timeout (gui.c's update_timer_cb, backed by
     * backlight_set_screen_on()) -- screen_timeout_enabled=false means never
     * auto-off, screen stays on until the power button is pressed.
     * screen_timeout_seconds is meaningful only when enabled, always one of
     * SCREEN_TIMEOUT_STEPS above (see settings_load(), which snaps any
     * hand-edited or legacy value to the nearest step, since the settings
     * file is plaintext and could be hand-edited out of range). */
    bool screen_timeout_enabled;
    int screen_timeout_seconds;

    /* Pre-timeout low-brightness stage. When enabled, gui.c dims an idle
     * screen to approximately 5% before the full screen timeout. */
    bool screen_dimming_enabled;

    /* Hide the global status bar (and, on the Player specifically, the
     * standalone back arrow) while the Player or its fullscreen Lyrics
     * view is active. Other screens retain their status information. */
    bool hide_player_topbar;

    /* Charge-status LEDs (/sys/class/leds/{red,blue}, see led_control.h) --
     * false forces both off regardless of charge state, for e.g. leaving the
     * device charging overnight in a dark room. */
    bool led_indicator_enabled;

    /* Caps charging at 85% to extend battery longevity -- see
     * charge_limiter.h for how this is actually enforced (there's no
     * dedicated state-of-charge cutoff on this hardware, so it throttles
     * the USB input current limit instead). */
    bool charge_limiter_enabled;

    /* Caps the AXP2101 constant-current charging phase at 500mA. Turning
     * this off deliberately leaves the PMIC's current setting unchanged. */
    bool safe_charging_enabled;

    /* Topbar battery percentage digits next to the battery icon -- the icon
     * itself (charge outline + fill gauge) is always shown regardless of
     * this, only the "NN%" readout is optional. When off, gui.c's
     * sync_topbar_status_icon_positions() lets the Wi-Fi/Bluetooth status
     * icons slide into the space the percentage would otherwise occupy,
     * right next to the battery icon, instead of leaving it empty. On by
     * default, matching the always-on behavior every previous version of
     * this app had. */
    bool show_battery_percent;

    /* Idle action after a long stretch idle (screen off, not playing, not
     * charging) -- either power_suspend_now() (suspend-to-RAM, see
     * idle_suspend_enabled below) or a full poweroff (idle_shutdown.h; the
     * same thing the stock firmware's own "Idle shutdown" setting does,
     * confirmed via strings on the stock binary: /sbin/poweroff,
     * power_save_shutdown_timer). On by default (10 minutes): real-world
     * feedback found users who never open Settings at all reporting
     * overnight battery drain, since a device left screen-off otherwise
     * just sits at full power indefinitely with nothing opted in. Existing
     * installs' saved settings are untouched by this default -- only a
     * fresh settings.txt (settings_load() finding none, see set_defaults())
     * picks it up. */
    bool idle_shutdown_enabled;
    int idle_shutdown_minutes;

    /* When true, the idle action above is power_suspend_now() (quick
     * resume) instead of idle_shutdown_now() (full poweroff) -- see
     * power_suspend.h for how real suspend was made to actually work on
     * this hardware. Only meaningful when idle_shutdown_enabled is true. On
     * by default alongside it: a full poweroff would cost the in-progress
     * queue/position and mean a real boot on every wake, exactly the "why
     * did my music reset" complaint a battery-drain fix shouldn't trade
     * for. */
    bool idle_suspend_enabled;

    /* Internal bookkeeping, not exposed in any UI: whether this install has
     * already gone through settings_load()'s one-time forced migration of
     * the three fields above to their new (on) defaults. Lets an existing
     * settings.txt saved by a version predating that default flip -- which
     * would otherwise keep pinning idle_shutdown_enabled/idle_suspend_enabled/
     * idle_shutdown_minutes back to their old off/off/30 values forever,
     * since settings_save() always writes every field -- get force-migrated
     * exactly once, the same way a brand new install already gets the new
     * defaults from set_defaults(). A user who deliberately turns any of
     * the three back off afterward is not touched again. */
    bool idle_suspend_default_migrated;

    /* USB gadget mode (Storage/USB DAC/ADB) -- see usb_mode_control.h for
     * usb_mode_t and how each is actually applied. Plain int here (values
     * matching usb_mode_t), not the enum itself, so this header doesn't
     * need to depend on usb_mode_control.h -- same reasoning as bt_codec
     * above being a plain string rather than pulling in an enum from
     * bluetooth_control.h. Purely a UI/persistence value: this only
     * remembers the last mode the user picked (to pre-select the right
     * radio button next time Settings is opened), it does NOT get
     * automatically re-applied to the real USB gadget hardware on startup
     * -- that hardware state doesn't survive a reboot regardless (ADB's
     * own init script is deliberately not auto-started either), and
     * silently re-enabling ADB/DAC mode on every boot without the user
     * asking felt like the wrong default. */
    int usb_mode;

    /* Player queue play mode -- Sequential/Repeat All/Repeat One/Shuffle.
     * Plain int (values matching gui.c's own play_mode_t), same reasoning as
     * usb_mode above: this header doesn't need to depend on gui.c's own
     * playback-orchestration types for a value that's purely persisted user
     * preference. */
    int play_mode;

    /* Swipe up (anywhere on a screen) jumps straight back to Home, the same
     * destination as repeatedly swipe-right-to-go-back all the way out --
     * see screen_gesture_event_cb() in gui.c. A new, less-standard gesture
     * alongside the existing swipe-right (back one level) and swipe-left
     * (jump to Now Playing), opt-out rather than opt-in since it's additive
     * (doesn't remove or reassign either existing gesture) and doesn't
     * fire on a false positive the way a diagonal swipe or an accidental
     * two-finger touch could. */
    bool swipe_up_home_enabled;

    /* Matches a stock-firmware setting: launch at a fixed volume every time
     * rather than wherever the slider was last left. Default matches
     * stock's own default (fixed, 20%) -- disable to fall back to this
     * app's original behavior of resuming at last-used volume. Only
     * startup_volume_fixed_percent is meaningful when
     * startup_volume_fixed_enabled is true; the plain `volume` field above
     * still tracks and persists the live slider position either way, so
     * turning this off later resumes from wherever it was last left, not
     * from stale pre-feature state. */
    bool startup_volume_fixed_enabled;
    int startup_volume_fixed_percent;

    /* Duration the quick-drawer sleep icon arms next time it's tapped --
     * see gui.c's quick_drawer_sleep_event_cb()/poll_sleep_timer(). Always
     * one of SLEEP_TIMER_STEPS (settings_load() snaps any hand-edited or
     * legacy value to the nearest step, same reasoning as
     * screen_timeout_seconds). Whether the timer is currently counting down
     * is live, session-only state, not saved here. */
    int sleep_timer_minutes;

    /* IANA time zone id (e.g. "America/New_York"), applied via
     * timezone_apply() (timezone_apply.h) both at startup and whenever
     * changed from the Time Zone picker (gui.c). Empty string means "not
     * set" -- this app has never applied anything, so the system falls
     * back to whatever (if anything) /usr/data/localtime already points
     * at, which is nothing by default on this hardware (confirmed live:
     * /etc/localtime is a symlink to a target that doesn't exist out of
     * the box), so time displays in UTC until a zone is chosen. */
    char timezone[64];

    /* Settings -> System -> Hostname. Empty means "use the stock device
     * name" (/usr/resource/hostname, baked into the read-only squashfs at
     * build time -- this app has no write access there). When non-empty,
     * applied at every boot by apply_custom_hostname() (main.c), which
     * bind-mounts a writable copy over BOTH /usr/resource/hostname and
     * /usr/resource/bt_name (the same file already drives the DHCP
     * hostname wifi_on.sh advertises and the Bluetooth alias bt_init sets
     * -- see apply_custom_hostname()'s own comment) and calls sethostname()
     * for the kernel's own copy. Requires a reboot to take effect since
     * both wifi_on.sh and bt_init only ever read their file once, at the
     * point something turns Wi-Fi/Bluetooth on -- matches the explicit
     * real-device request ("reboot and have it applied") rather than
     * trying to force a live re-apply into two scripts this app doesn't
     * own. RFC 1123 caps a hostname label at 63 characters; 64 leaves room
     * for the NUL. */
    char hostname[64];

    /* UI text size (Settings -> Font Size): 0 = Small (the original,
     * unchanged default sizing), 1 = Medium, 2 = "BlindMF" (largest --
     * literal feature name, requested verbatim). Applied once at startup,
     * before any screen is built (see gui.c's apply_font_size_tier(),
     * called at the very top of gui_init()) -- every text label in this
     * app sets its font via a plain per-object LVGL style
     * (lv_obj_set_style_text_font()), not a shared style object LVGL could
     * bulk-refresh, so there's no cheap way to re-style ~90 call sites'
     * worth of already-built widgets live. Changing this setting takes
     * effect on the next launch, same as usb_mode above. */
    int font_size_tier;

    /* Settings -> Lyrics Text Size: a SEPARATE size control just for the
     * fullscreen synchronized lyrics view, independent of font_size_tier
     * above. Real-device request: lyrics need to stay large/readable
     * regardless of whatever the user picked for the rest of the app's UI
     * chrome, since scaling them down with a smaller general text-size
     * choice made them hard to read. Only 2 of the 3 usual tiers apply here
     * (1 = Medium, 2 = Large, matching the same tier numbering/pixel sizes
     * font_size_tier's own Medium/BlindMF already use for their 28px slot --
     * see fallback_font.h's app_font_lyrics) -- there's no "Small" option,
     * since a small lyrics view was the exact complaint that prompted this
     * separate control to exist. Defaults to 2 (Large) rather than 1, for
     * the same readability-first reasoning. Same "next launch, not live"
     * timing as font_size_tier -- see that field's own comment for why. */
    int lyrics_font_size_tier;

    /* Screen brightness, logical 0-100 (same scale as backlight.h's
     * backlight_get_percent()/backlight_set_percent()). Real-device bug
     * report: brightness had no memory across a real power-down/power-up --
     * nothing applied any brightness at startup, so the screen came up at
     * whatever raw value the kernel/bootloader itself defaults the backlight
     * to, not whatever the user had it set to before powering off. Applied
     * once in gui_init(), before any screen is built (same timing as
     * apply_font_size_tier()), and re-persisted on every slider release
     * (quick_drawer_brightness_changed_cb() in gui.c) the same way volume
     * is. Default 80 matches backlight.c's own restore_percent fallback
     * default, for consistency. */
    int brightness_percent;

    /* Settings -> System -> "24-Hour Clock". Controls the topbar clock's
     * format (refresh_clock_label() in gui.c): true renders "%H:%M" (the
     * app's original, only-ever behavior, so this is the default -- an
     * existing install's clock does not change format on its own), false
     * renders "%I:%M" plus an AM/PM sprite next to it (topbar/am.png,
     * topbar/pm.png -- already-present theme assets, previously unused by
     * any code in this app). */
    bool clock_24h;
} player_settings_t;

/* Loads settings from disk into *out. If the settings file doesn't exist or
 * can't be parsed, *out is populated with sensible defaults (volume 1.0, no
 * last track, auto-resume on) and false is returned. */
bool settings_load(player_settings_t * out);

/* Writes *settings to disk, overwriting any existing file. Writes to a
 * temporary file and renames it into place, so a crash or power loss
 * mid-write can't corrupt the settings file. */
void settings_save(const player_settings_t * settings);

/* Upserts a Saved Servers profile by URL. Does not write disk -- call
 * settings_save() after, same as every other settings mutation. */
void settings_subsonic_server_upsert(player_settings_t * settings, const char * url, const char * username,
                                      const char * password, bool verify_tls);

/* Settings > System > Factory Reset: wipes every direct child of /usr/data
 * EXCEPT "mnt" (real bug report: an earlier version of this only deleted
 * the settings file, leaving the active PEQ, hostname override, Bluetooth
 * pairings, ALSA config, theme overrides, etc. all still in effect after a
 * "reset"), then reboots so the next boot's settings_load() falls through
 * to plain defaults, same as a genuinely fresh install.
 *
 * "mnt" is not an ordinary subdirectory -- it's the live SD card mount
 * (/usr/data/mnt/sd_0, confirmed on-device: /data is a symlink to usr/data,
 * and /dev/mmcblk0p1 is mounted there). Every one of this app's own
 * MUSIC_ROOT_DIR/PLAYLISTS_DIR/METADATA_DB_PATH references elsewhere point
 * at that same path -- the music/book library cache, saved PEQ profiles,
 * and playlists all already live there, so skipping "mnt" both protects the
 * user's actual SD card from ever being touched and, as a direct
 * consequence, already preserves all of that real user *data* without
 * needing its own separate carve-out. See settings.c's own implementation
 * comment for exactly how "mnt" is skipped (never even descended into, not
 * merely excluded by a flag/pattern).
 *
 * Doesn't touch *out or re-apply anything live -- the caller is expected to
 * reboot right after (see gui.c's factory_reset_confirm_cb()), since re-
 * syncing every individual subsystem's live state (accent color, screen
 * timeout, BT/Wi-Fi power, LEDs, ...) by hand is exactly what a fresh boot
 * already does for free, the same reasoning a firmware update reboots
 * rather than trying to hot-swap itself. */
void settings_factory_reset(void);

#endif /* SETTINGS_H */
