#pragma once
// Networking: network cards, and an IPv4 stack written for myos.
//
// A driver (e1000.c) registers each card as a network interface (struct
// netif, "eth0", "eth1", ...) when it probes it. The stack above it has:
//
//   Ethernet   frames in and out; no VLANs, no jumbo frames
//   ARP        a cache of neighbours' MAC addresses; a packet to a neighbour
//              not in it waits (a few per neighbour) while it is asked for
//   IPv4       one address per interface, its subnet and a default gateway;
//              fragments that come in are dropped, none are sent (DF on TCP)
//   ICMP       echo requests are answered (the machine answers ping), echo
//              replies go to ping sockets, closed UDP ports get "port
//              unreachable"
//   UDP        datagram sockets
//   TCP        stream sockets that connect out (tcp.c): retransmission,
//              flow control, the closing handshake; no listening yet
//   DHCP       every interface asks for its address, gateway and DNS server
//              (dhcp.c) and renews the lease; /etc/resolv.conf names the
//              DNS server for user programs
//
// One kernel thread, "net", does the work that isn't a system call's: it
// takes in what the cards received (woken by their interrupts, or polling
// a card without one) and runs the timers: ARP retries, TCP retransmission,
// DHCP. One mutex (net_lock) covers the whole stack, interfaces, ARP cache,
// sockets and connections; system calls and the net thread take it.
//
// IPv4 addresses are uint32_t in host byte order everywhere in the kernel
// (10.0.2.15 is 0x0a00020f); they and the ports are swapped to network
// order only in packet headers and at the system-call boundary.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ETH_ALEN       6
#define ETH_HLEN       14
#define ETH_MTU        1500                // largest IP packet in a frame
#define ETH_FRAME_MAX  (ETH_HLEN + ETH_MTU) // no FCS: the card adds and checks it
#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define IP_HLEN  20 // no options are sent
#define UDP_HLEN 8
#define TCP_HLEN 20 // without options

#define IP_ADDR(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define IP_BROADCAST        0xffffffffu

#define NET_MAX_IFS 4

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

// Big-endian fields in packets, at any alignment.
static inline uint16_t get16(const void *p) {
    const uint8_t *b = p;
    return (uint16_t)(b[0] << 8 | b[1]);
}
static inline uint32_t get32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
static inline void put16(void *p, uint16_t v) {
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}
static inline void put32(void *p, uint32_t v) {
    uint8_t *b = p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

// ---------------------------------------------------------------------------
// Interfaces (for drivers)
// ---------------------------------------------------------------------------

struct netif;

struct netif_ops {
    // Queue one Ethernet frame (header included, no FCS; the card pads it to
    // the minimum). False if the card's transmit ring is full: it is dropped.
    // May be called from any thread, with net_lock held.
    bool (*transmit)(struct netif *nif, const void *frame, size_t len);
    // Hand every frame that came in to net_receive(). Only the net thread
    // calls it, without net_lock.
    void (*poll)(struct netif *nif);
    // Whether the link is up, and its speed in Mbit/s and duplex.
    bool (*link)(struct netif *nif, unsigned *mbps, bool *full_duplex);
};

enum dhcp_state { DHCP_INIT, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

struct netif {
    // Filled in by the driver.
    const struct netif_ops *ops;
    void *priv;
    uint8_t mac[ETH_ALEN];
    char driver[16];  // "e1000e"
    char desc[64];    // "Intel 82574L at 00:02.0, MSI on vector 0x43"
    bool polled;      // no interrupt: the net thread polls it

    // Set by net_register(): "eth0".
    char name[8];
    unsigned index;

    // The rest is under net_lock.
    uint32_t ip, netmask, gateway, dns; // 0 until DHCP gives them
    bool link_up;

    // DHCP (dhcp.c).
    enum dhcp_state dhcp;
    uint32_t dhcp_xid;
    uint32_t dhcp_server, dhcp_offer;
    uint32_t dhcp_lease_s;
    unsigned dhcp_tries;
    uint64_t dhcp_next_ns;  // when to send again, or to move on
    uint64_t dhcp_bound_ns; // when the lease began
    uint64_t dhcp_start_ns; // when this exchange began (the "secs" field)

    uint64_t rx_packets, rx_bytes, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_dropped;
};

// Add a card the driver has set up; it becomes "eth<n>". False if there are
// already NET_MAX_IFS. Called from a driver's probe (during pci_init).
bool net_register(struct netif *nif);

// From the card's interrupt handler: frames came in (or the link changed),
// so the net thread should poll. Safe in an interrupt handler.
void net_wake(void);

// A frame the card received, without its FCS. From the driver's poll().
void net_receive(struct netif *nif, const void *frame, size_t len);

// Start the net thread and DHCP on every interface; wait a few seconds for
// the first interface to get an address, so the boot log can say what it
// got. Writes a line for the boot log; true if the card got an address.
// Needs the VFS, for /etc/resolv.conf.
bool net_start(char *line, size_t size);

// ---------------------------------------------------------------------------
// The stack, inside (net.c, dhcp.c, tcp.c, socket.c). Everything here is
// called with net_lock held unless it says otherwise.
// ---------------------------------------------------------------------------

struct mutex;
extern struct mutex net_lock;

// Wake every thread waiting in net_wait() (sockets: data came in, a
// connection changed state, ...).
void net_notify(void);

// Wait until net_notify() or `deadline_ns` (0: none). Gives up net_lock
// while waiting, and has it again on return. False if the deadline passed.
bool net_wait(uint64_t deadline_ns);

// "10.0.2.15" into `buf` (16 bytes). Returns buf.
char *ip_to_str(uint32_t ip, char buf[16]);

// The Internet checksum (RFC 1071) of `len` bytes, continuing from `sum`
// (0 to start): the one's complement of the one's-complement sum.
uint16_t inet_checksum(const void *data, size_t len, uint32_t sum);
// The partial sum to start a TCP or UDP checksum from: the pseudo-header.
uint32_t pseudo_header_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

// Send an IPv4 packet with `len` bytes of `payload` from `src` (0: the
// interface's address) to `dst`. `nif` NULL: route it (the interface on
// dst's subnet, else the one with a gateway). IP_BROADCAST goes out of
// `nif` (needed then) as an Ethernet broadcast. 0, or -ENETUNREACH,
// -EMSGSIZE, -ENOMEM.
int ip_send(struct netif *nif, uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len,
            bool dont_fragment);

// The address packets to `dst` go out from (the routed interface's), or 0
// if there is no route.
uint32_t ip_source_for(uint32_t dst);

// Whether `ip` is one of this machine's addresses.
bool ip_is_local(uint32_t ip);

struct netif *net_interface(unsigned i); // NULL past the last one
unsigned net_interface_count(void);

// Sockets (socket.c): a packet for them, from the IP layer. A datagram
// for no socket is refused (false), and the sender told so.
bool udp_socket_input(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, size_t len);
void icmp_echo_reply_input(uint32_t src, const uint8_t *icmp, size_t len);
void icmp_unreachable_input(uint32_t src, const uint8_t *icmp, size_t len);

// Send a UDP datagram (no socket needed: DHCP uses this).
int udp_send(struct netif *nif, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data,
             size_t len);

// DHCP (dhcp.c): a reply to port 68, and the timer.
void dhcp_input(struct netif *nif, const uint8_t *msg, size_t len);
void dhcp_timer(struct netif *nif, uint64_t now_ns);

// The interface got (or lost) its address: write /etc/resolv.conf when the
// net thread next lets go of net_lock.
void net_config_changed(void);
// Print the interface's new address (once the boot log is done).
void net_report_config(struct netif *nif);

// TCP (tcp.c).
struct tcp_conn;
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_timer(uint64_t now_ns);
// ICMP says `remote` can't be reached: a connection still connecting fails
// with `err`.
void tcp_unreachable(uint32_t remote, uint16_t rport, uint16_t lport, int err);
// Open a connection from `lport` to `remote`:`rport` and send the SYN.
// NULL if there is no memory.
struct tcp_conn *tcp_connect(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport);
// Wait until it is established (0) or failed (-ECONNREFUSED, -ETIMEDOUT,
// ...). The deadline is for the wait (0: none): -ETIMEDOUT when it passes.
int tcp_wait_connected(struct tcp_conn *c, uint64_t deadline_ns);
// Copy up to `len` bytes into the send buffer, waiting for room; the bytes
// taken, or an error (-EPIPE after close, -ECONNRESET, ...).
int64_t tcp_send(struct tcp_conn *c, const void *buf, size_t len, uint64_t deadline_ns);
// Wait for data (deadline_ns 0: forever), then take up to `len` bytes. 0 at
// the end of the stream; -EAGAIN when the deadline passes.
int64_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, uint64_t deadline_ns);
// The socket is closed: send a FIN once the data is out; the connection
// frees itself when it is done.
void tcp_close(struct tcp_conn *c);
// Whether a connection uses local port `port`.
bool tcp_port_in_use(uint16_t port);
// Print the connections (the `net` command).
void tcp_print(void);
void socket_print(void);

// For the `net` command (dhcp.c): the state in words.
const char *dhcp_state_name(enum dhcp_state s);
