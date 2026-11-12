// Sockets: the system calls user programs reach the network through.
//
// Three kinds, by Linux's rules:
//   socket(AF_INET, SOCK_STREAM, 0)            TCP (tcp.c): connect, then read/write
//   socket(AF_INET, SOCK_DGRAM, 0)             UDP: sendto/recvfrom, or connect then read/write
//   socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)  ping: send ICMP echo requests, receive the replies
//
// A ping socket works like Linux's: the program writes an echo request
// (type 8, an 8-byte header and any payload), and the kernel puts in the
// socket's own identifier and the checksum; reads give back the echo
// replies to that identifier, ICMP header and all.
//
// A socket is a vnode (vfs.h), so it lives in the process's file
// descriptors: close, dup, read and write work on it, and children get it.
// The last close frees it; a TCP connection then finishes on its own.
//
// Calls block until done. SO_RCVTIMEO and SO_SNDTIMEO (setsockopt) bound
// how long receiving and sending (connecting too) may wait: then -EAGAIN
// (-ETIMEDOUT for connect). There is no non-blocking mode, no poll/select
// and no listening for connections yet.

#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <net.h>
#include <proc.h>
#include <socket.h>
#include <string.h>
#include <sync.h>
#include <time.h>
#include <vfs.h>

#define DGRAM_QUEUE_BYTES (64 * 1024) // datagrams waiting on one socket, at most
#define EPHEMERAL_FIRST   49152       // ports picked for sockets that didn't bind one
#define STREAM_CHUNK      1024        // bytes moved through the kernel stack at a time

struct dgram {
    struct dgram *next;
    uint32_t src;
    uint16_t sport;
    size_t len;
    uint8_t data[];
};

struct socket {
    struct vnode vnode;
    struct socket *next;
    uint8_t proto;                  // IP_PROTO_TCP, _UDP or _ICMP
    uint32_t local_ip;              // 0: any
    uint16_t local_port;            // 0: not bound yet; the identifier, for ping
    uint32_t remote_ip;
    uint16_t remote_port;
    bool connected;
    uint64_t rcvtimeo_ns, sndtimeo_ns; // 0: wait as long as it takes
    int error;                      // from an ICMP error, for the next call
    struct dgram *rx_head, *rx_tail;
    size_t rx_bytes;
    struct tcp_conn *tcp;           // once connect() is called
};

static struct socket *sockets;
static uint16_t next_port = EPHEMERAL_FIRST;

static bool port_used(uint8_t proto, uint16_t port) {
    for (struct socket *s = sockets; s; s = s->next)
        if (s->proto == proto && s->local_port == port) return true;
    return proto == IP_PROTO_TCP && tcp_port_in_use(port);
}

static uint16_t ephemeral_port(uint8_t proto) {
    for (unsigned i = 0; i < 65536 - EPHEMERAL_FIRST; i++) {
        uint16_t p = next_port;
        next_port = next_port == 65535 ? EPHEMERAL_FIRST : next_port + 1;
        if (!port_used(proto, p)) return p;
    }
    return 0;
}

static void release(struct vnode *v) {
    struct socket *s = v->data;
    mutex_lock(&net_lock);
    for (struct socket **p = &sockets; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    while (s->rx_head) {
        struct dgram *d = s->rx_head;
        s->rx_head = d->next;
        kfree(d);
    }
    if (s->tcp) tcp_close(s->tcp);
    mutex_unlock(&net_lock);
    kfree(s);
}

static const struct vnode_ops socket_ops = {.fs_name = "sockfs", .release = release};

// ---------------------------------------------------------------------------
// From the IP layer (net_lock held)
// ---------------------------------------------------------------------------

static void enqueue(struct socket *s, uint32_t src, uint16_t sport, const uint8_t *data, size_t len) {
    if (s->rx_bytes + len > DGRAM_QUEUE_BYTES) return; // full: dropped
    struct dgram *d = kmalloc(sizeof *d + len);
    if (d == NULL) return;
    *d = (struct dgram){.src = src, .sport = sport, .len = len};
    memcpy(d->data, data, len);
    if (s->rx_tail) s->rx_tail->next = d;
    else s->rx_head = d;
    s->rx_tail = d;
    s->rx_bytes += len;
    net_notify();
}

bool udp_socket_input(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, size_t len) {
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto != IP_PROTO_UDP || s->local_port != dport || (s->local_ip && s->local_ip != dst)) continue;
        if (s->connected && (s->remote_ip != src || s->remote_port != sport)) continue;
        enqueue(s, src, sport, data, len);
        return true;
    }
    return false;
}

void icmp_echo_reply_input(uint32_t src, const uint8_t *icmp, size_t len) {
    uint16_t id = get16(icmp + 4);
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto != IP_PROTO_ICMP || s->local_port != id) continue;
        if (s->connected && s->remote_ip != src) continue;
        enqueue(s, src, 0, icmp, len);
        return;
    }
}

void icmp_unreachable_input(uint32_t src, const uint8_t *icmp, size_t len) {
    // The packet that couldn't be delivered follows: its IP header, then
    // at least 8 bytes, where the ports are.
    if (len < 8 + IP_HLEN + 8) return;
    const uint8_t *ip = icmp + 8;
    size_t hlen = (size_t)(ip[0] & 0xf) * 4;
    if (hlen < IP_HLEN || len < 8 + hlen + 4) return;
    uint32_t remote = get32(ip + 16);
    uint16_t lport = get16(ip + hlen), rport = get16(ip + hlen + 2);
    int err = icmp[1] == 3 ? ECONNREFUSED : icmp[1] == 0 ? ENETUNREACH : EHOSTUNREACH;
    if (ip[9] == IP_PROTO_TCP) {
        tcp_unreachable(remote, rport, lport, err);
        return;
    }
    if (ip[9] != IP_PROTO_UDP) return;
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto == IP_PROTO_UDP && s->local_port == lport && s->connected && s->remote_ip == remote &&
            s->remote_port == rport) {
            s->error = err;
            net_notify();
        }
    }
}

void socket_print(void) {
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->proto == IP_PROTO_TCP) continue; // tcp_print shows its connection
        char a[16], b[16];
        size_t n = 0;
        for (struct dgram *d = s->rx_head; d; d = d->next) n++;
        if (s->proto == IP_PROTO_ICMP) kprintf("ping socket, identifier %u", s->local_port);
        else kprintf("udp %s:%u", ip_to_str(s->local_ip, a), s->local_port);
        if (s->connected) kprintf(" -> %s:%u", ip_to_str(s->remote_ip, b), s->remote_port);
        kprintf(", %zu datagrams waiting\n", n);
    }
}

// ---------------------------------------------------------------------------
// System calls
// ---------------------------------------------------------------------------

// The socket behind descriptor `fd`, with a reference to its file held:
// returns the file, for vfs_close() once done (without net_lock).
static int hold(int64_t fd, struct socket **out) {
    struct process *p = process_current();
    if (fd < 0 || fd >= PROC_MAX_FDS || p->fds[fd] < 0) return -EBADF;
    int file = p->fds[fd];
    struct vnode *v = vfs_hold_vnode(file);
    if (v == NULL) return -EBADF;
    if (v->type != VNODE_SOCKET) {
        vfs_close(file);
        return -ENOTSOCK;
    }
    *out = v->data;
    return file;
}

static int64_t put(int file, int64_t result) {
    vfs_close(file);
    return result;
}

static uint64_t deadline_after(uint64_t timeout_ns) {
    return timeout_ns ? clock_monotonic_ns() + timeout_ns : 0;
}

static int read_addr(uint64_t user_addr, uint64_t len, uint32_t *ip, uint16_t *port) {
    struct sockaddr_in sin;
    if (len < sizeof sin) return -EINVAL;
    if (copy_from_user(&sin, user_addr, sizeof sin)) return -EFAULT;
    if (sin.sin_family != AF_INET) return -EAFNOSUPPORT;
    *ip = ntohl(sin.sin_addr);
    *port = ntohs(sin.sin_port);
    return 0;
}

static int write_addr(uint64_t user_addr, uint64_t user_len, uint32_t ip, uint16_t port) {
    if (user_addr == 0) return 0;
    uint32_t len;
    if (copy_from_user(&len, user_len, sizeof len)) return -EFAULT;
    struct sockaddr_in sin = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = htonl(ip)};
    if (copy_to_user(user_addr, &sin, len < sizeof sin ? len : sizeof sin)) return -EFAULT;
    len = sizeof sin;
    return copy_to_user(user_len, &len, sizeof len) ? -EFAULT : 0;
}

int64_t sys_socket(int64_t domain, int64_t type, int64_t protocol) {
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type & ~(int64_t)(0xf | SOCK_CLOEXEC)) return -EINVAL; // SOCK_NONBLOCK: not supported
    int base = (int)(type & 0xf);
    uint8_t proto;
    if (base == SOCK_STREAM && (protocol == 0 || protocol == IP_PROTO_TCP)) proto = IP_PROTO_TCP;
    else if (base == SOCK_DGRAM && (protocol == 0 || protocol == IP_PROTO_UDP)) proto = IP_PROTO_UDP;
    else if (base == SOCK_DGRAM && protocol == IP_PROTO_ICMP) proto = IP_PROTO_ICMP;
    else if (base != SOCK_STREAM && base != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    else return -EPROTONOSUPPORT;

    struct process *p = process_current();
    int fd = 0;
    while (fd < PROC_MAX_FDS && p->fds[fd] >= 0) fd++;
    if (fd == PROC_MAX_FDS) return -EMFILE;
    struct socket *s = kmalloc(sizeof *s);
    if (s == NULL) return -ENOMEM;
    *s = (struct socket){.proto = proto};
    s->vnode = (struct vnode){.type = VNODE_SOCKET, .ops = &socket_ops, .data = s};
    int file = vfs_open_vnode(&s->vnode, O_RDWR);
    if (file < 0) {
        kfree(s);
        return file;
    }
    mutex_lock(&net_lock);
    s->next = sockets;
    sockets = s;
    mutex_unlock(&net_lock);
    p->fds[fd] = file;
    return fd;
}

int64_t sys_bind(int64_t fd, uint64_t user_addr, uint64_t len) {
    uint32_t ip;
    uint16_t port;
    int err = read_addr(user_addr, len, &ip, &port);
    if (err) return err;
    struct socket *s;
    int file = hold(fd, &s);
    if (file < 0) return file;
    mutex_lock(&net_lock);
    if (s->local_port || s->tcp) err = -EINVAL; // bound already
    else if (ip && !ip_is_local(ip)) err = -EADDRNOTAVAIL;
    else if (port && port_used(s->proto, port)) err = -EADDRINUSE;
    else if (port == 0 && (port = ephemeral_port(s->proto)) == 0) err = -EADDRINUSE;
    if (err == 0) {
        s->local_ip = ip;
        s->local_port = port;
    }
    mutex_unlock(&net_lock);
    return put(file, err);
}

int64_t sys_connect(int64_t fd, uint64_t user_addr, uint64_t len) {
    uint32_t ip;
    uint16_t port;
    int err = read_addr(user_addr, len, &ip, &port);
    if (err) return err;
    struct socket *s;
    int file = hold(fd, &s);
    if (file < 0) return file;
    mutex_lock(&net_lock);
    if (s->proto != IP_PROTO_TCP) {
        s->remote_ip = ip;
        s->remote_port = port;
        s->connected = true;
        if (s->local_port == 0 && (s->local_port = ephemeral_port(s->proto)) == 0) err = -EADDRINUSE;
    } else if (s->tcp) {
        err = -EISCONN;
    } else if (port == 0) {
        err = -ECONNREFUSED;
    } else {
        uint32_t src = s->local_ip ? s->local_ip : ip_source_for(ip);
        if (src == 0) err = -ENETUNREACH;
        else if (s->local_port == 0 && (s->local_port = ephemeral_port(IP_PROTO_TCP)) == 0) err = -EADDRINUSE;
        else if ((s->tcp = tcp_connect(src, s->local_port, ip, port)) == NULL) err = -ENOMEM;
        if (err == 0) {
            s->remote_ip = ip;
            s->remote_port = port;
            err = tcp_wait_connected(s->tcp, deadline_after(s->sndtimeo_ns));
            if (err) { // it can be tried again
                tcp_close(s->tcp);
                s->tcp = NULL;
            } else {
                s->connected = true;
            }
        }
    }
    mutex_unlock(&net_lock);
    return put(file, err);
}

// Send `len` bytes from user memory on a connected TCP socket.
static int64_t stream_send(struct socket *s, uint64_t buf, uint64_t len) {
    uint8_t chunk[STREAM_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done < sizeof chunk ? len - done : sizeof chunk;
        if (copy_from_user(chunk, buf + done, n)) return done ? (int64_t)done : -EFAULT;
        mutex_lock(&net_lock);
        int64_t r = s->tcp ? tcp_send(s->tcp, chunk, n, deadline_after(s->sndtimeo_ns)) : -ENOTCONN;
        mutex_unlock(&net_lock);
        if (r < 0) return done ? (int64_t)done : r;
        done += (uint64_t)r;
        if ((size_t)r < n) break;
    }
    return (int64_t)done;
}

// Send one datagram: a UDP payload, or an ICMP echo request.
static int64_t dgram_send(struct socket *s, uint64_t buf, uint64_t len, bool to_given, uint32_t ip, uint16_t port) {
    uint8_t data[ETH_MTU - IP_HLEN];
    size_t max = s->proto == IP_PROTO_UDP ? sizeof data - UDP_HLEN : sizeof data;
    if (len > max) return -EMSGSIZE;
    if (copy_from_user(data, buf, len)) return -EFAULT;
    if (s->proto == IP_PROTO_ICMP && (len < 8 || data[0] != 8 || data[1] != 0)) return -EINVAL;
    mutex_lock(&net_lock);
    int64_t err = 0;
    if (!to_given) {
        if (!s->connected) err = -EDESTADDRREQ;
        ip = s->remote_ip;
        port = s->remote_port;
    }
    if (err == 0 && s->error) {
        err = -s->error;
        s->error = 0;
    }
    if (err == 0 && s->local_port == 0 && (s->local_port = ephemeral_port(s->proto)) == 0) err = -EADDRINUSE;
    if (err == 0 && s->proto == IP_PROTO_UDP) {
        err = udp_send(NULL, s->local_ip, s->local_port, ip, port, data, len);
    } else if (err == 0) {
        put16(data + 4, s->local_port); // the identifier
        put16(data + 2, 0);
        put16(data + 2, inet_checksum(data, len, 0));
        uint32_t src = s->local_ip ? s->local_ip : ip_source_for(ip);
        err = src || ip == IP_BROADCAST ? ip_send(NULL, src, ip, IP_PROTO_ICMP, data, len, false) : -ENETUNREACH;
    }
    mutex_unlock(&net_lock);
    return err ? err : (int64_t)len;
}

static int64_t send_on(struct socket *s, uint64_t buf, uint64_t len, bool to_given, uint32_t ip, uint16_t port) {
    if (!user_range_ok(buf, len, false)) return -EFAULT;
    return s->proto == IP_PROTO_TCP ? stream_send(s, buf, len) : dgram_send(s, buf, len, to_given, ip, port);
}

// Receive into user memory: bytes from a TCP stream, or one datagram (cut
// to `len`). The sender's address goes to user_addr (if not 0).
static int64_t recv_on(struct socket *s, uint64_t buf, uint64_t len, bool dont_wait, uint64_t user_addr,
                       uint64_t user_alen) {
    if (!user_range_ok(buf, len, true)) return -EFAULT;
    uint64_t deadline = dont_wait ? 1 : deadline_after(s->rcvtimeo_ns);
    if (s->proto == IP_PROTO_TCP) {
        uint8_t chunk[2 * STREAM_CHUNK];
        mutex_lock(&net_lock);
        int64_t r = s->tcp ? tcp_recv(s->tcp, chunk, len < sizeof chunk ? len : sizeof chunk, deadline) : -ENOTCONN;
        uint32_t ip = s->remote_ip;
        uint16_t port = s->remote_port;
        mutex_unlock(&net_lock);
        if (r > 0 && copy_to_user(buf, chunk, (size_t)r)) return -EFAULT;
        if (r >= 0 && write_addr(user_addr, user_alen, ip, port)) return -EFAULT;
        return r;
    }
    mutex_lock(&net_lock);
    int64_t err = 0;
    while (s->rx_head == NULL) {
        if (s->error) {
            err = -s->error;
            s->error = 0;
            break;
        }
        if (!net_wait(deadline)) {
            err = -EAGAIN;
            break;
        }
    }
    struct dgram *d = err ? NULL : s->rx_head;
    if (d) {
        s->rx_head = d->next;
        if (s->rx_head == NULL) s->rx_tail = NULL;
        s->rx_bytes -= d->len;
    }
    mutex_unlock(&net_lock);
    if (d == NULL) return err;
    size_t n = d->len < len ? d->len : len;
    err = copy_to_user(buf, d->data, n) ? -EFAULT : write_addr(user_addr, user_alen, d->src, d->sport);
    kfree(d);
    return err ? err : (int64_t)n;
}

int64_t sys_sendto(int64_t fd, uint64_t buf, uint64_t len, int64_t flags, uint64_t user_addr, uint64_t alen) {
    if (flags & ~(int64_t)MSG_NOSIGNAL) return -EOPNOTSUPP;
    uint32_t ip = 0;
    uint16_t port = 0;
    if (user_addr) {
        int err = read_addr(user_addr, alen, &ip, &port);
        if (err) return err;
    }
    struct socket *s;
    int file = hold(fd, &s);
    if (file < 0) return file;
    return put(file, send_on(s, buf, len, user_addr != 0, ip, port));
}

int64_t sys_recvfrom(int64_t fd, uint64_t buf, uint64_t len, int64_t flags, uint64_t user_addr, uint64_t user_alen) {
    if (flags & ~(int64_t)MSG_DONTWAIT) return -EOPNOTSUPP;
    struct socket *s;
    int file = hold(fd, &s);
    if (file < 0) return file;
    return put(file, recv_on(s, buf, len, flags & MSG_DONTWAIT, user_addr, user_alen));
}

int64_t sys_setsockopt(int64_t fd, int64_t level, int64_t name, uint64_t user_val, uint64_t len) {
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    uint64_t *timeout;
    struct socket *s;
    int file = hold(fd, &s);
    if (file < 0) return file;
    switch (name) {
    case SO_RCVTIMEO: timeout = &s->rcvtimeo_ns; break;
    case SO_SNDTIMEO: timeout = &s->sndtimeo_ns; break;
    case SO_REUSEADDR:
    case SO_KEEPALIVE:
    case SO_BROADCAST: return put(file, len >= sizeof(int) ? 0 : -EINVAL); // nothing to change
    default: return put(file, -ENOPROTOOPT);
    }
    struct timeval tv;
    if (len < sizeof tv) return put(file, -EINVAL);
    if (copy_from_user(&tv, user_val, sizeof tv)) return put(file, -EFAULT);
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return put(file, -EINVAL);
    uint64_t sec = (uint64_t)tv.tv_sec > 100ull * 365 * 24 * 3600 ? 100ull * 365 * 24 * 3600 : (uint64_t)tv.tv_sec;
    mutex_lock(&net_lock);
    *timeout = sec * NS_PER_SEC + (uint64_t)tv.tv_usec * 1000;
    mutex_unlock(&net_lock);
    return put(file, 0);
}

int64_t socket_rw(int file, uint64_t buf, uint64_t len, bool write) {
    struct vnode *v = vfs_hold_vnode(file);
    if (v == NULL) return -EBADF;
    if (v->type != VNODE_SOCKET) return put(file, -ENOTSOCK);
    struct socket *s = v->data;
    return put(file, write ? send_on(s, buf, len, false, 0, 0) : recv_on(s, buf, len, false, 0, 0));
}
