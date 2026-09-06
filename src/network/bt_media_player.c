#include "bt_media_player.h"
#include "debug_log.h"
#include "hiby_sys_server.h"
#include "input_device_utils.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* AVRCP transport button support.
 * 1. Hosts a D-Bus org.mpris.MediaPlayer2.Player object via Media1.RegisterPlayer
 *    to answer metadata and status queries from AVRCP controllers.
 * 2. Reads AVRCP button events from BlueZ's virtual evdev keyboard device
 *    ("<name> (AVRCP)") to handle play/pause, next, and previous inputs. */

#define BT_MEDIA_PLAYER_OBJECT_PATH "/org/openhibyplayer/MediaPlayer"
#define BT_MEDIA_PLAYER_ADAPTER_PATH "/org/bluez/hci0"
#define BT_MEDIA_PLAYER_POLL_TIMEOUT_MS 200

static DBusConnection * conn = NULL;
static pthread_t dispatch_thread;
static atomic_bool connect_thread_started = false;
static atomic_bool avrcp_input_thread_started = false;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
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
 * in bluetooth_control.c) and has no remotely-controllable loop-status/rate
 * concept. Accept and ignore rather than erroring, so a conformant MPRIS
 * client doesn't get an unexpected failure for a property it expects to be
 * settable. */
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
         * audio_toggle_pause() (see gui.c's toggle_play_pause()); there's
         * no separate "force play" vs "force pause" entry point, so
         * Play()/Stop() only request a toggle when that would actually
         * move toward the state they're asking for, using the cached
         * playing_state. */
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

/* Fire-and-forget: dbus_message_set_no_reply() tells BlueZ not to send a
 * reply, and dbus_connection_send() queues the call without blocking.
 * Neither RegisterPlayer's nor UnregisterPlayer's reply content is used
 * here beyond logging, and blocking on a reply would stall this thread's
 * own dispatch loop -- including incoming AVRCP button presses -- for up
 * to the reply timeout, which matters since this can fire during a
 * bluetoothd restart. */
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
        /* BlueZ's RegisterPlayer handler reads CanPlay/CanPause/
         * CanControl/CanGoNext/CanGoPrevious directly out of this
         * properties dict argument to populate its internal capability
         * flags -- it never queries the registered object's own D-Bus
         * properties afterward. Leaving this dict empty leaves every flag
         * false, so bluetoothd silently no-ops button presses without
         * ever calling this app. This app always supports all five
         * (matching append_property_value()'s hardcoded TRUE for the same
         * properties). */
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

    dbus_message_set_no_reply(msg, TRUE);
    if (!dbus_connection_send(conn, msg, NULL)) {
        DBG_LOG("bt_media_player: failed to queue %s\n", member);
    }
    dbus_message_unref(msg);
}

/* Emits PropertiesChanged for PlaybackStatus whenever playback state
 * actually changes. A physical play/pause button on a headset tracks its
 * own idea of "currently playing" and sends the opposite action next time
 * -- without this signal that idea never syncs with reality, so the first
 * button press after a state mismatch is a silent no-op (message_handler()
 * already ignores a request that doesn't match actual state) and only the
 * second press takes effect. */
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

    /* Forces the first loop iteration to send an initial PropertiesChanged
     * once registered, regardless of playing_state's default -- a fresh
     * connection shouldn't start from BlueZ's own default assumption if
     * this app already knows the real state. */
    bool last_notified_playing = !playing_state;

    /* Registers once for the app's lifetime to answer standard AVRCP
     * metadata/status queries. */
    call_register_player(true);
    DBG_LOG("bt_media_player: registered\n");

    for (;;) {
        pthread_mutex_lock(&state_mutex);
        bool current_playing = playing_state;
        pthread_mutex_unlock(&state_mutex);

        if (current_playing != last_notified_playing) {
            send_playback_status_changed();
            hiby_sys_server_report_playback_status(current_playing);
            last_notified_playing = current_playing;
        }

        /* Processes any incoming method calls (dispatched to
         * message_handler() above) and blocks for up to this long if
         * there's nothing to do -- also what paces this loop's own
         * playing_state check above, so a play/pause is reflected within
         * one timeout window, not instantly, matching every other
         * ~poll-interval Bluetooth state sync in this codebase (e.g. the
         * GUI's own ~5s connection poll is far coarser than this). */
        dbus_connection_read_write_dispatch(conn, BT_MEDIA_PLAYER_POLL_TIMEOUT_MS);
    }

    return NULL; /* unreached -- this thread runs for the app's whole lifetime, same as audio.c's playback thread */
}

/* Bounded retry for dbus_bus_get() to tolerate slow or in-progress D-Bus startup. */
#define DBUS_BUS_GET_RETRY_ATTEMPTS 10
#define DBUS_BUS_GET_RETRY_DELAY_US 300000

/* One connection attempt: get the bus, register our object path, start the
 * dispatch thread. Shared by the fast bounded retry in bt_media_player_
 * init() below and the slower background retry further down. */
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

/* Background retry delay when initial connection attempts fail. */
#define BT_MEDIA_PLAYER_BACKGROUND_RETRY_DELAY_US (5 * 1000000)

/* Connects to D-Bus and registers the media player, retrying in the background
 * if the bus is not immediately available. */
static void * connect_thread_func(void * arg) {
    (void) arg;
    for (int attempt = 0; attempt < DBUS_BUS_GET_RETRY_ATTEMPTS; attempt++) {
        if (attempt_connect_once()) return NULL;
        if (attempt + 1 < DBUS_BUS_GET_RETRY_ATTEMPTS) usleep(DBUS_BUS_GET_RETRY_DELAY_US);
    }

    DBG_LOG("bt_media_player: exhausted fast retry, falling back to background retry\n");
    for (;;) {
        usleep(BT_MEDIA_PLAYER_BACKGROUND_RETRY_DELAY_US);
        if (attempt_connect_once()) return NULL;
    }
}

/* See avrcp_input_thread_func()'s own comment (top of file) for why this
 * exists and why it's a separate device from hw_buttons.c's own two. */
#define BT_AVRCP_INPUT_NAME_MARKER "(AVRCP)"
#define BT_AVRCP_INPUT_RESCAN_DELAY_US (3 * 1000000)
#define BT_AVRCP_INPUT_POLL_TIMEOUT_MS 1000

static void * avrcp_input_thread_func(void * arg) {
    (void) arg;
    int fd = -1;

    for (;;) {
        if (fd < 0) {
            char path[64];
            if (find_input_device_containing(BT_AVRCP_INPUT_NAME_MARKER, path, sizeof(path))) {
                fd = open(path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    DBG_LOG("bt_media_player: AVRCP input device found at %s\n", path);
                } else {
                    DBG_LOG("bt_media_player: failed to open AVRCP input device %s\n", path);
                }
            }
            if (fd < 0) {
                usleep(BT_AVRCP_INPUT_RESCAN_DELAY_US);
                continue;
            }
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, BT_AVRCP_INPUT_POLL_TIMEOUT_MS);
        if (ret < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (ret == 0) continue; /* timeout, nothing to read -- just re-poll */

        struct input_event ev;
        ssize_t n;
        while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t) sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) { /* press only -- matches hw_buttons.c's own next/prev/play-pause handling, no release-triggered action */
                pthread_mutex_lock(&state_mutex);
                switch (ev.code) {
                    case KEY_PLAYCD:
                    case KEY_PAUSECD:      play_pause_requested = true; break;
                    case KEY_NEXTSONG:     next_requested = true; break;
                    case KEY_PREVIOUSSONG: prev_requested = true; break;
                    default: break;
                }
                pthread_mutex_unlock(&state_mutex);
            }
        }
        /* n == 0 (EOF) or a real error (not EAGAIN, which just means the
         * queue is empty for now) means the accessory disconnected and
         * BlueZ's input plugin destroyed this device -- close and go back
         * to rescanning rather than spinning on a dead fd. */
        if (n == 0 || (n < 0 && errno != EAGAIN)) {
            DBG_LOG("bt_media_player: AVRCP input device gone\n");
            close(fd);
            fd = -1;
        }
    }

    return NULL; /* unreached -- this thread runs for the app's whole lifetime, same as the D-Bus dispatch thread above */
}

void bt_media_player_init(void) {
    if (!atomic_exchange_explicit(&connect_thread_started, true, memory_order_acq_rel)) {
        pthread_t connect_thread;
        if (pthread_create(&connect_thread, NULL, connect_thread_func, NULL) == 0) {
            pthread_detach(connect_thread);
        } else {
            atomic_store_explicit(&connect_thread_started, false, memory_order_release);
        }
    }

    if (!atomic_exchange_explicit(&avrcp_input_thread_started, true, memory_order_acq_rel)) {
        pthread_t avrcp_input_thread;
        if (pthread_create(&avrcp_input_thread, NULL, avrcp_input_thread_func, NULL) == 0) {
            pthread_detach(avrcp_input_thread);
        } else {
            atomic_store_explicit(&avrcp_input_thread_started, false, memory_order_release);
        }
    }
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
