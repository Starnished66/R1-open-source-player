#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* Discrete screen-timeout choices, in seconds -- the slider (gui.c) steps
 * through these rather than offering a raw linear seconds range, since e.g.
 * "247s" isn't a meaningful choice (real-device feedback: should snap to
 * sensible presets: 30s, 1m, 2m, 5m, 10m, 30m). settings_load() snaps any
 * hand-edited or legacy value to the nearest one of these, so this is the
 * single place that defines what a "valid" timeout duration is. */
extern const int SCREEN_TIMEOUT_STEPS[];
#define SCREEN_TIMEOUT_STEP_COUNT 6
#define SCREEN_TIMEOUT_MIN_SECONDS 30
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

typedef struct {
    float volume;              /* 0.0 - 1.0 */
    char last_track[512];      /* absolute path, empty if none */
    double last_position;      /* seconds into last_track */
    bool auto_resume_enabled;  /* if true, launch straight into last_track/last_position */
    uint32_t accent_color;     /* packed 0xRRGGBB, applied to sliders/switches app-wide */
    bool crossfade_enabled;    /* if true, fade into the next queued track near the current one's end */

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

    /* Auto screen-timeout (gui.c's update_timer_cb, backed by
     * backlight_set_screen_on()) -- screen_timeout_enabled=false means never
     * auto-off, screen stays on until the power button is pressed.
     * screen_timeout_seconds is meaningful only when enabled, always one of
     * SCREEN_TIMEOUT_STEPS (see settings_load(), which snaps any
     * hand-edited or legacy value to the nearest step, since the settings
     * file is plaintext and could be hand-edited out of range). */
    bool screen_timeout_enabled;
    int screen_timeout_seconds;

    /* Charge-status LEDs (/sys/class/leds/{red,blue}, see led_control.h) --
     * false forces both off regardless of charge state, for e.g. leaving the
     * device charging overnight in a dark room. */
    bool led_indicator_enabled;

    /* Caps charging at 85% to extend battery longevity -- see
     * charge_limiter.h for how this is actually enforced (there's no
     * dedicated state-of-charge cutoff on this hardware, so it throttles
     * the USB input current limit instead). */
    bool charge_limiter_enabled;

    /* Full poweroff after a long stretch idle (screen off, not playing, not
     * charging) -- see idle_shutdown.h. This is the real standby
     * battery-life lever on this hardware: suspend-to-RAM is available at
     * the kernel level but its display/WiFi resume path is broken
     * (confirmed on real hardware), so a full poweroff -- the same thing
     * the stock firmware's own "Idle shutdown" setting does (confirmed via
     * strings on the stock binary: /sbin/poweroff, power_save_shutdown_timer)
     * -- is the only reliable option. Off by default, matching stock's own
     * default. */
    bool idle_shutdown_enabled;
    int idle_shutdown_minutes;

    /* When true, the idle action above is power_suspend_now() (quick
     * resume) instead of idle_shutdown_now() (full poweroff) -- see
     * power_suspend.h for how real suspend was made to actually work on
     * this hardware. Only meaningful when idle_shutdown_enabled is true.
     * Off by default: a full poweroff is the safer, better-tested default
     * (matches stock's own default too), suspend is the newer opt-in. */
    bool idle_suspend_enabled;

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
} player_settings_t;

/* Loads settings from disk into *out. If the settings file doesn't exist or
 * can't be parsed, *out is populated with sensible defaults (volume 1.0, no
 * last track, auto-resume on) and false is returned. */
bool settings_load(player_settings_t * out);

/* Writes *settings to disk, overwriting any existing file. Writes to a
 * temporary file and renames it into place, so a crash or power loss
 * mid-write can't corrupt the settings file. */
void settings_save(const player_settings_t * settings);

/* Settings > System > Factory Reset: deletes the persisted settings file
 * (and any stray .tmp left behind by an interrupted settings_save()) so the
 * next boot's settings_load() falls through to plain defaults, same as a
 * genuinely fresh install. Scoped to just this file -- the music/book
 * library cache (metadata_db.c), saved PEQ profiles, and playlists are
 * user *data*, not settings, and a "factory settings reset" deleting a
 * user's actual library/profiles/playlists out from under them would be a
 * much more destructive action than what was asked for. Doesn't touch
 * *out or re-apply anything live -- the caller is expected to reboot right
 * after (see gui.c's factory_reset_confirm_cb()), since re-syncing every
 * individual subsystem's live state (accent color, screen timeout, BT/
 * Wi-Fi power, LEDs, ...) by hand is exactly what a fresh boot already does
 * for free, the same reasoning a firmware update reboots rather than
 * trying to hot-swap itself. */
void settings_factory_reset(void);

#endif /* SETTINGS_H */
