#include "hiby_sys_server.h"
#include "bluetooth_control.h"
#include "debug_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define SYS_SERVER_SOCKET_PATH "/var/run/sys_server"

/* 300ms: generous for a local Unix socket round-trip, short enough that a
 * wedged/absent sys_server can't stall whichever thread calls into this
 * for long -- see this file's own header comment on why failures here are
 * silently tolerated rather than retried. */
#define SYS_SERVER_TIMEOUT_MS 300

static void send_command(const char * cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct timeval tv;
    tv.tv_sec = SYS_SERVER_TIMEOUT_MS / 1000;
    tv.tv_usec = (SYS_SERVER_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SYS_SERVER_SOCKET_PATH);

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }

    if (send(fd, cmd, strlen(cmd), 0) < 0) {
        close(fd);
        return;
    }

    /* Reply ("OK"/"FAIL") observed live but never needed by any caller --
     * still drained so sys_server's own send() doesn't block on a peer
     * that never reads, and so a slow reply is bounded by the same
     * SO_RCVTIMEO above rather than hanging this socket open. */
    char reply[64];
    ssize_t got = recv(fd, reply, sizeof(reply) - 1, 0);
    if (got > 0) {
        reply[got] = '\0';
        DBG_LOG("hiby_sys_server: \"%s\" -> \"%s\"\n", cmd, reply);
    }

    close(fd);
}

void hiby_sys_server_report_playback_status(bool playing) {
    send_command(playing ? "BT:PLAYER_STATUS:playing" : "BT:PLAYER_STATUS:paused");
}

void hiby_sys_server_report_metadata(const char * title, const char * artist,
                                      const char * album, const char * genre, long length_ms) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "BT:METADATA:%s\t%s\t%s\t%s\t%ld",
             title ? title : "", artist ? artist : "", album ? album : "", genre ? genre : "", length_ms);
    send_command(cmd);
}

void hiby_sys_server_report_position(long position_ms) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "BT:POSITION:%ld", position_ms);
    send_command(cmd);
}

void hiby_sys_server_report_volume(int percent) {
    char mac[18];
    if (!bt_control_get_connected_device_mac(mac, sizeof(mac))) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int raw = (percent * 127 + 50) / 100; /* 0-127, same AVRCP absolute-volume scale as BT_SOURCE_VOLUME_MAX in bluetooth_control.c */

    char cmd[48];
    snprintf(cmd, sizeof(cmd), "BT:ABSVOL:%s %d", mac, raw);
    send_command(cmd);
}
