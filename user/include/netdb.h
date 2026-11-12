#pragma once
// Host names to IPv4 addresses, through DNS.
//
// The DNS server is the first "nameserver" line of /etc/resolv.conf, which
// the kernel writes when DHCP gives it one. A name that is already an
// address ("10.0.2.2") needs no lookup.

#include <netinet/in.h>

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;    // AF_INET
    int h_length;      // 4
    char **h_addr_list; // NULL-terminated
};
#define h_addr h_addr_list[0]

// Why the last lookup failed.
extern int h_errno;
#define HOST_NOT_FOUND 1 // the server says there is no such name
#define TRY_AGAIN      2 // no answer from the server
#define NO_RECOVERY    3 // no server to ask, or an answer that makes no sense
#define NO_DATA        4 // the name exists, but has no IPv4 address

// Look `name` up: a static hostent, overwritten by the next call, or NULL
// with h_errno set.
struct hostent *gethostbyname(const char *name);
const char *hstrerror(int err);

// myos's own: ask the DNS server at `server`:`port` (network byte order,
// as in a sockaddr_in) for the IPv4 address of `name`. 0, or an h_errno
// value.
int dns_lookup(const char *name, struct in_addr server, in_port_t port, struct in_addr *out);

// The DNS server /etc/resolv.conf names: 0, or NO_RECOVERY if it names none.
int dns_server(struct in_addr *out);
