#pragma once
// Sockets: IPv4 only (AF_INET), with Linux's numbers.
//
//   socket(AF_INET, SOCK_STREAM, 0)            TCP: connect, then read/write (or send/recv)
//   socket(AF_INET, SOCK_DGRAM, 0)             UDP: sendto/recvfrom, or connect then send/recv
//   socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)  ping: send ICMP echo requests, receive the replies
//
// Calls block; SO_RCVTIMEO and SO_SNDTIMEO (struct timeval, sys/time.h)
// bound the wait, and then fail with EAGAIN (ETIMEDOUT for connect). TCP
// sockets connect out only: there is no listen or accept yet.

#include <sys/types.h>

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

#define AF_INET 2
#define PF_INET AF_INET

#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_CLOEXEC 02000000

#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_BROADCAST 6
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t addr_len);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *addr_len);
int setsockopt(int fd, int level, int name, const void *value, socklen_t len);
