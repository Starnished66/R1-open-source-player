#include "bt_remote_control.h"
#include "debug_log.h"
#include "remote_control.h"

#include <dbus/dbus.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* This UUID is the Android discovery contract; keep it in sync with the
 * service documentation in bt_remote_control.h. */
#define BT_RC_UUID "9d2f8a7e-6b4c-4d51-8e43-38f7a2c0b719"
#define BT_RC_NAME "Compas Remote Control"
#define BT_RC_OBJECT_PATH "/org/openhibyplayer/RemoteControlProfile"
#define BT_RC_RETRY_MS 3000

/* Implemented alongside the Wi-Fi HTTP transport. This module retains
 * descriptor ownership and closes it after the single-request handler exits. */

static DBusConnection * connection;
static pthread_t service_thread;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool enabled = false;
static bool thread_started;
static bool client_active;
static int active_client_fd = -1;
static char active_device_path[128];
static char bluez_owner[64];
static bool registered;

static void send_empty_reply(DBusConnection * c, DBusMessage * msg) {
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static void send_error(DBusConnection * c, DBusMessage * msg, const char * name,
                       const char * text) {
    DBusMessage * reply = dbus_message_new_error(msg, name, text);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static void * serve_client(void * arg) {
    int fd = (int) (intptr_t) arg;
    remote_control_handle_stream(fd);
    pthread_mutex_lock(&client_mutex);
    close(fd);
    client_active = false;
    active_client_fd = -1;
    active_device_path[0] = '\0';
    pthread_mutex_unlock(&client_mutex);
    return NULL;
}

static DBusHandlerResult profile_message_handler(DBusConnection * c,
                                                   DBusMessage * msg,
                                                   void * user_data) {
    (void) user_data;
    const char * interface = dbus_message_get_interface(msg);
    const char * member = dbus_message_get_member(msg);
    if (!interface || strcmp(interface, "org.bluez.Profile1") != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char * sender = dbus_message_get_sender(msg);
    if (!sender || !bluez_owner[0] || strcmp(sender, bluez_owner) != 0) {
        send_error(c, msg, "org.bluez.Error.Rejected", "Profile requests must come from bluetoothd");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(member, "Release") == 0) {
        registered = false;
        send_empty_reply(c, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "RequestDisconnection") == 0) {
        const char * device = NULL;
        DBusError err;
        dbus_error_init(&err);
        if (dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device,
                                  DBUS_TYPE_INVALID)) {
            pthread_mutex_lock(&client_mutex);
            if (active_client_fd >= 0 && strcmp(active_device_path, device) == 0)
                shutdown(active_client_fd, SHUT_RDWR);
            pthread_mutex_unlock(&client_mutex);
        }
        dbus_error_free(&err);
        send_empty_reply(c, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "NewConnection") != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char * device = NULL;
    int received_fd = -1;
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
        send_error(c, msg, DBUS_ERROR_INVALID_ARGS, "Expected BlueZ device path");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_message_iter_get_basic(&iter, &device);
    if (!dbus_message_iter_next(&iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UNIX_FD) {
        send_error(c, msg, DBUS_ERROR_INVALID_ARGS, "Expected RFCOMM file descriptor");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_message_iter_get_basic(&iter, &received_fd);
    if (!dbus_message_iter_next(&iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        send_error(c, msg, DBUS_ERROR_INVALID_ARGS, "Expected connection options dictionary");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* The descriptor belongs to the incoming D-Bus message. Duplicate it
     * before replying so the worker can outlive message dispatch. */
    int client_fd = fcntl(received_fd, F_DUPFD_CLOEXEC, 0);
    if (client_fd < 0) {
        send_error(c, msg, "org.bluez.Error.Rejected", "Could not retain RFCOMM connection");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    struct timeval io_timeout = { .tv_sec = 5, .tv_usec = 0 };
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) != 0 ||
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) != 0) {
        close(client_fd);
        send_error(c, msg, "org.bluez.Error.Rejected", "Could not configure RFCOMM timeouts");
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    pthread_mutex_lock(&client_mutex);
    if (client_active || !atomic_load(&enabled)) {
        pthread_mutex_unlock(&client_mutex);
        close(client_fd);
        send_error(c, msg, "org.bluez.Error.Rejected", "Remote control is busy or disabled");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    client_active = true;
    active_client_fd = client_fd;
    strncpy(active_device_path, device, sizeof(active_device_path) - 1);
    active_device_path[sizeof(active_device_path) - 1] = '\0';
    pthread_mutex_unlock(&client_mutex);

    pthread_t worker;
    int rc = pthread_create(&worker, NULL, serve_client, (void *) (intptr_t) client_fd);
    if (rc != 0) {
        pthread_mutex_lock(&client_mutex);
        client_active = false;
        active_client_fd = -1;
        active_device_path[0] = '\0';
        pthread_mutex_unlock(&client_mutex);
        close(client_fd);
        send_error(c, msg, "org.bluez.Error.Rejected", "Could not start remote-control session");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    pthread_detach(worker);
    send_empty_reply(c, msg);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable profile_vtable = {
    NULL, profile_message_handler, NULL, NULL, NULL, NULL
};

/* Polling alone can miss a very short bluetoothd restart. This signal makes
 * a lost/reclaimed org.bluez name invalidate registration immediately. */
static DBusHandlerResult name_owner_changed(DBusConnection * c, DBusMessage * msg,
                                             void * user_data) {
    (void) c;
    (void) user_data;
    if (!dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char * name = NULL;
    const char * old_owner = NULL;
    const char * new_owner = NULL;
    DBusError err;
    dbus_error_init(&err);
    bool valid = dbus_message_get_args(msg, &err,
        DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
        DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID);
    dbus_error_free(&err);
    if (valid && strcmp(name, "org.bluez") == 0 && strcmp(old_owner, new_owner) != 0)
        registered = false;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool append_option_string(DBusMessageIter * dict, const char * key,
                                 const char * value) {
    DBusMessageIter entry, variant;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value) ||
        !dbus_message_iter_close_container(&entry, &variant)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

static bool append_option_bool(DBusMessageIter * dict, const char * key,
                               dbus_bool_t value) {
    DBusMessageIter entry, variant;
    if (!dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value) ||
        !dbus_message_iter_close_container(&entry, &variant)) return false;
    return dbus_message_iter_close_container(dict, &entry);
}

/* Android discovers the RFCOMM channel from the SDP record by UUID. Require
 * authentication so BlueZ completes pairing before invoking NewConnection. */
static bool register_profile(bool register_it) {
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
        "org.bluez.ProfileManager1", register_it ? "RegisterProfile" : "UnregisterProfile");
    if (!msg) return false;

    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    const char * path = BT_RC_OBJECT_PATH;
    const char * uuid = BT_RC_UUID;
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path) ||
        !dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &uuid)) {
        dbus_message_unref(msg);
        return false;
    }
    if (register_it) {
        DBusMessageIter dict;
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
        bool ok = append_option_string(&dict, "Name", BT_RC_NAME) &&
                  append_option_string(&dict, "Role", "server") &&
                  append_option_bool(&dict, "AutoConnect", FALSE) &&
                  append_option_bool(&dict, "RequireAuthentication", TRUE);
        if (!ok || !dbus_message_iter_close_container(&iter, &dict)) {
            dbus_message_unref(msg);
            return false;
        }
    }

    DBusError err;
    dbus_error_init(&err);
    DBusMessage * reply = dbus_connection_send_with_reply_and_block(connection, msg, 3000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        DBG_LOG("bt_remote_control: %s failed: %s\n", register_it ? "RegisterProfile" : "UnregisterProfile",
                err.message ? err.message : "no D-Bus reply");
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(reply);
    dbus_error_free(&err);
    registered = register_it;
    DBG_LOG("bt_remote_control: profile %s\n", register_it ? "registered" : "unregistered");
    return true;
}

/* The older libdbus shipped on the target lacks dbus_bus_get_name_owner().
 * Query the bus daemon directly and retain only its small unique-name string
 * so even a fast bluetoothd restart is detected if both polls see an owner. */
static bool get_bluez_owner(char * owner, size_t owner_size) {
    DBusMessage * msg = dbus_message_new_method_call("org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
    if (!msg) return false;
    const char * name = "org.bluez";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage * reply = dbus_connection_send_with_reply_and_block(connection, msg, 1000, &err);
    dbus_message_unref(msg);
    const char * value = NULL;
    bool ok = reply && dbus_message_get_args(reply, &err,
        DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
    if (ok && value && owner_size > 0) {
        snprintf(owner, owner_size, "%s", value);
    } else if (owner_size > 0) {
        owner[0] = '\0';
    }
    if (reply) dbus_message_unref(reply);
    dbus_error_free(&err);
    return ok;
}

static void * service_thread_func(void * unused) {
    (void) unused;
    bool bluez_present = false;
    unsigned retry = 0;
    while (atomic_load(&enabled)) {
        if (!connection) {
            DBusError err;
            dbus_error_init(&err);
            connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
            if (!connection) {
                DBG_LOG("bt_remote_control: system bus unavailable: %s\n", err.message ? err.message : "unknown error");
                dbus_error_free(&err);
                usleep(BT_RC_RETRY_MS * 1000);
                continue;
            }
            dbus_error_free(&err);
            dbus_connection_set_exit_on_disconnect(connection, FALSE);
            DBusError match_err;
            dbus_error_init(&match_err);
            dbus_bus_add_match(connection,
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
                &match_err);
            if (dbus_error_is_set(&match_err)) {
                DBG_LOG("bt_remote_control: could not watch BlueZ restarts: %s\n",
                        match_err.message ? match_err.message : "unknown error");
                dbus_error_free(&match_err);
            } else {
                dbus_error_free(&match_err);
                dbus_connection_add_filter(connection, name_owner_changed, NULL, NULL);
            }
            if (!dbus_connection_register_object_path(connection, BT_RC_OBJECT_PATH, &profile_vtable, NULL)) {
                DBG_LOG("bt_remote_control: failed to export Profile1 object\n");
                dbus_connection_close(connection);
                dbus_connection_unref(connection);
                connection = NULL;
                usleep(BT_RC_RETRY_MS * 1000);
                continue;
            }
            bluez_present = false;
        }

        if (!dbus_connection_get_is_connected(connection)) {
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            connection = NULL;
            registered = false;
            bluez_present = false;
            bluez_owner[0] = '\0';
            continue;
        }

        char current_owner[64] = { 0 };
        bool now_present = get_bluez_owner(current_owner, sizeof(current_owner));
        if (now_present && bluez_owner[0] && strcmp(bluez_owner, current_owner) != 0)
            registered = false;
        snprintf(bluez_owner, sizeof(bluez_owner), "%s", current_owner);
        if (now_present && (!bluez_present || !registered) && retry == 0) {
            if (!register_profile(true)) retry = 2;
        } else if (!now_present) {
            registered = false;
            retry = 0;
        }
        bluez_present = now_present;
        if (retry > 0) retry--;
        dbus_connection_read_write_dispatch(connection, 500);
    }

    if (connection) {
        if (registered && dbus_connection_get_is_connected(connection)) register_profile(false);
        dbus_connection_unregister_object_path(connection, BT_RC_OBJECT_PATH);
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
        connection = NULL;
    }
    bluez_owner[0] = '\0';
    registered = false;
    return NULL;
}

void bt_remote_control_start(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    if (thread_started) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }
    atomic_store(&enabled, true);
    int rc = pthread_create(&service_thread, NULL, service_thread_func, NULL);
    if (rc == 0) {
        thread_started = true;
    } else {
        atomic_store(&enabled, false);
        DBG_LOG("bt_remote_control: could not start service thread (%d)\n", rc);
    }
    pthread_mutex_unlock(&lifecycle_mutex);
}

void bt_remote_control_stop(void) {
    pthread_t thread;
    pthread_mutex_lock(&lifecycle_mutex);
    if (!thread_started) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }
    atomic_store(&enabled, false);
    pthread_mutex_lock(&client_mutex);
    if (active_client_fd >= 0) shutdown(active_client_fd, SHUT_RDWR);
    pthread_mutex_unlock(&client_mutex);
    thread = service_thread;
    thread_started = false;
    pthread_mutex_unlock(&lifecycle_mutex);
    pthread_join(thread, NULL);
}
