#pragma once
// Socket system calls (socket.c): Linux's numbers, constants and structures,
// for IPv4. See socket.c for what each kind of socket does.

#include <stdbool.h>
#include <stdint.h>

#define AF_INET 2

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_CLOEXEC  02000000 // accepted; nothing is exec'd over a process yet
#define SOCK_NONBLOCK 04000    // not supported

#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_BROADCAST 6
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

struct sockaddr_in {
    uint16_t sin_family; // AF_INET
    uint16_t sin_port;   // network byte order
    uint32_t sin_addr;   // network byte order
    uint8_t sin_zero[8];
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

int64_t sys_socket(int64_t domain, int64_t type, int64_t protocol);
int64_t sys_bind(int64_t fd, uint64_t addr, uint64_t len);
int64_t sys_connect(int64_t fd, uint64_t addr, uint64_t len);
int64_t sys_sendto(int64_t fd, uint64_t buf, uint64_t len, int64_t flags, uint64_t addr, uint64_t addr_len);
int64_t sys_recvfrom(int64_t fd, uint64_t buf, uint64_t len, int64_t flags, uint64_t addr, uint64_t addr_len_ptr);
int64_t sys_setsockopt(int64_t fd, int64_t level, int64_t name, uint64_t value, uint64_t len);

// read() or write() on VFS file `file`, if it is a socket; -ENOTSOCK if it
// isn't.
int64_t socket_rw(int file, uint64_t buf, uint64_t len, bool write);
