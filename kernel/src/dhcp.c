// DHCP client (RFC 2131): each interface asks for an address, its subnet,
// a gateway and a DNS server, and renews the lease halfway through it.
//
//   INIT ──DISCOVER──> SELECTING ──OFFER, REQUEST──> REQUESTING ──ACK──> BOUND
//                                                                          │
//   (lease over) <── RENEWING <──────────── half the lease gone, REQUEST ──┘
//
// A NAK, or no answer to a REQUEST, starts over from INIT. Messages go out
// as broadcasts from 0.0.0.0 (with our address in ciaddr when renewing), and
// ask the server to broadcast its answers, so they arrive before the
// interface has an address. Nothing is sent while the link is down. The net
// thread runs dhcp_timer() with net_lock held.

#include <kprintf.h>
#include <net.h>
#include <string.h>
#include <time.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

#define OPT_PAD        0
#define OPT_SUBNET     1
#define OPT_ROUTER     3
#define OPT_DNS        6
#define OPT_HOSTNAME   12
#define OPT_REQUESTED  50
#define OPT_LEASE      51
#define OPT_MSG_TYPE   53
#define OPT_SERVER_ID  54
#define OPT_PARAMS     55
#define OPT_CLIENT_ID  61
#define OPT_END        255

#define FIXED_LEN 236 // op .. file, before the magic cookie

#define RETRY_MAX_S      16 // DISCOVER again after 2, 4, 8, 16, 16, ... seconds
#define REQUEST_TRIES    4  // REQUESTs without an answer before starting over
#define RENEW_RETRY_S    10

const char *dhcp_state_name(enum dhcp_state s) {
    switch (s) {
    case DHCP_INIT: return "starting";
    case DHCP_SELECTING: return "asking for an address";
    case DHCP_REQUESTING: return "requesting the offered address";
    case DHCP_BOUND: return "bound";
    case DHCP_RENEWING: return "renewing";
    }
    return "?";
}

static uint32_t new_xid(struct netif *n) {
    uint64_t t = clock_monotonic_ns();
    uint32_t x = (uint32_t)(t ^ t >> 29) * 2654435761u;
    return x ^ (uint32_t)n->mac[5] << 24 ^ (uint32_t)n->mac[4] << 16 ^ n->index;
}

static void send_message(struct netif *n, uint8_t type, uint64_t now) {
    uint8_t m[FIXED_LEN + 64];
    memset(m, 0, sizeof m);
    m[0] = 1; // BOOTREQUEST
    m[1] = 1; // Ethernet
    m[2] = ETH_ALEN;
    put32(m + 4, n->dhcp_xid);
    uint64_t secs = (now - n->dhcp_start_ns) / NS_PER_SEC;
    put16(m + 8, (uint16_t)(secs > 0xffff ? 0xffff : secs));
    bool renewing = n->dhcp == DHCP_RENEWING;
    put16(m + 10, renewing ? 0 : 0x8000); // broadcast the answer: we can't take unicast yet
    if (renewing) put32(m + 12, n->ip);    // ciaddr
    memcpy(m + 28, n->mac, ETH_ALEN);       // chaddr
    put32(m + FIXED_LEN, DHCP_MAGIC);

    uint8_t *o = m + FIXED_LEN + 4;
    *o++ = OPT_MSG_TYPE, *o++ = 1, *o++ = type;
    *o++ = OPT_CLIENT_ID, *o++ = 1 + ETH_ALEN, *o++ = 1;
    memcpy(o, n->mac, ETH_ALEN);
    o += ETH_ALEN;
    if (type == DHCPREQUEST && !renewing) {
        *o++ = OPT_REQUESTED, *o++ = 4;
        put32(o, n->dhcp_offer);
        o += 4;
        *o++ = OPT_SERVER_ID, *o++ = 4;
        put32(o, n->dhcp_server);
        o += 4;
    }
    *o++ = OPT_PARAMS, *o++ = 4, *o++ = OPT_SUBNET, *o++ = OPT_ROUTER, *o++ = OPT_DNS, *o++ = OPT_LEASE;
    *o++ = OPT_HOSTNAME, *o++ = 4;
    memcpy(o, "myos", 4);
    o += 4;
    *o++ = OPT_END;
    size_t len = (size_t)(o - m);
    if (len < 300) len = 300; // the smallest BOOTP message; some servers want it
    _Static_assert(sizeof m >= 300, "room for a 300-byte message");
    udp_send(n, 0, DHCP_CLIENT_PORT, IP_BROADCAST, DHCP_SERVER_PORT, m, len);
}

static uint64_t backoff_ns(unsigned tries) {
    uint64_t s = 1ull << (tries < 4 ? tries : 4);
    return (s > RETRY_MAX_S ? RETRY_MAX_S : s) * NS_PER_SEC;
}

static void start_over(struct netif *n, uint64_t now) {
    if (n->ip) {
        n->ip = n->netmask = n->gateway = n->dns = 0;
        net_config_changed();
        net_report_config(n);
    }
    n->dhcp = DHCP_INIT;
    n->dhcp_next_ns = now;
}

void dhcp_timer(struct netif *n, uint64_t now) {
    if (!n->link_up || now < n->dhcp_next_ns) return;
    switch (n->dhcp) {
    case DHCP_INIT:
        n->dhcp_xid = new_xid(n);
        n->dhcp_start_ns = now;
        n->dhcp = DHCP_SELECTING;
        n->dhcp_tries = 1;
        send_message(n, DHCPDISCOVER, now);
        n->dhcp_next_ns = now + backoff_ns(n->dhcp_tries);
        break;
    case DHCP_SELECTING:
        n->dhcp_tries++;
        send_message(n, DHCPDISCOVER, now);
        n->dhcp_next_ns = now + backoff_ns(n->dhcp_tries);
        break;
    case DHCP_REQUESTING:
        if (++n->dhcp_tries > REQUEST_TRIES) {
            start_over(n, now);
            break;
        }
        send_message(n, DHCPREQUEST, now);
        n->dhcp_next_ns = now + backoff_ns(n->dhcp_tries);
        break;
    case DHCP_BOUND: // half the lease is gone: renew it
        n->dhcp = DHCP_RENEWING;
        n->dhcp_xid = new_xid(n);
        n->dhcp_start_ns = now;
        send_message(n, DHCPREQUEST, now);
        n->dhcp_next_ns = now + RENEW_RETRY_S * NS_PER_SEC;
        break;
    case DHCP_RENEWING:
        if (now - n->dhcp_bound_ns >= (uint64_t)n->dhcp_lease_s * NS_PER_SEC) {
            start_over(n, now); // the lease ran out
            break;
        }
        send_message(n, DHCPREQUEST, now);
        n->dhcp_next_ns = now + RENEW_RETRY_S * NS_PER_SEC;
        break;
    }
}

struct dhcp_options {
    uint8_t type;
    uint32_t server, subnet, router, dns, lease;
};

static bool parse_options(const uint8_t *o, size_t len, struct dhcp_options *out) {
    *out = (struct dhcp_options){0};
    size_t i = 0;
    while (i < len) {
        uint8_t code = o[i++];
        if (code == OPT_PAD) continue;
        if (code == OPT_END) break;
        if (i >= len) return false;
        uint8_t n = o[i++];
        if (i + n > len) return false;
        const uint8_t *v = o + i;
        switch (code) {
        case OPT_MSG_TYPE: if (n >= 1) out->type = v[0]; break;
        case OPT_SERVER_ID: if (n >= 4) out->server = get32(v); break;
        case OPT_SUBNET: if (n >= 4) out->subnet = get32(v); break;
        case OPT_ROUTER: if (n >= 4) out->router = get32(v); break; // the first of the list
        case OPT_DNS: if (n >= 4) out->dns = get32(v); break;
        case OPT_LEASE: if (n >= 4) out->lease = get32(v); break;
        }
        i += n;
    }
    return out->type != 0;
}

// The subnet mask a class A, B or C address has by default (for a server
// that doesn't send one).
static uint32_t default_mask(uint32_t ip) {
    if (ip >> 31 == 0) return 0xff000000;
    if (ip >> 30 == 2) return 0xffff0000;
    return 0xffffff00;
}

void dhcp_input(struct netif *n, const uint8_t *m, size_t len) {
    if (len < FIXED_LEN + 4 || m[0] != 2 || get32(m + 4) != n->dhcp_xid || memcmp(m + 28, n->mac, ETH_ALEN) != 0 ||
        get32(m + FIXED_LEN) != DHCP_MAGIC)
        return;
    struct dhcp_options opt;
    if (!parse_options(m + FIXED_LEN + 4, len - FIXED_LEN - 4, &opt)) return;
    uint32_t yiaddr = get32(m + 16);
    uint64_t now = clock_monotonic_ns();

    if (opt.type == DHCPOFFER && n->dhcp == DHCP_SELECTING && yiaddr && opt.server) {
        n->dhcp_offer = yiaddr;
        n->dhcp_server = opt.server;
        n->dhcp = DHCP_REQUESTING;
        n->dhcp_tries = 1;
        send_message(n, DHCPREQUEST, now);
        n->dhcp_next_ns = now + backoff_ns(n->dhcp_tries);
    } else if (opt.type == DHCPACK && (n->dhcp == DHCP_REQUESTING || n->dhcp == DHCP_RENEWING) && yiaddr) {
        bool changed = n->ip != yiaddr || n->gateway != opt.router || n->dns != opt.dns;
        n->ip = yiaddr;
        n->netmask = opt.subnet ? opt.subnet : default_mask(yiaddr);
        n->gateway = opt.router;
        n->dns = opt.dns;
        if (opt.server) n->dhcp_server = opt.server;
        n->dhcp_lease_s = opt.lease ? opt.lease : 3600;
        n->dhcp_bound_ns = now;
        n->dhcp = DHCP_BOUND;
        n->dhcp_next_ns = n->dhcp_lease_s == 0xffffffff ? UINT64_MAX : now + (uint64_t)n->dhcp_lease_s * NS_PER_SEC / 2;
        if (changed) {
            net_config_changed();
            net_report_config(n);
        }
        net_notify();
    } else if (opt.type == DHCPNAK && (n->dhcp == DHCP_REQUESTING || n->dhcp == DHCP_RENEWING)) {
        start_over(n, now + NS_PER_SEC);
    }
}
