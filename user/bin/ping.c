// ping: send ICMP echo requests to a host, once a second, and time the
// replies, through a ping socket (sys/socket.h).
//
//   ping [-c count] [-s size] host
//
// It sends `count` requests (4 by default: there is no Ctrl+C for a program
// yet), each with `size` bytes of data (56), and waits up to a second for
// each reply. The exit status is 0 if any reply came back.

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_DATA 1472 // what fits in one Ethernet frame

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void sleep_us(uint64_t us) {
    struct timespec ts = {(time_t)(us / 1000000), (long)(us % 1000000 * 1000)};
    nanosleep(&ts, NULL);
}

// "1.234" (milliseconds, from microseconds)
static void print_ms(uint64_t us) { printf("%lu.%03lu", us / 1000, us % 1000); }

static int usage(void) {
    dprintf(2, "usage: ping [-c count] [-s size] host\n");
    return 2;
}

int main(int argc, char **argv) {
    long count = 4, size = 56;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) count = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) size = strtol(argv[++i], NULL, 10);
        else if (argv[i][0] == '-' || host) return usage();
        else host = argv[i];
    }
    if (host == NULL || count < 1 || size < 0 || size > MAX_DATA - 8) return usage();

    struct hostent *h = gethostbyname(host);
    if (h == NULL) {
        dprintf(2, "ping: %s: %s\n", host, hstrerror(h_errno));
        return 2;
    }
    struct sockaddr_in to = {.sin_family = AF_INET};
    memcpy(&to.sin_addr, h->h_addr, 4);
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &to.sin_addr, addr, sizeof addr);

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        dprintf(2, "ping: socket: %s\n", strerror(errno));
        return 2;
    }
    printf("PING %s (%s): %ld data bytes\n", host, addr, size);

    static uint8_t packet[MAX_DATA], reply[MAX_DATA + 8];
    long sent = 0, received = 0;
    uint64_t min = UINT64_MAX, max = 0, total = 0;
    for (long seq = 1; seq <= count; seq++) {
        memset(packet, 0, 8);
        packet[0] = 8; // echo request; the kernel fills in the identifier and checksum
        packet[6] = (uint8_t)(seq >> 8);
        packet[7] = (uint8_t)seq;
        for (long i = 0; i < size; i++) packet[8 + i] = (uint8_t)(i + seq);
        uint64_t t0 = now_us();
        if (sendto(fd, packet, 8 + (size_t)size, 0, (struct sockaddr *)&to, sizeof to) < 0) {
            printf("ping: sendto %s: %s\n", addr, strerror(errno));
        } else {
            sent++;
            // Wait out the second for this reply; an older one that turns up
            // late is not counted again.
            for (;;) {
                uint64_t waited = now_us() - t0;
                if (waited >= 1000000) {
                    printf("no reply for icmp_seq=%ld\n", seq);
                    break;
                }
                struct timeval tv = {0, (long)(1000000 - waited)};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                struct sockaddr_in from;
                socklen_t from_len = sizeof from;
                ssize_t n = recvfrom(fd, reply, sizeof reply, 0, (struct sockaddr *)&from, &from_len);
                if (n < 0) continue; // timed out: the loop says so
                long rseq = reply[6] << 8 | reply[7];
                if (n < 8 || reply[0] != 0 || rseq != seq || from.sin_addr.s_addr != to.sin_addr.s_addr) continue;
                if (n != 8 + size || memcmp(reply + 8, packet + 8, (size_t)size) != 0) {
                    printf("%zd bytes from %s: icmp_seq=%ld: wrong data\n", n, addr, seq);
                    break;
                }
                uint64_t rtt = now_us() - t0;
                received++;
                total += rtt;
                if (rtt < min) min = rtt;
                if (rtt > max) max = rtt;
                printf("%zd bytes from %s: icmp_seq=%ld time=", n, addr, seq);
                print_ms(rtt);
                printf(" ms\n");
                break;
            }
        }
        uint64_t spent = now_us() - t0;
        if (seq < count && spent < 1000000) sleep_us(1000000 - spent);
    }
    close(fd);

    printf("--- %s ping statistics ---\n", host);
    printf("%ld packets transmitted, %ld received, %ld%% packet loss\n", sent, received,
           sent ? (sent - received) * 100 / sent : 100);
    if (received) {
        printf("rtt min/avg/max = ");
        print_ms(min);
        printf("/");
        print_ms(total / (uint64_t)received);
        printf("/");
        print_ms(max);
        printf(" ms\n");
    }
    return received ? 0 : 1;
}
