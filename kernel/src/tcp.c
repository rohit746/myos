// TCP (RFC 9293), for connections this machine opens.
//
// What it does: the three-way handshake (with the MSS option), sending
// within the window the peer offers, in segments of up to its MSS;
// retransmitting everything unacknowledged when the retransmission timer
// runs out (go-back-N, the timeout doubling each time, from 1 s up to 30 s);
// probing a zero window; taking data in order into a 32 KiB receive buffer
// and offering what is free of it as the window; and both ways of closing.
//
// What it leaves out: listening for connections, out-of-order segments
// (dropped; the peer sends them again), window scaling and other options
// but MSS, RTT measurement (the timeout starts at 1 s every time), and
// congestion control: it sends as much as the peer's window allows.
//
// A connection outlives its socket: after close() it finishes sending,
// sends its FIN and waits for the peer's, then frees itself. Everything here
// runs with net_lock held.

#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <net.h>
#include <string.h>
#include <time.h>

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define OUR_MSS        (ETH_MTU - IP_HLEN - TCP_HLEN) // 1460
#define DEFAULT_MSS    536 // when the peer sends no MSS option
#define SEND_BUFFER    (16 * 1024)
#define RECV_BUFFER    (32 * 1024)
#define RTO_INITIAL_NS (1 * NS_PER_SEC)
#define RTO_MAX_NS     (30 * NS_PER_SEC)
#define SYN_RETRIES    5 // gives up connecting after about a minute
#define DATA_RETRIES   8
#define TIME_WAIT_NS   (5 * NS_PER_SEC)  // short: only this machine's own ports are at stake
#define FIN_WAIT_2_NS  (60 * NS_PER_SEC) // a closed socket waits this long for the peer's FIN

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)

enum tcp_state {
    CLOSED,
    SYN_SENT,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSING,
    TIME_WAIT,
    CLOSE_WAIT,
    LAST_ACK,
};

static const char *const state_names[] = {"CLOSED",     "SYN_SENT", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
                                          "CLOSING",    "TIME_WAIT", "CLOSE_WAIT", "LAST_ACK"};

struct tcp_conn {
    struct tcp_conn *next;
    enum tcp_state state;
    uint32_t local, remote;
    uint16_t lport, rport;
    bool user_closed; // the socket is gone: free the connection once it is CLOSED
    int error;        // why it failed (ECONNREFUSED, ECONNRESET, ETIMEDOUT), reported once

    // Sending. sbuf holds the bytes from snd_una on: sent and not yet
    // acknowledged, then not sent yet.
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd;
    uint16_t mss;
    uint8_t *sbuf;
    size_t slen;
    bool fin_queued, fin_sent, fin_acked;
    uint32_t fin_seq;

    // Receiving: a ring of rlen bytes from rhead.
    uint32_t rcv_nxt, rcv_adv; // rcv_adv: the right edge of the window last offered
    uint8_t *rbuf;
    size_t rhead, rlen;
    bool peer_fin; // the peer has closed: reads end once rbuf is empty

    uint64_t rto_ns, rtx_deadline; // 0: the retransmission timer is off
    unsigned retries;
    uint64_t close_deadline; // TIME_WAIT, or FIN_WAIT_2 after close(): when to go
    uint64_t bytes_sent, bytes_received;
};

static struct tcp_conn *conns;

static uint32_t window_of(struct tcp_conn *c) {
    size_t free = RECV_BUFFER - c->rlen;
    return free > 0xffff ? 0xffff : (uint32_t)free;
}

// One segment from (local, lport) to (remote, rport).
static void send_raw(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport, uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t window, uint16_t mss_option, const uint8_t *data, size_t len) {
    uint8_t seg[TCP_HLEN + 4 + OUR_MSS];
    size_t hlen = TCP_HLEN + (mss_option ? 4 : 0);
    if (len > OUR_MSS) len = OUR_MSS;
    put16(seg, lport);
    put16(seg + 2, rport);
    put32(seg + 4, seq);
    put32(seg + 8, flags & TCP_ACK ? ack : 0);
    seg[12] = (uint8_t)(hlen / 4 << 4);
    seg[13] = flags;
    put16(seg + 14, window);
    put16(seg + 16, 0);
    put16(seg + 18, 0);
    if (mss_option) {
        seg[20] = 2;
        seg[21] = 4;
        put16(seg + 22, mss_option);
    }
    if (len) memcpy(seg + hlen, data, len);
    size_t total = hlen + len;
    put16(seg + 16, inet_checksum(seg, total, pseudo_header_sum(local, remote, IP_PROTO_TCP, (uint16_t)total)));
    ip_send(NULL, local, remote, IP_PROTO_TCP, seg, total, true);
}

static void send_segment(struct tcp_conn *c, uint32_t seq, uint8_t flags, const uint8_t *data, size_t len) {
    uint32_t wnd = window_of(c);
    send_raw(c->local, c->lport, c->remote, c->rport, seq, c->rcv_nxt, flags, (uint16_t)wnd,
             flags & TCP_SYN ? OUR_MSS : 0, data, len);
    if (flags & TCP_ACK) c->rcv_adv = c->rcv_nxt + wnd;
    c->bytes_sent += len;
}

static void send_ack(struct tcp_conn *c) { send_segment(c, c->snd_nxt, TCP_ACK, NULL, 0); }

static void advance_max(struct tcp_conn *c) {
    if (SEQ_GT(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
}

static void arm_timer(struct tcp_conn *c) {
    if (c->rtx_deadline == 0) c->rtx_deadline = clock_monotonic_ns() + c->rto_ns;
}

// Send what the window allows of the data not sent yet, then the FIN once
// all of it is out.
static void output(struct tcp_conn *c) {
    if (c->state != ESTABLISHED && c->state != CLOSE_WAIT && c->state != FIN_WAIT_1 && c->state != CLOSING &&
        c->state != LAST_ACK)
        return;
    for (;;) {
        size_t off = c->snd_nxt - c->snd_una; // already sent
        if (off >= c->slen) break;
        size_t room = c->snd_wnd > off ? c->snd_wnd - off : 0;
        if (room == 0) break;
        size_t n = c->slen - off;
        if (n > c->mss) n = c->mss;
        if (n > room) n = room;
        bool last = off + n == c->slen;
        send_segment(c, c->snd_nxt, TCP_ACK | (last ? TCP_PSH : 0), c->sbuf + off, n);
        c->snd_nxt += (uint32_t)n;
        advance_max(c);
    }
    if (c->fin_queued && !c->fin_sent && c->snd_nxt == c->snd_una + c->slen) {
        c->fin_seq = c->snd_nxt;
        send_segment(c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
        c->snd_nxt++;
        c->fin_sent = true;
        advance_max(c);
    }
    if (c->snd_nxt != c->snd_una) arm_timer(c);
    else if (c->slen && c->snd_wnd == 0) arm_timer(c); // to probe the closed window
}

static void unlink_conn(struct tcp_conn *c) {
    for (struct tcp_conn **p = &conns; *p; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    kfree(c->sbuf);
    kfree(c->rbuf);
    kfree(c);
}

// Done with: free it if the socket is gone too.
static void set_closed(struct tcp_conn *c, int error) {
    c->state = CLOSED;
    c->rtx_deadline = 0;
    if (error && !c->error) c->error = error;
    net_notify();
    if (c->user_closed) unlink_conn(c);
}

static uint32_t new_iss(void) {
    static uint32_t counter;
    uint64_t t = clock_monotonic_ns();
    // RFC 6528 wants a hash with a secret; a clock and a counter at least
    // don't repeat between connections.
    return (uint32_t)(t / 4000) + (uint32_t)(t * 2654435761u) + (counter += 64000);
}

struct tcp_conn *tcp_connect(uint32_t local, uint16_t lport, uint32_t remote, uint16_t rport) {
    struct tcp_conn *c = kmalloc(sizeof *c);
    uint8_t *sbuf = kmalloc(SEND_BUFFER), *rbuf = kmalloc(RECV_BUFFER);
    if (c == NULL || sbuf == NULL || rbuf == NULL) {
        kfree(c);
        kfree(sbuf);
        kfree(rbuf);
        return NULL;
    }
    *c = (struct tcp_conn){
        .state = SYN_SENT,
        .local = local,
        .remote = remote,
        .lport = lport,
        .rport = rport,
        .mss = DEFAULT_MSS,
        .sbuf = sbuf,
        .rbuf = rbuf,
        .rto_ns = RTO_INITIAL_NS,
    };
    c->iss = new_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;
    c->next = conns;
    conns = c;
    send_segment(c, c->iss, TCP_SYN, NULL, 0);
    arm_timer(c);
    return c;
}

int tcp_wait_connected(struct tcp_conn *c, uint64_t deadline_ns) {
    while (c->state == SYN_SENT)
        if (!net_wait(deadline_ns)) return -ETIMEDOUT;
    if (c->state == CLOSED) return -(c->error ? c->error : ECONNREFUSED);
    return 0;
}

int64_t tcp_send(struct tcp_conn *c, const void *buf, size_t len, uint64_t deadline_ns) {
    const uint8_t *b = buf;
    size_t done = 0;
    while (done < len) {
        if (c->error) return done ? (int64_t)done : -c->error;
        if (c->state != ESTABLISHED && c->state != CLOSE_WAIT) return done ? (int64_t)done : -EPIPE;
        size_t room = SEND_BUFFER - c->slen;
        if (room == 0) {
            if (!net_wait(deadline_ns)) return done ? (int64_t)done : -EAGAIN;
            continue;
        }
        size_t n = len - done < room ? len - done : room;
        memcpy(c->sbuf + c->slen, b + done, n);
        c->slen += n;
        done += n;
        output(c);
    }
    return (int64_t)done;
}

int64_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, uint64_t deadline_ns) {
    if (len == 0) return 0;
    for (;;) {
        if (c->rlen) {
            size_t n = len < c->rlen ? len : c->rlen;
            for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = c->rbuf[(c->rhead + i) % RECV_BUFFER];
            c->rhead = (c->rhead + n) % RECV_BUFFER;
            c->rlen -= n;
            // Tell the peer about the room made, once it is worth a segment
            // (it may be waiting on a window that had closed).
            uint32_t offered = c->rcv_adv - c->rcv_nxt;
            if (c->state != CLOSED && window_of(c) >= offered + 2u * c->mss) send_ack(c);
            return (int64_t)n;
        }
        if (c->peer_fin) return 0;
        if (c->error) return -c->error;
        if (c->state == CLOSED) return 0;
        if (!net_wait(deadline_ns)) return -EAGAIN;
    }
}

void tcp_close(struct tcp_conn *c) {
    c->user_closed = true;
    c->rlen = 0; // nobody will read it
    switch (c->state) {
    case CLOSED:
    case SYN_SENT: set_closed(c, 0); break;
    case ESTABLISHED:
        c->state = FIN_WAIT_1;
        c->fin_queued = true;
        output(c);
        break;
    case CLOSE_WAIT:
        c->state = LAST_ACK;
        c->fin_queued = true;
        output(c);
        break;
    case FIN_WAIT_2: c->close_deadline = clock_monotonic_ns() + FIN_WAIT_2_NS; break;
    default: break; // closing already
    }
}

bool tcp_port_in_use(uint16_t port) {
    for (struct tcp_conn *c = conns; c; c = c->next)
        if (c->lport == port) return true;
    return false;
}

void tcp_unreachable(uint32_t remote, uint16_t rport, uint16_t lport, int err) {
    for (struct tcp_conn *c = conns; c; c = c->next)
        if (c->state == SYN_SENT && c->remote == remote && c->rport == rport && c->lport == lport) {
            set_closed(c, err);
            return;
        }
}

// A segment for no connection: refuse it with a reset (RFC 9293 3.10.7.1).
static void send_reset(uint32_t src, uint32_t dst, const uint8_t *seg, size_t dlen) {
    uint8_t flags = seg[13];
    if (flags & TCP_RST) return;
    if (flags & TCP_ACK) {
        send_raw(dst, get16(seg + 2), src, get16(seg), get32(seg + 8), 0, TCP_RST, 0, 0, NULL, 0);
    } else {
        uint32_t ack = get32(seg + 4) + (uint32_t)dlen + (flags & TCP_SYN ? 1 : 0) + (flags & TCP_FIN ? 1 : 0);
        send_raw(dst, get16(seg + 2), src, get16(seg), 0, ack, TCP_RST | TCP_ACK, 0, 0, NULL, 0);
    }
}

static uint16_t parse_mss(const uint8_t *seg, size_t hlen) {
    for (size_t i = TCP_HLEN; i < hlen;) {
        uint8_t kind = seg[i];
        if (kind == 0) break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= hlen || seg[i + 1] < 2 || i + seg[i + 1] > hlen) break;
        if (kind == 2 && seg[i + 1] == 4) return get16(seg + i + 2);
        i += seg[i + 1];
    }
    return 0;
}

// The peer acknowledged up to `ack`.
static void take_ack(struct tcp_conn *c, uint32_t ack) {
    uint32_t acked = ack - c->snd_una;
    size_t data = acked < c->slen ? acked : c->slen;
    memmove(c->sbuf, c->sbuf + data, c->slen - data);
    c->slen -= data;
    c->snd_una = ack;
    if (SEQ_LT(c->snd_nxt, c->snd_una)) c->snd_nxt = c->snd_una; // a window probe's byte
    if (c->fin_sent && ack == c->fin_seq + 1) c->fin_acked = true;
    c->retries = 0;
    c->rto_ns = RTO_INITIAL_NS;
    c->rtx_deadline = 0;
    if (c->snd_nxt != c->snd_una) arm_timer(c);
    net_notify(); // room in the send buffer
}

static void enter_time_wait(struct tcp_conn *c) {
    c->state = TIME_WAIT;
    c->rtx_deadline = 0;
    c->close_deadline = clock_monotonic_ns() + TIME_WAIT_NS;
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    if (len < TCP_HLEN) return;
    size_t hlen = (size_t)(seg[12] >> 4) * 4;
    if (hlen < TCP_HLEN || hlen > len) return;
    if (inet_checksum(seg, len, pseudo_header_sum(src, dst, IP_PROTO_TCP, (uint16_t)len)) != 0) return;
    uint16_t sport = get16(seg), dport = get16(seg + 2);
    uint32_t seq = get32(seg + 4), ack = get32(seg + 8);
    uint8_t flags = seg[13];
    uint32_t wnd = get16(seg + 14);
    const uint8_t *data = seg + hlen;
    size_t dlen = len - hlen;

    struct tcp_conn *c = conns;
    while (c && !(c->lport == dport && c->rport == sport && c->remote == src && c->local == dst)) c = c->next;
    if (c == NULL || c->state == CLOSED) {
        send_reset(src, dst, seg, dlen);
        return;
    }

    if (c->state == SYN_SENT) {
        bool ack_ok = (flags & TCP_ACK) && ack == c->iss + 1;
        if ((flags & TCP_ACK) && !ack_ok) {
            send_reset(src, dst, seg, dlen);
            return;
        }
        if (flags & TCP_RST) {
            if (ack_ok) set_closed(c, ECONNREFUSED);
            return;
        }
        if (!(flags & TCP_SYN) || !ack_ok) return; // a simultaneous open isn't supported
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = wnd;
        uint16_t mss = parse_mss(seg, hlen);
        c->mss = mss ? (mss < OUR_MSS ? mss : OUR_MSS) : DEFAULT_MSS;
        c->state = ESTABLISHED;
        c->retries = 0;
        c->rto_ns = RTO_INITIAL_NS;
        c->rtx_deadline = 0;
        send_ack(c);
        net_notify();
        output(c);
        return;
    }

    if (flags & TCP_RST) {
        // Only a reset inside the window counts (RFC 5961 makes it exact;
        // the window is enough against stray old segments).
        if (SEQ_LEQ(c->rcv_nxt, seq) && SEQ_LT(seq, c->rcv_nxt + (window_of(c) ? window_of(c) : 1))) {
            if (c->state == TIME_WAIT || c->state == LAST_ACK || c->state == CLOSING) set_closed(c, 0);
            else set_closed(c, ECONNRESET);
        }
        return;
    }
    if (flags & TCP_SYN) { // a SYN on an open connection: answer with an ACK and drop it
        send_ack(c);
        return;
    }
    if (!(flags & TCP_ACK)) return;

    if (SEQ_GT(ack, c->snd_una) && SEQ_LEQ(ack, c->snd_max)) {
        take_ack(c, ack);
        c->snd_wnd = wnd;
    } else if (ack == c->snd_una) {
        c->snd_wnd = wnd; // a window update
    } else if (SEQ_GT(ack, c->snd_max)) {
        send_ack(c); // acknowledges what was never sent
        return;
    }

    if (c->fin_acked) {
        if (c->state == FIN_WAIT_1) {
            c->state = FIN_WAIT_2;
            if (c->user_closed) c->close_deadline = clock_monotonic_ns() + FIN_WAIT_2_NS;
        } else if (c->state == CLOSING) {
            enter_time_wait(c);
        } else if (c->state == LAST_ACK) {
            set_closed(c, 0);
            return;
        }
    }

    bool need_ack = false;
    if (dlen || (flags & TCP_FIN)) {
        need_ack = true; // new data, a duplicate or out of order: say what we have
        bool can_take = c->state == ESTABLISHED || c->state == FIN_WAIT_1 || c->state == FIN_WAIT_2;
        int32_t gap = (int32_t)(seq - c->rcv_nxt);
        if (can_take && gap <= 0) {
            size_t skip = (size_t)-gap; // already have these
            size_t taken = 0, n = skip < dlen ? dlen - skip : 0;
            if (n) {
                size_t room = c->user_closed ? n : RECV_BUFFER - c->rlen;
                taken = n < room ? n : room;
                if (!c->user_closed)
                    for (size_t i = 0; i < taken; i++)
                        c->rbuf[(c->rhead + c->rlen + i) % RECV_BUFFER] = data[skip + i];
                if (!c->user_closed) c->rlen += taken;
                c->rcv_nxt += (uint32_t)taken;
                c->bytes_received += taken;
                net_notify();
            }
            if ((flags & TCP_FIN) && skip <= dlen && taken == n) {
                c->rcv_nxt++;
                c->peer_fin = true;
                if (c->state == ESTABLISHED) c->state = CLOSE_WAIT;
                else if (c->state == FIN_WAIT_1) c->state = c->fin_acked ? TIME_WAIT : CLOSING;
                else if (c->state == FIN_WAIT_2) c->state = TIME_WAIT;
                if (c->state == TIME_WAIT) enter_time_wait(c);
                net_notify();
            }
        } else if (c->state == TIME_WAIT && (flags & TCP_FIN)) {
            enter_time_wait(c); // the peer didn't get our ACK of its FIN
        }
    }
    if (need_ack) send_ack(c);
    output(c);
}

void tcp_timer(uint64_t now) {
    struct tcp_conn *next;
    for (struct tcp_conn *c = conns; c; c = next) {
        next = c->next;
        if (c->close_deadline && now >= c->close_deadline &&
            (c->state == TIME_WAIT || (c->state == FIN_WAIT_2 && c->user_closed))) {
            set_closed(c, 0);
            continue;
        }
        if (c->rtx_deadline == 0 || now < c->rtx_deadline) continue;
        c->rtx_deadline = 0;
        bool probe = c->state != SYN_SENT && c->snd_nxt == c->snd_una && c->slen && c->snd_wnd == 0;
        if (!probe && ++c->retries > (c->state == SYN_SENT ? SYN_RETRIES : DATA_RETRIES)) {
            set_closed(c, ETIMEDOUT);
            continue;
        }
        c->rto_ns = c->rto_ns * 2 > RTO_MAX_NS ? RTO_MAX_NS : c->rto_ns * 2;
        if (c->state == SYN_SENT) {
            send_segment(c, c->iss, TCP_SYN, NULL, 0);
            arm_timer(c);
        } else if (probe) {
            // One byte past the closed window: the peer answers with its
            // window, and takes the byte if it has room by now.
            send_segment(c, c->snd_una, TCP_ACK, c->sbuf, 1);
            if (SEQ_GT(c->snd_una + 1, c->snd_max)) c->snd_max = c->snd_una + 1;
            arm_timer(c);
        } else {
            // Go back to the oldest unacknowledged byte and send it all again.
            c->snd_nxt = c->snd_una;
            if (c->fin_sent && !c->fin_acked) c->fin_sent = false;
            output(c);
        }
    }
}

void tcp_print(void) {
    for (struct tcp_conn *c = conns; c; c = c->next) {
        char l[16], r[16];
        kprintf("tcp %s:%u -> %s:%u %s, %lu bytes sent, %lu received%s\n", ip_to_str(c->local, l), c->lport,
                ip_to_str(c->remote, r), c->rport, state_names[c->state], c->bytes_sent, c->bytes_received,
                c->user_closed ? " (closed by its program)" : "");
    }
}
