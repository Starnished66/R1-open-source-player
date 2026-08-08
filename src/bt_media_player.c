#include "bt_media_player.h"
#include "debug_log.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Real-device bug report: connecting Bluetooth headphones played audio
 * fine (once BT output itself worked, see audio_set_bt_output()'s history
 * in audio.h) and volume synced (see bt_control_source_volume_sync_start()
 * in bluetooth_control.c), but the headphones' own play/pause/next/
 * previous buttons had no effect on this app at all.
 *
 * Root cause: AVRCP transport passthrough commands (a headphone pressing
 * play/pause/skip) arrive at bluetoothd, which delivers them by calling
 * Play()/Pause()/Next()/Previous() as D-Bus METHODS on an object *this
 * app* must register and host -- not something reachable via the
 * one-shot bluetoothctl/dbus-send/bluealsa-cli invocations everything
 * else in bluetooth_control.c uses (those can only send calls out, not
 * receive incoming ones). Confirmed by reading BlueZ's own
 * doc/org.bluez.Media.rst: Media1.RegisterPlayer's registered object
 * "must implement at least org.mpris.MediaPlayer2.Player as defined in
 * [the freedesktop] MPRIS 2.2 spec" -- the standard "media player on
 * D-Bus" interface (also used by desktop apps like VLC/Spotify), not a
 * BlueZ-specific one.
 *
 * This needed a real D-Bus *service* (responding to incoming calls, not
 * just sending them), which needed a real D-Bus client library --
 * vendored from source (libdbus) rather than reusing the device's own
 * glibc-built libdbus-1.so.3, since this whole project targets
 * mipsel-linux-musl and musl/glibc aren't ABI-compatible (dlopen() from a
 * static musl binary doesn't work on this device either, per the
 * Makefile's own mbedTLS comment) -- see the Makefile's DBUS_DIR section
 * for the full story.
 *
 * Everything below runs on ONE dedicated thread that owns the
 * DBusConnection exclusively: it both dispatches BlueZ's incoming method
 * calls AND makes this app's own outgoing calls (RegisterPlayer/
 * UnregisterPlayer), so there's no need for libdbus's own thread-safety
 * locking mode (dbus_threads_init_default()) -- avoids a whole class of
 * "which thread touches the connection when" bugs by construction, the
 * same reasoning audio.c's playback thread being the sole owner of
 * alsa_pcm/bt_aplay_fd already follows. */

#define BT_MEDIA_PLAYER_OBJECT_PATH "/org/openhibyplayer/MediaPlayer"
#define BT_MEDIA_PLAYER_ADAPTER_PATH "/org/bluez/hci0"
#define BT_MEDIA_PLAYER_POLL_TIMEOUT_MS 200

static DBusConnection * conn = NULL;
static pthread_t dispatch_thread;
static bool init_done = false;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool requested_active = false; /* what gui.c wants (bt_media_player_set_active) */
static bool actual_active = false;    /* what the dispatch thread has actually told BlueZ (RegisterPlayer done) -- only that thread writes this */
static bool playing_state = false;    /* mirrors audio_is_playing(), for PlaybackStatus -- set via bt_media_player_notify_playback_state() */
static bool play_pause_requested = false;
static bool next_requested = false;
static bool prev_requested = false;

static void append_variant_bool(DBusMessageIter * iter, dbus_bool_t value) {
    DBusMessageIter variant;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(iter, &variant);
}

static void append_variant_string(DBusMessageIter * iter, const char * value) {
    DBusMessageIter variant;
    dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(iter, &variant);
}

/* Returns NULL (not an error) if name isn't one of ours -- callers fall
 * through to "no such property" in that case. */
static const char * playback_status_string(void) {
    pthread_mutex_lock(&state_mutex);
    bool p = playing_state;
    pthread_mutex_unlock(&state_mutex);
    return p ? "Playing" : "Paused";
}

/* Appends this property's value as a variant into iter (already
 * positioned correctly by the caller -- either directly for Get, or as a
 * dict-entry value for GetAll). Returns false if `property` isn't
 * recognized for `interface`. */
static bool append_property_value(DBusMessageIter * iter, const char * interface, const char * property) {
    if (strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) {
        if (strcmp(property, "PlaybackStatus") == 0) { append_variant_string(iter, playback_status_string()); return true; }
        if (strcmp(property, "CanControl") == 0) { append_variant_bool(iter, TRUE); return true; }
        if (strcmp(property, "CanPlay") == 0) { append_variant_bool(iter, TRUE); return true; }
        if (strcmp(property, "CanPause") == 0) { append_variant_bool(iter, TRUE); return true; }
        if (strcmp(property, "CanGoNext") == 0) { append_variant_bool(iter, TRUE); return true; }
        if (strcmp(property, "CanGoPrevious") == 0) { append_variant_bool(iter, TRUE); return true; }
        return false;
    }
    if (strcmp(interface, "org.mpris.MediaPlayer2") == 0) {
        if (strcmp(property, "Identity") == 0) { append_variant_string(iter, "open_hiby_player"); return true; }
        if (strcmp(property, "CanQuit") == 0) { append_variant_bool(iter, FALSE); return true; }
        if (strcmp(property, "CanRaise") == 0) { append_variant_bool(iter, FALSE); return true; }
        if (strcmp(property, "HasTrackList") == 0) { append_variant_bool(iter, FALSE); return true; }
        return false;
    }
    return false;
}

/* Every property name this app exposes for `interface`, in the same order
 * append_property_value() recognizes them -- used by GetAll. NULL-terminated. */
static const char * const player_properties[] = {
    "PlaybackStatus", "CanControl", "CanPlay", "CanPause", "CanGoNext", "CanGoPrevious", NULL
};
static const char * const root_properties[] = {
    "Identity", "CanQuit", "CanRaise", "HasTrackList", NULL
};

static const char * const * properties_for_interface(const char * interface) {
    if (strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) return player_properties;
    if (strcmp(interface, "org.mpris.MediaPlayer2") == 0) return root_properties;
    return NULL;
}

static void send_empty_reply(DBusConnection * c, DBusMessage * msg) {
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static void send_error_reply(DBusConnection * c, DBusMessage * msg, const char * error_name, const char * error_msg) {
    DBusMessage * reply = dbus_message_new_error(msg, error_name, error_msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static DBusHandlerResult handle_properties_get(DBusConnection * c, DBusMessage * msg) {
    const char * interface = "";
    const char * property = "";
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        send_error_reply(c, msg, DBUS_ERROR_INVALID_ARGS, "Expected (interface, property)");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    if (!append_property_value(&iter, interface, property)) {
        dbus_message_unref(reply);
        send_error_reply(c, msg, "org.freedesktop.DBus.Error.UnknownProperty", property);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_properties_get_all(DBusConnection * c, DBusMessage * msg) {
    const char * interface = "";
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID)) {
        dbus_error_free(&err);
        send_error_reply(c, msg, DBUS_ERROR_INVALID_ARGS, "Expected (interface)");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    const char * const * names = properties_for_interface(interface);

    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessageIter iter, dict_iter;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
    for (int i = 0; names && names[i]; i++) {
        DBusMessageIter entry_iter;
        dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
        dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &names[i]);
        append_property_value(&entry_iter, interface, names[i]);
        dbus_message_iter_close_container(&dict_iter, &entry_iter);
    }
    dbus_message_iter_close_container(&iter, &dict_iter);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* MPRIS's Volume/LoopStatus/Rate are writable in the spec, but this app
 * doesn't use MPRIS for volume (see bt_control_source_volume_sync_start()
 * in bluetooth_control.c -- that goes through bluealsa's own AVRCP
 * absolute-volume property instead, the mechanism actually reachable from
 * a2dp-source audio) and has no loop-status/rate concept remotely
 * controllable yet. Accept and ignore rather than erroring, so a
 * conformant MPRIS client that tries doesn't get an unexpected failure
 * for a property it reasonably expects to be settable. */
static DBusHandlerResult handle_properties_set(DBusConnection * c, DBusMessage * msg) {
    send_empty_reply(c, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult message_handler(DBusConnection * c, DBusMessage * msg, void * user_data) {
    (void) user_data;
    const char * interface = dbus_message_get_interface(msg);
    const char * member = dbus_message_get_member(msg);
    if (!interface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(interface, "org.freedesktop.DBus.Properties") == 0) {
        if (strcmp(member, "Get") == 0) return handle_properties_get(c, msg);
        if (strcmp(member, "GetAll") == 0) return handle_properties_get_all(c, msg);
        if (strcmp(member, "Set") == 0) return handle_properties_set(c, msg);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (strcmp(interface, "org.freedesktop.DBus.Introspectable") == 0 && strcmp(member, "Introspect") == 0) {
        static const char xml[] =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            "  <interface name=\"org.mpris.MediaPlayer2\">\n"
            "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
            "  </interface>\n"
            "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
            "    <method name=\"Play\"/>\n"
            "    <method name=\"Pause\"/>\n"
            "    <method name=\"PlayPause\"/>\n"
            "    <method name=\"Stop\"/>\n"
            "    <method name=\"Next\"/>\n"
            "    <method name=\"Previous\"/>\n"
            "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
            "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
            "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
            "  </interface>\n"
            "</node>\n";
        DBusMessage * reply = dbus_message_new_method_return(msg);
        if (reply) {
            const char * p = xml;
            dbus_message_append_args(reply, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
            dbus_connection_send(c, reply, NULL);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) {
        /* Play/Pause/PlayPause/Stop all reduce to this app's single
         * audio_toggle_pause() (see gui.c's toggle_play_pause(), which
         * bt_media_player_consume_play_pause() below ultimately triggers)
         * -- there's no separate "force play" vs "force pause" entry
         * point, so Play()/Stop() only request a toggle when that would
         * actually move toward the state they're asking for, using the
         * cached playing_state bt_media_player_notify_playback_state()
         * keeps current. */
        pthread_mutex_lock(&state_mutex);
        bool currently_playing = playing_state;
        if (strcmp(member, "PlayPause") == 0) {
            play_pause_requested = true;
        } else if (strcmp(member, "Play") == 0) {
            if (!currently_playing) play_pause_requested = true;
        } else if (strcmp(member, "Pause") == 0 || strcmp(member, "Stop") == 0) {
            if (currently_playing) play_pause_requested = true;
        } else if (strcmp(member, "Next") == 0) {
            next_requested = true;
        } else if (strcmp(member, "Previous") == 0) {
            prev_requested = true;
        } else {
            pthread_mutex_unlock(&state_mutex);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        pthread_mutex_unlock(&state_mutex);
        send_empty_reply(c, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult vtable_message_handler(DBusConnection * c, DBusMessage * msg, void * user_data) {
    return message_handler(c, msg, user_data);
}

static DBusObjectPathVTable vtable = {
    NULL, /* unregister_function */
    vtable_message_handler,
    NULL, NULL, NULL, NULL
};

/* Blocking (dbus_connection_send_with_reply_and_block) -- only ever
 * called from this file's own dispatch thread (see do_set_active() below),
 * never from the caller of bt_media_player_set_active(), so this doesn't
 * risk blocking the GUI thread the way a direct call would. */
static void call_register_player(bool register_it) {
    const char * member = register_it ? "RegisterPlayer" : "UnregisterPlayer";
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", BT_MEDIA_PLAYER_ADAPTER_PATH,
                                                       "org.bluez.Media1", member);
    if (!msg) return;

    const char * path = BT_MEDIA_PLAYER_OBJECT_PATH;
    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
    if (register_it) {
        /* Real-device root cause: BlueZ's RegisterPlayer handler
         * (profiles/audio/media.c's register_player()/
         * parse_player_properties(), confirmed by reading BlueZ's own
         * source) reads CanPlay/CanPause/CanControl/CanGoNext/
         * CanGoPrevious DIRECTLY out of THIS properties dict argument to
         * populate its internal capability flags -- it never queries the
         * registered object's own D-Bus properties afterward, contrary to
         * what the MPRIS spec's property-based design might suggest.
         * Leaving this dict empty (the original version of this function)
         * left every flag false, so bluetoothd's local_player_play()/
         * _pause()/_next() etc. silently no-op without ever calling this
         * app at all -- confirmed live: bluetoothd's own debug log showed
         * "AV/C: PLAY pressed" and its play()/next()/m_pause() firing on
         * every button press, but this app's message handler never once
         * received a call. This app always supports all five (see
         * append_property_value()'s own hardcoded TRUE for these same
         * properties when BlueZ or anything else queries them normally
         * via Properties.Get/GetAll). */
        DBusMessageIter dict_iter;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
        static const char * const cap_names[] = {
            "CanPlay", "CanPause", "CanControl", "CanGoNext", "CanGoPrevious"
        };
        for (size_t i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
            DBusMessageIter entry_iter;
            dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
            dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &cap_names[i]);
            append_variant_bool(&entry_iter, TRUE);
            dbus_message_iter_close_container(&dict_iter, &entry_iter);
        }
        dbus_message_iter_close_container(&iter, &dict_iter);
    }

    DBusError err;
    dbus_error_init(&err);
    DBusMessage * reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    if (!reply) {
        DBG_LOG("bt_media_player: %s failed: %s\n", member, err.message ? err.message : "(no message)");
        dbus_error_free(&err);
    } else {
        dbus_message_unref(reply);
    }
    dbus_message_unref(msg);
}

/* Real-device bug report: "I have to press twice the play/pause action to
 * get it to work the first time, then it's just once". Root cause: this
 * app only ever ANSWERED PlaybackStatus queries (Properties.Get/GetAll);
 * it never proactively told BlueZ/the accessory when it changed. A
 * physical play/pause button on a real headset tracks its OWN idea of
 * "currently playing" (confirmed live in bluetoothd's own debug log: a
 * single physical button sends distinct AV/C PLAY vs PAUSE op-codes, not
 * one unconditional toggle) so it can send the opposite action next time
 * -- and with no PropertiesChanged ever emitted, that idea starts from
 * whatever the accessory defaults to (observed behavior consistent with
 * "assume paused") regardless of whether this app was already mid-track
 * when the connection was made. The mismatched first press is silently a
 * no-op on this app's side (message_handler()'s Play()/Pause() handlers
 * already correctly ignore a request that doesn't match actual state --
 * see their own comment), which is exactly "the first press did nothing";
 * the second press then lines up. Emitting this signal whenever playback
 * state actually changes keeps the accessory's own assumption correct
 * going forward, closing the gap a fresh connection would otherwise hit
 * once. */
static void send_playback_status_changed(void) {
    DBusMessage * signal = dbus_message_new_signal(BT_MEDIA_PLAYER_OBJECT_PATH,
                                                     "org.freedesktop.DBus.Properties", "PropertiesChanged");
    if (!signal) return;

    DBusMessageIter iter, dict_iter, entry_iter, invalid_iter;
    dbus_message_iter_init_append(signal, &iter);
    const char * interface_name = "org.mpris.MediaPlayer2.Player";
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface_name);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
    dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
    const char * prop_name = "PlaybackStatus";
    dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &prop_name);
    append_variant_string(&entry_iter, playback_status_string());
    dbus_message_iter_close_container(&dict_iter, &entry_iter);
    dbus_message_iter_close_container(&iter, &dict_iter);

    /* Third PropertiesChanged argument: invalidated property names (none) -- still required by the signature. */
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &invalid_iter);
    dbus_message_iter_close_container(&iter, &invalid_iter);

    dbus_connection_send(conn, signal, NULL);
    dbus_message_unref(signal);
}

static void * dispatch_thread_func(void * arg) {
    (void) arg;

    /* Forces the first loop iteration to always send an initial
     * PropertiesChanged once actually registered, regardless of which way
     * playing_state happens to default -- a fresh connection should never
     * start from BlueZ's own unknown/default assumption if this app
     * already knows the real state. */
    bool last_notified_playing = !playing_state;

    for (;;) {
        pthread_mutex_lock(&state_mutex);
        bool want_active = requested_active;
        bool current_playing = playing_state;
        pthread_mutex_unlock(&state_mutex);

        if (want_active != actual_active) {
            call_register_player(want_active);
            actual_active = want_active; /* only this thread writes actual_active -- no lock needed for the write itself */
            if (actual_active) last_notified_playing = !current_playing; /* force a fresh notify right after (re)registering */
        }

        if (actual_active && current_playing != last_notified_playing) {
            send_playback_status_changed();
            last_notified_playing = current_playing;
        }

        /* Processes any incoming method calls (dispatched to
         * message_handler() above) and blocks for up to this long if
         * there's nothing to do -- also what paces this loop's own
         * requested_active/playing_state checks above, so a connect/
         * disconnect or a play/pause is reflected within one timeout
         * window, not instantly, matching every other ~poll-interval
         * Bluetooth state sync in this codebase (e.g. the GUI's own ~5s
         * connection poll is far coarser than this). */
        dbus_connection_read_write_dispatch(conn, BT_MEDIA_PLAYER_POLL_TIMEOUT_MS);
    }

    return NULL; /* unreached -- this thread runs for the app's whole lifetime, same as audio.c's playback thread */
}

/* Bounded retry for dbus_bus_get() below -- real-device bug report: after a
 * freeze-triggered reboot, AVRCP transport controls stopped working for the
 * rest of that boot, even though a normal cold boot never had the problem.
 * Root cause: this is called very early in main() (right after audio_init(),
 * well before gui_init()), racing S30dbus's own dbus-daemon startup -- a
 * single failed dbus_bus_get() here used to give up for the whole session
 * (init_done reset to false, but bt_media_player_init() is only ever called
 * once from main.c, so nothing actually retried). A reboot immediately
 * following a freeze/crash is exactly the boot least likely to have normal,
 * predictable init.d timing (filesystem checks, whatever state the crash
 * left things in), so this needs to tolerate the bus not being up yet rather
 * than assume a clean boot's timing. 10 attempts / 300ms apart = 3s worst
 * case -- generous enough for a slow boot, still bounded so a genuinely
 * absent bus (not just slow) doesn't hang startup indefinitely. */
#define DBUS_BUS_GET_RETRY_ATTEMPTS 10
#define DBUS_BUS_GET_RETRY_DELAY_US 300000

/* One connection attempt: get the bus, register our object path, start the
 * dispatch thread. Shared by the fast bounded retry in bt_media_player_
 * init() below and the slower background retry further down -- pulled out
 * so both loops attempt the exact same thing rather than drifting apart. */
static bool attempt_connect_once(void) {
    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        DBG_LOG("bt_media_player: dbus_bus_get failed: %s\n", err.message ? err.message : "(no message)");
        dbus_error_free(&err);
        return false;
    }
    dbus_error_free(&err);

    if (!dbus_connection_register_object_path(conn, BT_MEDIA_PLAYER_OBJECT_PATH, &vtable, NULL)) {
        DBG_LOG("bt_media_player: failed to register object path\n");
        return false;
    }
    DBG_LOG("bt_media_player: connected to system bus and registered %s\n", BT_MEDIA_PLAYER_OBJECT_PATH);

    pthread_create(&dispatch_thread, NULL, dispatch_thread_func, NULL);
    pthread_detach(dispatch_thread);
    return true;
}

/* Real-device bug report (a second, worse instance of the incident
 * documented above DBUS_BUS_GET_RETRY_ATTEMPTS): AVRCP transport controls
 * still didn't work on a fresh boot, even with that bounded 3s retry in
 * place. Root cause: this device's own S30dbus + S80_bt_init boot scripts
 * always leave TWO independent dbus-daemon processes running (see
 * bluetooth_control.h's own extensive history on this exact split-brain
 * condition, and why this app deliberately does NOT try to fix it at boot
 * time -- an earlier attempt reliably hung the whole app before it ever
 * reached the main loop, confirmed across three separate rounds of trying
 * to fix the attempt itself, not just the timing). Confirmed live: after
 * manually consolidating the two daemons down to one and forcing a
 * reconnect, dbus_bus_get() STILL failed every one of the 10 fast
 * attempts, apparently because the freshly-restarted daemon+bluetoothd
 * pair genuinely needed more than the ~3s this loop allows for. A slow or
 * contended boot -- exactly what the split-brain condition, or a slow
 * chip bring-up (bt_control_init_chip() itself already documents ~10-13s
 * for that alone), makes plausible -- can outlast 3s even on a completely
 * normal, non-crash-triggered boot, not just the freeze-reboot scenario
 * the original fix targeted.
 *
 * Falls back to this slower, effectively-unbounded background retry
 * instead of giving up outright once the fast loop below is exhausted --
 * deliberately on its OWN thread, unlike the fast loop (which main.c
 * already accepts blocking startup for, up to 3s): retrying every 5s
 * indefinitely from the main thread would either reintroduce that same
 * kind of startup stall repeatedly, or (worse, if done from update_timer_
 * cb's periodic polling) freeze the UI for a moment on every single retry
 * for as long as the bus stays down. A background thread waking up briefly
 * every 5s is negligible overhead even in the worst case (a bus that never
 * comes up at all), so there's no real cost to just retrying forever
 * rather than picking some arbitrary give-up point that would only
 * reproduce this exact bug again for anyone whose boot is slower than
 * that. */
#define BT_MEDIA_PLAYER_BACKGROUND_RETRY_DELAY_US (5 * 1000000)

static void * background_retry_thread_func(void * arg) {
    (void) arg;
    for (;;) {
        usleep(BT_MEDIA_PLAYER_BACKGROUND_RETRY_DELAY_US);
        if (attempt_connect_once()) {
            init_done = true;
            return NULL;
        }
    }
}

void bt_media_player_init(void) {
    if (init_done) return;
    init_done = true;

    for (int attempt = 0; attempt < DBUS_BUS_GET_RETRY_ATTEMPTS; attempt++) {
        if (attempt_connect_once()) return;
        if (attempt + 1 < DBUS_BUS_GET_RETRY_ATTEMPTS) usleep(DBUS_BUS_GET_RETRY_DELAY_US);
    }

    DBG_LOG("bt_media_player: exhausted fast retry, falling back to background retry\n");
    init_done = false;
    pthread_t retry_thread;
    pthread_create(&retry_thread, NULL, background_retry_thread_func, NULL);
    pthread_detach(retry_thread);
}

void bt_media_player_set_active(bool active) {
    pthread_mutex_lock(&state_mutex);
    requested_active = active;
    pthread_mutex_unlock(&state_mutex);
}

void bt_media_player_notify_playback_state(bool playing) {
    pthread_mutex_lock(&state_mutex);
    playing_state = playing;
    pthread_mutex_unlock(&state_mutex);
}

bool bt_media_player_consume_play_pause(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = play_pause_requested;
    play_pause_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool bt_media_player_consume_next(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = next_requested;
    next_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool bt_media_player_consume_prev(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = prev_requested;
    prev_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}
