// host: look a name up in DNS.
//
//   host name [server[:port]]
//
// Asks the DNS server in /etc/resolv.conf (or `server`, on port 53 or
// `port`) for the name's IPv4 address, and prints it.

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        dprintf(2, "usage: host name [server[:port]]\n");
        return 2;
    }
    struct in_addr server, addr;
    in_port_t port = htons(53);
    if (argc == 3) {
        char s[INET_ADDRSTRLEN + 8];
        snprintf(s, sizeof s, "%s", argv[2]);
        char *colon = strchr(s, ':');
        if (colon) {
            *colon = '\0';
            long p = strtol(colon + 1, NULL, 10);
            if (p < 1 || p > 65535) {
                dprintf(2, "host: bad port %s\n", colon + 1);
                return 2;
            }
            port = htons((uint16_t)p);
        }
        if (!inet_aton(s, &server)) {
            dprintf(2, "host: %s is not an IPv4 address\n", s);
            return 2;
        }
    } else if (dns_server(&server)) {
        dprintf(2, "host: no nameserver in /etc/resolv.conf (no DHCP lease yet?)\n");
        return 1;
    }
    int err = dns_lookup(argv[1], server, port, &addr);
    if (err) {
        printf("host: %s: %s\n", argv[1], hstrerror(err));
        return 1;
    }
    printf("%s has address %s\n", argv[1], inet_ntoa(addr));
    return 0;
}
