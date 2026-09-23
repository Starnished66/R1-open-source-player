#include "remote_control_mdns.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT 5353
#define RC_PORT 8899
#define SERVICE_NAME "_compas-remote._tcp.local"
#define INSTANCE_NAME "Compas Remote Control._compas-remote._tcp.local"

static atomic_bool active;
static pthread_t worker;
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint16_t put16(uint8_t *p, uint16_t v) {
    v = htons(v); memcpy(p, &v, 2); return 2;
}

static uint16_t put32(uint8_t *p, uint32_t v) {
    v = htonl(v); memcpy(p, &v, 4); return 4;
}

static size_t put_name(uint8_t *out, size_t capacity, const char *name) {
    size_t n = 0;
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (!len || len > 63 || n + len + 2 > 255 || n + len + 2 > capacity) return 0;
        out[n++] = (uint8_t)len;
        memcpy(out + n, label, len); n += len;
        if (!dot) break;
        label = dot + 1;
    }
    out[n++] = 0;
    return n;
}

static bool decode_name(const uint8_t *packet, size_t packet_len, size_t *offset,
                        char *out, size_t out_size) {
    size_t pos = *offset, written = 0, resume = 0;
    bool jumped = false;
    unsigned labels = 0;
    for (;;) {
        if (pos >= packet_len || ++labels > 128) return false;
        uint8_t len = packet[pos++];
        if ((len & 0xc0) == 0xc0) {
            if (pos >= packet_len) return false;
            size_t pointer = ((size_t)(len & 0x3f) << 8) | packet[pos++];
            if (pointer >= packet_len) return false;
            if (!jumped) resume = pos;
            jumped = true; pos = pointer;
            continue;
        }
        if (len & 0xc0 || pos + len > packet_len) return false;
        if (!len) break;
        if (written && written + 1 >= out_size) return false;
        if (written) out[written++] = '.';
        if (written + len >= out_size) return false;
        for (unsigned i = 0; i < len; i++) {
            unsigned char c = packet[pos++];
            out[written++] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
        }
    }
    out[written] = '\0';
    *offset = jumped ? resume : pos;
    return true;
}

static bool append_record(uint8_t *packet, size_t *offset, const char *owner,
                          uint16_t type, uint32_t ttl, bool unique,
                          const uint8_t *data, size_t len) {
    size_t n = *offset;
    if (n + 255 + 10 + len > 1400) return false;
    size_t name_len = put_name(packet + n, 1400 - n, owner);
    if (!name_len || n + name_len + 10 + len > 1400) return false;
    n += name_len;
    n += put16(packet + n, type);
    n += put16(packet + n, (uint16_t)(1 | (unique ? 0x8000 : 0))); /* IN; cache flush on unique records */
    n += put32(packet + n, ttl);
    n += put16(packet + n, (uint16_t)len);
    memcpy(packet + n, data, len); n += len;
    *offset = n;
    return true;
}

static size_t make_answer(uint8_t *packet, const char *host, struct in_addr ip, uint32_t ttl,
                          const char *qname, uint16_t qtype, unsigned *answers, unsigned *additional) {
    memset(packet, 0, 1400);
    put16(packet, 0); put16(packet + 2, 0x8400); /* response, authoritative */
    *answers = *additional = 0;
    size_t n = 12;
    uint8_t data[300];
    bool announce = !qname;
    bool service = announce || strcasecmp(qname, SERVICE_NAME) == 0;
    bool instance = announce || strcasecmp(qname, INSTANCE_NAME) == 0;
    bool host_query = announce || strcasecmp(qname, host) == 0;
    bool ptr = service && (announce || qtype == 12 || qtype == 255);
    bool srv = instance && (announce || qtype == 33 || qtype == 255);
    bool txt = instance && (announce || qtype == 16 || qtype == 255);
    bool address = host_query && (announce || qtype == 1 || qtype == 255);
    size_t len = put_name(data, sizeof(data), INSTANCE_NAME);
    if (ptr && append_record(packet, &n, SERVICE_NAME, 12, ttl, false, data, len)) (*answers)++;
    len = put16(data, 0); len += put16(data + len, 0); len += put16(data + len, RC_PORT);
    len += put_name(data + len, sizeof(data) - len, host);
    if (srv && append_record(packet, &n, INSTANCE_NAME, 33, ttl, true, data, len)) (*answers)++;
    data[0] = 5; memcpy(data + 1, "api=1", 5);
    if (txt && append_record(packet, &n, INSTANCE_NAME, 16, ttl, true, data, 6)) (*answers)++;
    memcpy(data, &ip.s_addr, 4);
    if (address && append_record(packet, &n, host, 1, ttl, true, data, 4)) (*answers)++;
    /* Browse answers carry the SRV/TXT/A records as additional data. */
    if (!announce && ptr) {
        len = put16(data, 0); len += put16(data + len, 0); len += put16(data + len, RC_PORT);
        len += put_name(data + len, sizeof(data) - len, host);
        if (append_record(packet, &n, INSTANCE_NAME, 33, ttl, true, data, len)) (*additional)++;
        data[0] = 5; memcpy(data + 1, "api=1", 5);
        if (append_record(packet, &n, INSTANCE_NAME, 16, ttl, true, data, 6)) (*additional)++;
        memcpy(data, &ip.s_addr, 4);
        if (append_record(packet, &n, host, 1, ttl, true, data, 4)) (*additional)++;
    } else if (!announce && srv) {
        memcpy(data, &ip.s_addr, 4);
        if (append_record(packet, &n, host, 1, ttl, true, data, 4)) (*additional)++;
    }
    put16(packet + 6, (uint16_t)*answers);
    put16(packet + 10, (uint16_t)*additional);
    return n;
}

static bool wifi_address(struct in_addr *out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    snprintf(req.ifr_name, sizeof(req.ifr_name), "wlan0");
    bool ok = ioctl(fd, SIOCGIFFLAGS, &req) == 0 &&
              (req.ifr_flags & IFF_UP) && (req.ifr_flags & IFF_RUNNING);
    if (ok) {
        memset(&req, 0, sizeof(req));
        snprintf(req.ifr_name, sizeof(req.ifr_name), "wlan0");
        ok = ioctl(fd, SIOCGIFADDR, &req) == 0;
    }
    if (ok) {
        struct sockaddr_in *addr = (struct sockaddr_in *)&req.ifr_addr;
        *out = addr->sin_addr;
        ok = out->s_addr != htonl(INADDR_ANY) && (ntohl(out->s_addr) >> 24) != 169;
    }
    close(fd);
    return ok;
}

static void send_records(int fd, const struct sockaddr_in *to, const char *host,
                         struct in_addr ip, uint32_t ttl, const char *qname, uint16_t qtype) {
    uint8_t packet[1400];
    unsigned answers, additional;
    size_t len = make_answer(packet, host, ip, ttl, qname, qtype, &answers, &additional);
    if (len > 12 && answers) (void)sendto(fd, packet, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static bool parse_question(const uint8_t *packet, size_t len, char *name, size_t name_size,
                           uint16_t *type, bool *unicast) {
    if (len < 12 || (packet[2] & 0x80) || (packet[4] == 0 && packet[5] == 0)) return false;
    size_t offset = 12;
    if (!decode_name(packet, len, &offset, name, name_size) || offset + 4 > len) return false;
    *type = ((uint16_t)packet[offset] << 8) | packet[offset + 1];
    uint16_t qclass = ((uint16_t)packet[offset + 2] << 8) | packet[offset + 3];
    *unicast = (qclass & 0x8000) != 0;
    return (qclass & 0x7fff) == 1;
}

static void *mdns_worker(void *unused) {
    (void)unused;
    char hostname[64] = "compas";
    if (gethostname(hostname, sizeof(hostname) - 1) != 0 || !hostname[0]) strcpy(hostname, "compas");
    hostname[sizeof(hostname) - 1] = '\0';
    for (char *p = hostname; *p; p++) {
        if (*p == '.') { *p = '\0'; break; }
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-')) *p = '-';
    }
    char host[256]; snprintf(host, sizeof(host), "%s.local", hostname);
    struct sockaddr_in group = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);
    int fd = -1;
    struct in_addr last_ip = {0};
    time_t last_announce = 0;
    while (atomic_load(&active)) {
        struct in_addr ip;
        if (!wifi_address(&ip)) {
            if (fd >= 0) {
                send_records(fd, &group, host, last_ip, 0, NULL, 255);
                close(fd); fd = -1; last_ip.s_addr = 0;
            }
            poll(NULL, 0, 2000);
            continue;
        }
        if (fd < 0 || ip.s_addr != last_ip.s_addr) {
            if (fd >= 0) {
                send_records(fd, &group, host, last_ip, 0, NULL, 255);
                close(fd);
            }
            fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) { poll(NULL, 0, 2000); continue; }
            int yes = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
            struct sockaddr_in bind_addr = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
            if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) { close(fd); fd = -1; poll(NULL, 0, 3000); continue; }
            struct ip_mreq membership = { .imr_multiaddr = group.sin_addr, .imr_interface = ip };
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) { close(fd); fd = -1; poll(NULL, 0, 3000); continue; }
            unsigned char ttl = 255, loop = 1;
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ip, sizeof(ip));
            last_ip = ip;
            send_records(fd, &group, host, ip, 120, NULL, 255);
            last_announce = time(NULL);
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 2000);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            uint8_t query[1500];
            struct sockaddr_in source;
            socklen_t source_len = sizeof(source);
            ssize_t got = recvfrom(fd, query, sizeof(query), 0, (struct sockaddr *)&source, &source_len);
            char qname[256]; uint16_t qtype; bool unicast;
            if (got > 0 && parse_question(query, (size_t)got, qname, sizeof(qname), &qtype, &unicast))
                send_records(fd, unicast ? &source : &group, host, ip, 120, qname, qtype);
        }
        time_t now = time(NULL);
        if (now - last_announce >= 60) {
            send_records(fd, &group, host, ip, 120, NULL, 255);
            last_announce = now;
        }
    }
    if (fd >= 0) { send_records(fd, &group, host, last_ip, 0, NULL, 255); close(fd); }
    return NULL;
}

void remote_control_mdns_start(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    if (!atomic_load(&active)) {
        atomic_store(&active, true);
        if (pthread_create(&worker, NULL, mdns_worker, NULL) != 0) atomic_store(&active, false);
    }
    pthread_mutex_unlock(&lifecycle_mutex);
}

void remote_control_mdns_stop(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    bool was_active = atomic_exchange(&active, false);
    pthread_t thread = worker;
    pthread_mutex_unlock(&lifecycle_mutex);
    if (was_active) pthread_join(thread, NULL);
}
