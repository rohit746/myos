// The network stack's arithmetic: checksums and addresses.

#include <ktest.h>
#include <net.h>
#include <string.h>

KTEST(inet_checksum_rfc1071) {
    // RFC 1071's example: the one's-complement sum of these words is ddf2.
    static const uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    KEXPECT_EQ(inet_checksum(data, sizeof data, 0), 0x220d);
    // An odd length is padded with a zero byte.
    static const uint8_t odd[] = {0x01, 0x02, 0x03};
    KEXPECT_EQ(inet_checksum(odd, sizeof odd, 0), (uint16_t)~(0x0102 + 0x0300));
}

KTEST(inet_checksum_ip_header) {
    // A real IPv4 header (192.168.0.1 -> 192.168.0.199, TTL 64, UDP), whose
    // checksum field holds b861: summing it with the field comes to zero.
    uint8_t h[20] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                     0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
    KEXPECT_EQ(inet_checksum(h, sizeof h, 0), 0);
    put16(h + 10, 0);
    KEXPECT_EQ(inet_checksum(h, sizeof h, 0), 0xb861);
}

KTEST(pseudo_header_and_byte_order) {
    // The pseudo-header's words: the addresses' halves, the protocol, the length.
    KEXPECT_EQ(pseudo_header_sum(IP_ADDR(10, 0, 2, 15), IP_ADDR(10, 0, 2, 2), IP_PROTO_UDP, 20),
               0x0a00 + 0x020f + 0x0a00 + 0x0202 + 17 + 20);
    uint8_t b[4];
    put32(b, IP_ADDR(192, 168, 1, 2));
    KEXPECT_EQ(b[0], 192);
    KEXPECT_EQ(b[3], 2);
    KEXPECT_EQ(get32(b), IP_ADDR(192, 168, 1, 2));
    put16(b, 0x1234);
    KEXPECT_EQ(get16(b), 0x1234);
    KEXPECT_EQ(htons(0x1234), 0x3412);
    KEXPECT_EQ(ntohl(htonl(0xdeadbeef)), 0xdeadbeef);
    char s[16];
    KEXPECT_EQ(strcmp(ip_to_str(IP_ADDR(10, 0, 2, 15), s), "10.0.2.15"), 0);
    KEXPECT_EQ(strcmp(ip_to_str(IP_BROADCAST, s), "255.255.255.255"), 0);
}
