// The socket system call wrappers, and inet_aton & co.

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static long check(long r) {
    if (r < 0 && r >= -4095) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int socket(int domain, int type, int protocol) { return (int)check(__syscall(SYS_socket, domain, type, protocol, 0, 0, 0)); }

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)check(__syscall(SYS_bind, fd, (long)addr, len, 0, 0, 0));
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)check(__syscall(SYS_connect, fd, (long)addr, len, 0, 0, 0));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addr_len) {
    return check(__syscall(SYS_sendto, fd, (long)buf, (long)len, flags, (long)addr, addr_len));
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addr_len) {
    return check(__syscall(SYS_recvfrom, fd, (long)buf, (long)len, flags, (long)addr, (long)addr_len));
}

ssize_t send(int fd, const void *buf, size_t len, int flags) { return sendto(fd, buf, len, flags, NULL, 0); }
ssize_t recv(int fd, void *buf, size_t len, int flags) { return recvfrom(fd, buf, len, flags, NULL, NULL); }

int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
    return (int)check(__syscall(SYS_setsockopt, fd, level, name, (long)value, len, 0));
}

int inet_aton(const char *s, struct in_addr *out) {
    uint32_t addr = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255) return 0;
        }
        addr = addr << 8 | v;
        if (part < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    out->s_addr = htonl(addr);
    return 1;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(src, dst);
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    uint32_t a = ntohl(((const struct in_addr *)src)->s_addr);
    int n = snprintf(dst, size, "%u.%u.%u.%u", a >> 24, a >> 16 & 0xff, a >> 8 & 0xff, a & 0xff);
    if (n < 0 || (socklen_t)n >= size) {
        errno = ENOSPC;
        return NULL;
    }
    return dst;
}

char *inet_ntoa(struct in_addr in) {
    static char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &in, buf, sizeof buf);
    return buf;
}
