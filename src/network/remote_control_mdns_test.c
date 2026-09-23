/* Protocol regression check: `make remote-control-mdns-selftest`. */
#include "remote_control_mdns.c"

#include <assert.h>

static uint16_t get16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

static void check_record(const uint8_t *packet, size_t packet_len, size_t *offset,
                         const char *expected_owner, uint16_t expected_type,
                         bool expect_cache_flush, uint8_t *rdata, uint16_t *rdlength) {
    char owner[256];
    assert(decode_name(packet, packet_len, offset, owner, sizeof(owner)));
    assert(strcasecmp(owner, expected_owner) == 0);
    assert(*offset + 10 <= packet_len);
    uint16_t type = get16(packet + *offset);
    uint16_t class = get16(packet + *offset + 2);
    uint16_t rdlen = get16(packet + *offset + 8);
    assert(type == expected_type);
    assert((class & 0x7fff) == 1);
    assert(((class & 0x8000) != 0) == expect_cache_flush);
    *offset += 10;
    assert(*offset + rdlen <= packet_len);
    if (rdata && rdlen) memcpy(rdata, packet + *offset, rdlen);
    if (rdlength) *rdlength = rdlen;
    *offset += rdlen;
}

static size_t make_ptr_query(uint8_t *packet, bool unicast) {
    memset(packet, 0, 300);
    put16(packet + 4, 1);
    size_t name_len = put_name(packet + 12, 288, SERVICE_NAME);
    put16(packet + 12 + name_len, 12);
    put16(packet + 14 + name_len, (uint16_t)(1 | (unicast ? 0x8000 : 0)));
    return 16 + name_len;
}

static void test_ptr_qu_query(void) {
    uint8_t query[300]; char name[256]; uint16_t type; bool unicast;
    size_t len = make_ptr_query(query, true);
    assert(parse_question(query, len, name, sizeof(name), &type, &unicast));
    assert(strcmp(name, SERVICE_NAME) == 0 && type == 12 && unicast);

    unsigned answers, additional;
    struct in_addr ip;
    assert(inet_pton(AF_INET, "192.168.4.20", &ip) == 1);
    uint8_t response[1400];
    len = make_answer(response, "compas.local", ip, 120, name, type, &answers, &additional);
    assert(answers == 1 && additional == 3);
    assert(get16(response + 6) == 1 && get16(response + 10) == 3);
    size_t offset = 12; uint8_t rdata[300]; uint16_t rdlen;
    check_record(response, len, &offset, SERVICE_NAME, 12, false, rdata, &rdlen);
    size_t ptr_offset = 0; char target[256];
    assert(decode_name(rdata, rdlen, &ptr_offset, target, sizeof(target)));
    assert(strcasecmp(target, INSTANCE_NAME) == 0 && ptr_offset == rdlen);

    check_record(response, len, &offset, INSTANCE_NAME, 33, true, rdata, &rdlen);
    assert(rdlen > 6 && get16(rdata + 4) == 8899);
    size_t srv_offset = 6;
    assert(decode_name(rdata, rdlen, &srv_offset, target, sizeof(target)));
    assert(strcmp(target, "compas.local") == 0 && srv_offset == rdlen);

    check_record(response, len, &offset, INSTANCE_NAME, 16, true, rdata, &rdlen);
    assert(rdlen == 6 && rdata[0] == 5 && memcmp(rdata + 1, "api=1", 5) == 0);
    check_record(response, len, &offset, "compas.local", 1, true, rdata, &rdlen);
    assert(rdlen == 4 && memcmp(rdata, &ip.s_addr, 4) == 0 && offset == len);
}

static void test_direct_txt_query_and_bad_packet(void) {
    uint8_t query[300] = {0}; char name[256]; uint16_t type; bool unicast;
    put16(query + 4, 1);
    size_t name_len = put_name(query + 12, sizeof(query) - 12, INSTANCE_NAME);
    put16(query + 12 + name_len, 16);
    put16(query + 14 + name_len, 1);
    assert(parse_question(query, 16 + name_len, name, sizeof(name), &type, &unicast));
    assert(type == 16 && !unicast);
    unsigned answers, additional; struct in_addr ip; uint8_t response[1400];
    assert(inet_pton(AF_INET, "192.168.4.20", &ip) == 1);
    size_t len = make_answer(response, "compas.local", ip, 120, name, type, &answers, &additional);
    assert(answers == 1 && additional == 0);
    size_t offset = 12; uint8_t rdata[300]; uint16_t rdlen;
    check_record(response, len, &offset, INSTANCE_NAME, 16, true, rdata, &rdlen);
    assert(rdlen == 6 && memcmp(rdata + 1, "api=1", 5) == 0 && offset == len);

    assert(!parse_question(query, 12, name, sizeof(name), &type, &unicast));
}

int main(void) {
    test_ptr_qu_query();
    test_direct_txt_query_and_bad_packet();
    puts("remote control mDNS protocol checks passed");
    return 0;
}
