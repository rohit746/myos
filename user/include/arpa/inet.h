#pragma once
// Dotted-quad IPv4 addresses ("10.0.2.15") to and from struct in_addr.

#include <netinet/in.h>

#define INET_ADDRSTRLEN 16

// "a.b.c.d" (each part 0-255) into *out: 1, or 0 if it isn't one.
int inet_aton(const char *s, struct in_addr *out);
// The same for AF_INET: 1, 0 if it isn't an address, -1 (EAFNOSUPPORT) for
// another family.
int inet_pton(int af, const char *src, void *dst);
// "a.b.c.d" into dst (at least INET_ADDRSTRLEN bytes); NULL if it doesn't fit.
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
// The same in a static buffer, overwritten by the next call.
char *inet_ntoa(struct in_addr in);
