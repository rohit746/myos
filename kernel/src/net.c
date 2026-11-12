// The network stack's core: interfaces, the net thread, Ethernet, ARP,
// IPv4, ICMP and UDP, and the `net` command. See net.h.

#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <net.h>
#include <sched.h>
#include <shell.h>
#include <string.h>
#include <sync.h>
#include <time.h>
#include <vfs.h>

#define POLL_INTERVAL_MS  2   // how often the net thread polls a card with no interrupt
#define TIMER_INTERVAL_MS 100 // and runs the timers, at the least
#define BOOT_WAIT_MS      4000 // net_start waits this long for an address...
#define BOOT_LINK_WAIT_MS 2500 // ...or this long, if the link is still down by then

struct mutex net_lock;
static struct condvar net_cond;
static bool lock_ready;

static struct netif *ifs[NET_MAX_IFS];
static unsigned if_count;

// The net thread sleeps on this until a card's interrupt (net_wake), or
// its next poll or timer.
static struct spinlock wake_lock = SPINLOCK_INIT("net wake");
static struct wait_queue wake_waiters;
static bool wake_pending;

static bool resolv_dirty; // write /etc/resolv.conf (net_config_changed)
static uint16_t next_ip_id;

static void init_lock(void) {
    if (lock_ready) return;
    mutex_init(&net_lock, "net");
    cond_init(&net_cond);
    lock_ready = true;
}

bool net_register(struct netif *nif) {
    init_lock();
    if (if_count == NET_MAX_IFS) return false;
    nif->index = if_count;
    ksnprintf(nif->name, sizeof nif->name, "eth%u", if_count);
    nif->dhcp = DHCP_INIT;
    ifs[if_count++] = nif;
    return true;
}

struct netif *net_interface(unsigned i) { return i < if_count ? ifs[i] : NULL; }
unsigned net_interface_count(void) { return if_count; }

void net_wake(void) {
    uint64_t flags = spin_lock_irqsave(&wake_lock);
    wake_pending = true;
    wait_queue_wake_all(&wake_waiters);
    spin_unlock_irqrestore(&wake_lock, flags);
}

void net_notify(void) { cond_broadcast(&net_cond); }

bool net_wait(uint64_t deadline_ns) {
    if (deadline_ns == 0) {
        cond_wait(&net_cond, &net_lock);
        return true;
    }
    if (clock_monotonic_ns() >= deadline_ns) return false;
    return cond_wait_until(&net_cond, &net_lock, deadline_ns) || clock_monotonic_ns() < deadline_ns;
}

void net_config_changed(void) { resolv_dirty = true; }

char *ip_to_str(uint32_t ip, char buf[16]) {
    ksnprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xff, ip >> 8 & 0xff, ip & 0xff);
    return buf;
}

static unsigned prefix_len(uint32_t mask) { return (unsigned)__builtin_popcount(mask); }

uint16_t inet_checksum(const void *data, size_t len, uint32_t sum) {
    const uint8_t *b = data;
    uint64_t s = sum;
    for (; len > 1; len -= 2, b += 2) s += (uint32_t)(b[0] << 8 | b[1]);
    if (len) s += (uint32_t)b[0] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

uint32_t pseudo_header_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len) {
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + len;
}

static bool is_loopback(uint32_t ip) { return ip >> 24 == 127; }

bool ip_is_local(uint32_t ip) {
    if (is_loopback(ip)) return true;
    for (unsigned i = 0; i < if_count; i++)
        if (ifs[i]->ip && ifs[i]->ip == ip) return true;
    return false;
}

// The interface packets to `dst` leave by, and the neighbour they go to:
// one on dst's subnet, else the first with a gateway.
static struct netif *route(uint32_t dst, uint32_t *next_hop) {
    for (unsigned i = 0; i < if_count; i++) {
        struct netif *n = ifs[i];
        if (n->ip && (dst & n->netmask) == (n->ip & n->netmask)) {
            *next_hop = dst;
            return n;
        }
    }
    for (unsigned i = 0; i < if_count; i++) {
        if (ifs[i]->ip && ifs[i]->gateway) {
            *next_hop = ifs[i]->gateway;
            return ifs[i];
        }
    }
    return NULL;
}

uint32_t ip_source_for(uint32_t dst) {
    if (is_loopback(dst)) return IP_ADDR(127, 0, 0, 1);
    if (ip_is_local(dst)) return dst;
    uint32_t hop;
    struct netif *n = route(dst, &hop);
    return n ? n->ip : 0;
}

static void link_send(struct netif *nif, const void *frame, size_t len) {
    if (nif->ops->transmit(nif, frame, len)) {
        nif->tx_packets++;
        nif->tx_bytes += len;
    } else {
        nif->tx_dropped++;
    }
}

// ---------------------------------------------------------------------------
// ARP
// ---------------------------------------------------------------------------

#define ARP_ENTRIES   32
#define ARP_QUEUE     4 // packets waiting for one neighbour's address
#define ARP_TTL_NS    (600 * NS_PER_SEC)
#define ARP_RETRY_NS  (1 * NS_PER_SEC)
#define ARP_TRIES     3

struct arp_entry {
    bool used, resolved;
    uint32_t ip;
    struct netif *nif;
    uint8_t mac[ETH_ALEN];
    uint64_t expires_ns; // resolved: when to forget it; not yet: when to ask again
    unsigned tries;
    unsigned queued;
    uint8_t *queue[ARP_QUEUE]; // frames, each after its length (a size_t)
};

static struct arp_entry arp_table[ARP_ENTRIES];
static const uint8_t broadcast_mac[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static void arp_drop_queue(struct arp_entry *e) {
    for (unsigned i = 0; i < e->queued; i++) kfree(e->queue[i]);
    if (e->nif) e->nif->tx_dropped += e->queued;
    e->queued = 0;
}

static struct arp_entry *arp_find(struct netif *nif, uint32_t ip) {
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_table[i].used && arp_table[i].nif == nif && arp_table[i].ip == ip) return &arp_table[i];
    return NULL;
}

// A free entry, or the one that expires first.
static struct arp_entry *arp_new(struct netif *nif, uint32_t ip) {
    struct arp_entry *e = NULL;
    for (int i = 0; i < ARP_ENTRIES && (e == NULL || e->used); i++)
        if (e == NULL || !arp_table[i].used || arp_table[i].expires_ns < e->expires_ns) e = &arp_table[i];
    if (e->used) arp_drop_queue(e);
    *e = (struct arp_entry){.used = true, .ip = ip, .nif = nif};
    return e;
}

static void arp_send(struct netif *nif, uint16_t op, const uint8_t *target_mac, uint32_t target_ip) {
    uint8_t f[ETH_HLEN + 28];
    memcpy(f, op == 1 ? broadcast_mac : target_mac, ETH_ALEN);
    memcpy(f + 6, nif->mac, ETH_ALEN);
    put16(f + 12, ETH_TYPE_ARP);
    uint8_t *a = f + ETH_HLEN;
    put16(a, 1);             // Ethernet
    put16(a + 2, ETH_TYPE_IPV4);
    a[4] = ETH_ALEN;
    a[5] = 4;
    put16(a + 6, op);        // 1 request, 2 reply
    memcpy(a + 8, nif->mac, ETH_ALEN);
    put32(a + 14, nif->ip);
    if (op == 1) memset(a + 18, 0, ETH_ALEN);
    else memcpy(a + 18, target_mac, ETH_ALEN);
    put32(a + 24, target_ip);
    link_send(nif, f, sizeof f);
}

// The neighbour's address is known now: send what waited for it.
static void arp_resolved(struct arp_entry *e, const uint8_t *mac) {
    memcpy(e->mac, mac, ETH_ALEN);
    e->resolved = true;
    e->tries = 0;
    e->expires_ns = clock_monotonic_ns() + ARP_TTL_NS;
    for (unsigned i = 0; i < e->queued; i++) {
        size_t len;
        memcpy(&len, e->queue[i], sizeof len);
        uint8_t *frame = e->queue[i] + sizeof len;
        memcpy(frame, mac, ETH_ALEN);
        link_send(e->nif, frame, len);
        kfree(e->queue[i]);
    }
    e->queued = 0;
}

static void arp_input(struct netif *nif, const uint8_t *a, size_t len) {
    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_TYPE_IPV4 || a[4] != ETH_ALEN || a[5] != 4) return;
    uint16_t op = get16(a + 6);
    const uint8_t *sha = a + 8;
    uint32_t spa = get32(a + 14), tpa = get32(a + 24);
    if (spa == 0) return; // a probe (RFC 5227): nothing to learn
    // RFC 826: update the sender if known; add it if the packet is for us.
    struct arp_entry *e = arp_find(nif, spa);
    bool for_us = nif->ip && tpa == nif->ip;
    if (e == NULL && for_us) e = arp_new(nif, spa);
    if (e) arp_resolved(e, sha);
    if (for_us && op == 1) arp_send(nif, 2, sha, spa);
}

// Send an IPv4 frame (Ethernet header filled in but for the destination)
// to `next_hop` on `nif`, asking for its address first if need be.
static int eth_send_ip(struct netif *nif, uint32_t next_hop, uint8_t *frame, size_t len) {
    if (next_hop == IP_BROADCAST || (nif->netmask && next_hop == (nif->ip | ~nif->netmask))) {
        memcpy(frame, broadcast_mac, ETH_ALEN);
        link_send(nif, frame, len);
        return 0;
    }
    struct arp_entry *e = arp_find(nif, next_hop);
    if (e && e->resolved) {
        memcpy(frame, e->mac, ETH_ALEN);
        link_send(nif, frame, len);
        return 0;
    }
    if (e == NULL) e = arp_new(nif, next_hop);
    if (e->queued == ARP_QUEUE) { // the oldest waits no longer
        kfree(e->queue[0]);
        memmove(e->queue, e->queue + 1, (ARP_QUEUE - 1) * sizeof e->queue[0]);
        e->queued--;
        nif->tx_dropped++;
    }
    uint8_t *copy = kmalloc(sizeof len + len);
    if (copy == NULL) return -ENOMEM;
    memcpy(copy, &len, sizeof len);
    memcpy(copy + sizeof len, frame, len);
    e->queue[e->queued++] = copy;
    if (e->tries == 0) {
        e->tries = 1;
        e->expires_ns = clock_monotonic_ns() + ARP_RETRY_NS;
        arp_send(nif, 1, NULL, next_hop);
    }
    return 0;
}

static void arp_timer(uint64_t now) {
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_table[i];
        if (!e->used || now < e->expires_ns) continue;
        if (e->resolved || e->tries >= ARP_TRIES) { // stale, or nobody answered
            arp_drop_queue(e);
            e->used = false;
            continue;
        }
        e->tries++;
        e->expires_ns = now + ARP_RETRY_NS;
        arp_send(e->nif, 1, NULL, e->ip);
    }
}

// ---------------------------------------------------------------------------
// IPv4
// ---------------------------------------------------------------------------

// Packets to this machine's own addresses, for the net thread to take in.
struct loop_packet {
    struct loop_packet *next;
    size_t len;
    uint8_t data[];
};
static struct loop_packet *loop_head, *loop_tail;

static int loopback_queue(const uint8_t *packet, size_t len) {
    struct loop_packet *p = kmalloc(sizeof *p + len);
    if (p == NULL) return -ENOMEM;
    p->next = NULL;
    p->len = len;
    memcpy(p->data, packet, len);
    if (loop_tail) loop_tail->next = p;
    else loop_head = p;
    loop_tail = p;
    net_wake();
    return 0;
}

int ip_send(struct netif *nif, uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len,
            bool dont_fragment) {
    if (len > ETH_MTU - IP_HLEN) return -EMSGSIZE;
    uint32_t next_hop = dst;
    bool loop = !nif && ip_is_local(dst);
    if (loop) {
        if (src == 0) src = ip_source_for(dst);
    } else if (dst == IP_BROADCAST) {
        if (nif == NULL) nif = if_count ? ifs[0] : NULL;
        if (nif == NULL) return -ENETUNREACH;
    } else if (nif == NULL) {
        nif = route(dst, &next_hop);
        if (nif == NULL) return -ENETUNREACH;
    } else if (nif->ip && (dst & nif->netmask) != (nif->ip & nif->netmask)) {
        if (nif->gateway == 0) return -ENETUNREACH;
        next_hop = nif->gateway;
    }
    if (src == 0 && nif) src = nif->ip;

    uint8_t frame[ETH_FRAME_MAX];
    uint8_t *ip = frame + ETH_HLEN;
    ip[0] = 0x45; // version 4, 5 words of header
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(IP_HLEN + len));
    put16(ip + 4, next_ip_id++);
    put16(ip + 6, dont_fragment ? 0x4000 : 0);
    ip[8] = 64; // TTL
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, inet_checksum(ip, IP_HLEN, 0));
    memcpy(ip + IP_HLEN, payload, len);
    if (loop) return loopback_queue(ip, IP_HLEN + len);
    memcpy(frame + 6, nif->mac, ETH_ALEN);
    put16(frame + 12, ETH_TYPE_IPV4);
    return eth_send_ip(nif, next_hop, frame, ETH_HLEN + IP_HLEN + len);
}

int udp_send(struct netif *nif, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data,
             size_t len) {
    if (len > ETH_MTU - IP_HLEN - UDP_HLEN) return -EMSGSIZE;
    if (dst == IP_BROADCAST && !nif) nif = net_interface(0); // the checksum covers the source it gets
    if (src == 0 && !nif) src = ip_source_for(dst);
    if (src == 0 && nif) src = nif->ip;
    uint8_t d[ETH_MTU - IP_HLEN];
    size_t total = UDP_HLEN + len;
    put16(d, sport);
    put16(d + 2, dport);
    put16(d + 4, (uint16_t)total);
    put16(d + 6, 0);
    memcpy(d + UDP_HLEN, data, len);
    uint16_t sum = inet_checksum(d, total, pseudo_header_sum(src, dst, IP_PROTO_UDP, (uint16_t)total));
    put16(d + 6, sum ? sum : 0xffff); // 0 would mean "no checksum"
    return ip_send(nif, src, dst, IP_PROTO_UDP, d, total, false);
}

// ICMP destination unreachable (type 3) about the packet `ip` (its header
// and the first 8 bytes after it, RFC 792).
static void icmp_send_unreachable(uint8_t code, const uint8_t *ip, size_t ip_len) {
    size_t hlen = (size_t)(ip[0] & 0xf) * 4;
    size_t quote = hlen + 8 < ip_len ? hlen + 8 : ip_len;
    uint8_t m[8 + 60 + 8];
    memset(m, 0, 8);
    m[0] = 3;
    m[1] = code;
    memcpy(m + 8, ip, quote);
    put16(m + 2, inet_checksum(m, 8 + quote, 0));
    ip_send(NULL, get32(ip + 16), get32(ip + 12), IP_PROTO_ICMP, m, 8 + quote, false);
}

static void icmp_input(uint32_t src, uint32_t dst, bool to_us, const uint8_t *m, size_t len) {
    if (len < 8 || inet_checksum(m, len, 0) != 0) return;
    switch (m[0]) {
    case 8: { // echo request: send it back as a reply (not to broadcasts)
        if (!to_us || len > ETH_MTU - IP_HLEN) return;
        uint8_t reply[ETH_MTU - IP_HLEN];
        memcpy(reply, m, len);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, inet_checksum(reply, len, 0));
        ip_send(NULL, dst, src, IP_PROTO_ICMP, reply, len, false);
        break;
    }
    case 0: icmp_echo_reply_input(src, m, len); break;
    case 3: icmp_unreachable_input(src, m, len); break;
    }
}

static void udp_input(struct netif *nif, const uint8_t *ip, size_t ip_len, uint32_t src, uint32_t dst, bool to_us,
                      const uint8_t *u, size_t len) {
    if (len < UDP_HLEN) return;
    uint16_t ulen = get16(u + 4);
    if (ulen < UDP_HLEN || ulen > len) return;
    if (get16(u + 6) && inet_checksum(u, ulen, pseudo_header_sum(src, dst, IP_PROTO_UDP, ulen)) != 0) return;
    uint16_t sport = get16(u), dport = get16(u + 2);
    if (dport == 68 && sport == 67 && nif) {
        dhcp_input(nif, u + UDP_HLEN, ulen - UDP_HLEN);
        return;
    }
    if (!udp_socket_input(src, sport, dst, dport, u + UDP_HLEN, ulen - UDP_HLEN) && to_us)
        icmp_send_unreachable(3, ip, ip_len); // port unreachable
}

// An IPv4 packet, from `nif` (NULL: looped back).
static void ip_input(struct netif *nif, const uint8_t *ip, size_t len) {
    if (len < IP_HLEN || ip[0] >> 4 != 4) return;
    size_t hlen = (size_t)(ip[0] & 0xf) * 4, total = get16(ip + 2);
    if (hlen < IP_HLEN || total < hlen || total > len || inet_checksum(ip, hlen, 0) != 0) return;
    if (get16(ip + 6) & 0x3fff) { // more fragments, or a fragment offset: not reassembled
        if (nif) nif->rx_dropped++;
        return;
    }
    uint32_t src = get32(ip + 12), dst = get32(ip + 16);
    uint8_t proto = ip[9];
    bool to_us; // unicast to this machine (not broadcast)
    if (nif == NULL) {
        to_us = true;
    } else if (nif->ip == 0) {
        // No address yet: only DHCP gets through, whatever the destination.
        if (proto != IP_PROTO_UDP) return;
        to_us = false;
    } else {
        to_us = dst == nif->ip;
        bool broadcast = dst == IP_BROADCAST || dst == (nif->ip | ~nif->netmask);
        if (!to_us && !broadcast) return; // multicast, or for someone else
    }
    const uint8_t *payload = ip + hlen;
    size_t plen = total - hlen;
    switch (proto) {
    case IP_PROTO_ICMP: icmp_input(src, dst, to_us, payload, plen); break;
    case IP_PROTO_UDP: udp_input(nif, ip, total, src, dst, to_us, payload, plen); break;
    case IP_PROTO_TCP:
        if (to_us) tcp_input(src, dst, payload, plen);
        break;
    }
}

void net_receive(struct netif *nif, const void *frame, size_t len) {
    const uint8_t *f = frame;
    mutex_lock(&net_lock);
    nif->rx_packets++;
    nif->rx_bytes += len;
    if (len >= ETH_HLEN) {
        uint16_t type = get16(f + 12);
        if (type == ETH_TYPE_ARP) arp_input(nif, f + ETH_HLEN, len - ETH_HLEN);
        else if (type == ETH_TYPE_IPV4) ip_input(nif, f + ETH_HLEN, len - ETH_HLEN);
    }
    mutex_unlock(&net_lock);
}

// ---------------------------------------------------------------------------
// The net thread
// ---------------------------------------------------------------------------

static bool boot_done; // net_start has printed its line: report changes as they happen

// "10.0.2.15/24 from DHCP server 10.0.2.2, gateway 10.0.2.2, DNS 10.0.2.3"
static void describe_config(struct netif *n, char *buf, size_t size) {
    char ip[16], server[16], gw[16], dns[16];
    ksnprintf(buf, size, "%s/%u from DHCP server %s, gateway %s, DNS %s", ip_to_str(n->ip, ip), prefix_len(n->netmask),
              ip_to_str(n->dhcp_server, server), n->gateway ? ip_to_str(n->gateway, gw) : "none",
              n->dns ? ip_to_str(n->dns, dns) : "none");
}

void net_report_config(struct netif *n) {
    if (!boot_done) return;
    char line[160];
    if (n->ip) describe_config(n, line, sizeof line);
    else ksnprintf(line, sizeof line, "no address (the DHCP lease ended)");
    kprintf("  net: %s: %s\n", n->name, line);
}

static void describe_link(struct netif *n, char *buf, size_t size) {
    unsigned mbps = 0;
    bool full = false;
    if (n->ops->link(n, &mbps, &full)) ksnprintf(buf, size, "link up, %u Mbit/s %s duplex", mbps, full ? "full" : "half");
    else ksnprintf(buf, size, "link down");
}

// /etc/resolv.conf: the DNS server DHCP gave, for user programs.
static void write_resolv_conf(void) {
    char text[64] = "";
    mutex_lock(&net_lock);
    for (unsigned i = 0; i < if_count; i++) {
        if (ifs[i]->ip && ifs[i]->dns) {
            char dns[16];
            ksnprintf(text, sizeof text, "nameserver %s\n", ip_to_str(ifs[i]->dns, dns));
            break;
        }
    }
    mutex_unlock(&net_lock);
    vfs_mkdir("/etc"); // there already, unless the initramfs had none
    int fd = vfs_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    vfs_write(fd, text, strlen(text));
    vfs_close(fd);
}

static void net_thread(void *arg) {
    bool polled = false;
    for (unsigned i = 0; i < if_count; i++) polled |= ifs[i]->polled;
    for (;;) {
        uint64_t deadline = clock_monotonic_ns() + (polled ? POLL_INTERVAL_MS : TIMER_INTERVAL_MS) * NS_PER_MS;
        uint64_t flags = spin_lock_irqsave(&wake_lock);
        while (!wake_pending)
            if (!wait_queue_sleep_until(&wake_waiters, &wake_lock, deadline)) break;
        wake_pending = false;
        spin_unlock_irqrestore(&wake_lock, flags);

        for (unsigned i = 0; i < if_count; i++) ifs[i]->ops->poll(ifs[i]);

        mutex_lock(&net_lock);
        while (loop_head) {
            struct loop_packet *p = loop_head;
            loop_head = p->next;
            if (loop_head == NULL) loop_tail = NULL;
            ip_input(NULL, p->data, p->len);
            kfree(p);
        }
        uint64_t now = clock_monotonic_ns();
        for (unsigned i = 0; i < if_count; i++) {
            struct netif *n = ifs[i];
            unsigned mbps;
            bool full;
            bool up = n->ops->link(n, &mbps, &full);
            if (up != n->link_up) {
                n->link_up = up;
                if (boot_done) {
                    char link[64];
                    describe_link(n, link, sizeof link);
                    kprintf("  net: %s: %s\n", n->name, link);
                }
            }
            dhcp_timer(n, now);
        }
        arp_timer(now);
        tcp_timer(now);
        bool resolv = resolv_dirty;
        resolv_dirty = false;
        mutex_unlock(&net_lock);
        if (resolv) write_resolv_conf();
    }
}

bool net_start(char *line, size_t size) {
    init_lock();
    for (unsigned i = 0; i < if_count; i++) {
        unsigned mbps;
        bool full;
        ifs[i]->link_up = ifs[i]->ops->link(ifs[i], &mbps, &full);
    }
    // Even with no card: it delivers what is sent to 127.0.0.1.
    if (thread_create("net", net_thread, NULL) == NULL) {
        ksnprintf(line, size, "network: no memory for the net thread");
        return false;
    }
    if (if_count == 0) {
        boot_done = true;
        ksnprintf(line, size, "network: no card myos has a driver for (127.0.0.1 works)");
        return false;
    }
    // Wait for the first card's address, so the boot log can show it.
    struct netif *n = ifs[0];
    uint64_t t0 = clock_monotonic_ns();
    for (;;) {
        uint64_t ms = (clock_monotonic_ns() - t0) / NS_PER_MS;
        mutex_lock(&net_lock);
        bool done = n->ip != 0 || ms >= BOOT_WAIT_MS || (ms >= BOOT_LINK_WAIT_MS && !n->link_up);
        if (done) {
            char link[64];
            describe_link(n, link, sizeof link);
            if (n->ip) {
                char config[160];
                describe_config(n, config, sizeof config);
                ksnprintf(line, size, "network: %s %s (%s)", n->name, config, link);
            } else {
                ksnprintf(line, size, "network: %s has no address yet (%s); DHCP carries on in the background",
                          n->name, link);
            }
        }
        boot_done = done;
        mutex_unlock(&net_lock);
        if (done) return n->ip != 0;
        thread_sleep(10);
    }
}

// ---------------------------------------------------------------------------
// The `net` command
// ---------------------------------------------------------------------------

SHELL_COMMAND(net, "", "network cards, addresses, neighbours and connections");
static void cmd_net(int argc, char **argv) {
    if (if_count == 0) {
        kprintf("no network card myos has a driver for\n");
        return;
    }
    mutex_lock(&net_lock);
    uint64_t now = clock_monotonic_ns();
    for (unsigned i = 0; i < if_count; i++) {
        struct netif *n = ifs[i];
        char link[64], a[16], b[16], c[16];
        describe_link(n, link, sizeof link);
        kprintf("%s: %s, %s\n", n->name, n->desc, link);
        kprintf("  ether %02x:%02x:%02x:%02x:%02x:%02x\n", n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4],
                n->mac[5]);
        if (n->ip) {
            kprintf("  inet %s/%u, gateway %s, DNS %s\n", ip_to_str(n->ip, a), prefix_len(n->netmask),
                    n->gateway ? ip_to_str(n->gateway, b) : "none", n->dns ? ip_to_str(n->dns, c) : "none");
            uint64_t age = (now - n->dhcp_bound_ns) / NS_PER_SEC;
            if (n->dhcp_lease_s == 0xffffffff) kprintf("  DHCP: %s, from %s, lease never ends\n",
                                                       dhcp_state_name(n->dhcp), ip_to_str(n->dhcp_server, a));
            else kprintf("  DHCP: %s, from %s, lease %u s, %lu s left\n", dhcp_state_name(n->dhcp),
                         ip_to_str(n->dhcp_server, a), n->dhcp_lease_s,
                         age < n->dhcp_lease_s ? n->dhcp_lease_s - age : 0);
        } else {
            kprintf("  inet: none yet (DHCP: %s)\n", dhcp_state_name(n->dhcp));
        }
        kprintf("  rx %lu packets, %lu bytes, %lu dropped; tx %lu packets, %lu bytes, %lu dropped\n", n->rx_packets,
                n->rx_bytes, n->rx_dropped, n->tx_packets, n->tx_bytes, n->tx_dropped);
    }
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_table[i];
        if (!e->used) continue;
        char a[16];
        if (e->resolved)
            kprintf("arp: %s is %02x:%02x:%02x:%02x:%02x:%02x (%s)\n", ip_to_str(e->ip, a), e->mac[0], e->mac[1],
                    e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->nif->name);
        else kprintf("arp: %s: asked %u times, no answer yet (%s)\n", ip_to_str(e->ip, a), e->tries, e->nif->name);
    }
    socket_print();
    tcp_print();
    mutex_unlock(&net_lock);
}
