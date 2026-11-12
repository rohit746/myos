// A DNS stub resolver (RFC 1035): one A query over UDP, asked up to three
// times, two seconds apart. See netdb.h.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DNS_PORT     53
#define TRIES        3
#define TIMEOUT_S    2
#define MAX_MESSAGE  512 // UDP answers are cut to this (no EDNS)
#define TYPE_A       1
#define CLASS_IN     1
#define RCODE_NXDOMAIN 3

int h_errno;

const char *hstrerror(int err) {
    switch (err) {
    case HOST_NOT_FOUND: return "no such host";
    case TRY_AGAIN: return "no answer from the DNS server";
    case NO_RECOVERY: return "no DNS server, or a bad answer";
    case NO_DATA: return "no IPv4 address";
    default: return "unknown error";
    }
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// The query for `name`'s A record, with message ID `id`. Its length, or 0
// if the name is no good (a label longer than 63 bytes, or too long overall).
static size_t make_query(uint8_t *q, uint16_t id, const char *name) {
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01; // recursion desired
    q[5] = 1;    // one question
    size_t len = 12;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        if (n == 0 || n > 63 || len + n + 1 > 12 + 255) return 0;
        q[len++] = (uint8_t)n;
        memcpy(q + len, name, n);
        len += n;
        name += n;
        if (*name) name++; // the dot; a trailing one ends the name
    }
    q[len++] = 0;
    q[len++] = 0, q[len++] = TYPE_A;
    q[len++] = 0, q[len++] = CLASS_IN;
    return len;
}

// Step past a name at `off` (labels, or a pointer to them): the offset after
// it, or 0 if it runs off the end.
static size_t skip_name(const uint8_t *m, size_t len, size_t off) {
    while (off < len) {
        uint8_t n = m[off];
        if (n == 0) return off + 1;
        if ((n & 0xc0) == 0xc0) return off + 2 <= len ? off + 2 : 0; // a pointer ends it
        off += 1 + (size_t)n;
    }
    return 0;
}

// The first A record of the answer: 0, or an h_errno value.
static int parse_answer(const uint8_t *m, size_t len, struct in_addr *out) {
    if (len < 12 || !(m[2] & 0x80)) return NO_RECOVERY; // not a response
    if ((m[3] & 0xf) == RCODE_NXDOMAIN) return HOST_NOT_FOUND;
    if (m[3] & 0xf) return NO_RECOVERY;
    unsigned questions = get16(m + 4), answers = get16(m + 6);
    size_t off = 12;
    for (unsigned i = 0; i < questions; i++) {
        off = skip_name(m, len, off);
        if (off == 0 || off + 4 > len) return NO_RECOVERY;
        off += 4;
    }
    for (unsigned i = 0; i < answers; i++) {
        off = skip_name(m, len, off);
        if (off == 0 || off + 10 > len) return NO_RECOVERY;
        uint16_t type = get16(m + off), class = get16(m + off + 2), rdlen = get16(m + off + 8);
        off += 10;
        if (off + rdlen > len) return NO_RECOVERY;
        if (type == TYPE_A && class == CLASS_IN && rdlen == 4) {
            memcpy(&out->s_addr, m + off, 4); // already in network order
            return 0;
        }
        off += rdlen; // a CNAME, say: its target's A record follows
    }
    return NO_DATA;
}

int dns_lookup(const char *name, struct in_addr server, in_port_t port, struct in_addr *out) {
    uint8_t query[12 + 256 + 4], answer[MAX_MESSAGE];
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint16_t id = (uint16_t)(now.tv_nsec ^ now.tv_nsec >> 16 ^ getpid());
    size_t qlen = make_query(query, id, name);
    if (qlen == 0) return HOST_NOT_FOUND;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NO_RECOVERY;
    struct timeval tv = {TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = port, .sin_addr = server};
    int result = TRY_AGAIN;
    for (int t = 0; t < TRIES && result == TRY_AGAIN; t++) {
        if (sendto(fd, query, qlen, 0, (struct sockaddr *)&to, sizeof to) < 0) {
            result = NO_RECOVERY; // no route, say
            break;
        }
        for (;;) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof from;
            ssize_t n = recvfrom(fd, answer, sizeof answer, 0, (struct sockaddr *)&from, &from_len);
            if (n < 0) break; // timed out: ask again
            // Only the answer to this query, from that server, counts.
            if (from.sin_addr.s_addr != server.s_addr || from.sin_port != port || n < 12 ||
                get16(answer) != id)
                continue;
            result = parse_answer(answer, (size_t)n, out);
            break;
        }
    }
    close(fd);
    return result;
}

int dns_server(struct in_addr *out) {
    char buf[512];
    int fd = open("/etc/resolv.conf", O_RDONLY);
    if (fd < 0) return NO_RECOVERY;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NO_RECOVERY;
    buf[n] = '\0';
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (strncmp(line, "nameserver", 10) == 0 && (line[10] == ' ' || line[10] == '\t')) {
            char *a = line + 11;
            while (*a == ' ' || *a == '\t') a++;
            char *end = a;
            while (*end && *end != ' ' && *end != '\t' && *end != '\r') end++;
            *end = '\0';
            if (inet_aton(a, out)) return 0;
        }
        line = next;
    }
    return NO_RECOVERY;
}

struct hostent *gethostbyname(const char *name) {
    static struct in_addr addr;
    static char *addrs[2] = {(char *)&addr, NULL};
    static char *aliases[1] = {NULL};
    static char host[256];
    static struct hostent h = {.h_name = host, .h_aliases = aliases, .h_addrtype = AF_INET, .h_length = 4,
                               .h_addr_list = addrs};
    size_t len = strlen(name);
    if (len >= sizeof host) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    memcpy(host, name, len + 1);
    if (inet_aton(name, &addr)) return &h;
    struct in_addr server;
    int err = dns_server(&server);
    if (err == 0) err = dns_lookup(name, server, htons(DNS_PORT), &addr);
    if (err) {
        h_errno = err;
        return NULL;
    }
    return &h;
}
