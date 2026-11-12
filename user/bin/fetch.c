// fetch: download a web page over HTTP and print it.
//
//   fetch [-i] http://host[:port]/path
//
// It sends an HTTP/1.0 GET (so the server closes the connection when the
// page is done, and doesn't chunk it), then writes the body to stdout:
// `fetch http://example.com/ > /page.html` saves it. -i prints the
// response's status line and headers first. There is no TLS, so no https.
// The exit status is 0 for a 2xx answer that arrived whole.

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define TIMEOUT_S   20
#define HEADERS_MAX 8192

static int usage(void) {
    dprintf(2, "usage: fetch [-i] http://host[:port]/path\n");
    return 2;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

// Case-insensitive prefix match.
static int starts_with(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        char a = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s, b = *prefix >= 'A' && *prefix <= 'Z' ? *prefix + 32 : *prefix;
        if (a != b) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int show_headers = 0;
    const char *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) show_headers = 1;
        else if (argv[i][0] == '-' || url) return usage();
        else url = argv[i];
    }
    if (url == NULL) return usage();
    if (starts_with(url, "https://")) {
        dprintf(2, "fetch: https is not supported (no TLS yet); try http://\n");
        return 2;
    }
    if (!starts_with(url, "http://")) return usage();

    // http://host[:port][/path]
    static char host[256];
    const char *p = url + 7, *path = strchr(p, '/');
    size_t host_len = path ? (size_t)(path - p) : strlen(p);
    if (host_len == 0 || host_len >= sizeof host) return usage();
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    if (path == NULL) path = "/";
    long port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = strtol(colon + 1, NULL, 10);
        if (port < 1 || port > 65535) {
            dprintf(2, "fetch: bad port in %s\n", url);
            return 2;
        }
    }

    struct hostent *h = gethostbyname(host);
    if (h == NULL) {
        dprintf(2, "fetch: %s: %s\n", host, hstrerror(h_errno));
        return 1;
    }
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    memcpy(&to.sin_addr, h->h_addr, 4);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        dprintf(2, "fetch: socket: %s\n", strerror(errno));
        return 1;
    }
    struct timeval tv = {TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&to, sizeof to) < 0) {
        dprintf(2, "fetch: connect to %s (%s) port %ld: %s\n", host, inet_ntoa(to.sin_addr), port, strerror(errno));
        return 1;
    }

    static char request[1024];
    int n = snprintf(request, sizeof request,
                     "GET %s HTTP/1.0\r\nHost: %s%s%s\r\nUser-Agent: myos-fetch/1.0\r\nAccept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, port == 80 ? "" : ":", port == 80 ? "" : colon + 1);
    if (n < 0 || (size_t)n >= sizeof request) {
        dprintf(2, "fetch: the URL is too long\n");
        return 2;
    }
    if (write_all(fd, request, (size_t)n) < 0) {
        dprintf(2, "fetch: sending the request: %s\n", strerror(errno));
        return 1;
    }

    // Read until the end of the headers, then pass the body through.
    static char buf[HEADERS_MAX + 1];
    size_t have = 0;
    char *body = NULL;
    while (body == NULL) {
        if (have == HEADERS_MAX) {
            dprintf(2, "fetch: the response headers are over %d bytes\n", HEADERS_MAX);
            return 1;
        }
        ssize_t r = read(fd, buf + have, HEADERS_MAX - have);
        if (r < 0) {
            dprintf(2, "fetch: %s\n", errno == EAGAIN ? "no answer in time" : strerror(errno));
            return 1;
        }
        if (r == 0) {
            dprintf(2, "fetch: the connection closed before the headers ended\n");
            return 1;
        }
        have += (size_t)r;
        buf[have] = '\0';
        char *end = strstr(buf, "\r\n\r\n");
        if (end) body = end + 4;
    }
    int status = 0;
    if (!starts_with(buf, "HTTP/1.") || buf[8] != ' ' || (status = atoi(buf + 9)) < 100) {
        dprintf(2, "fetch: not an HTTP response\n");
        return 1;
    }
    long length = -1; // Content-Length, if given
    for (char *line = strstr(buf, "\r\n") + 2; line < body - 2; line = strstr(line, "\r\n") + 2)
        if (starts_with(line, "content-length:")) length = strtol(line + 15, NULL, 10);
    if (show_headers && write_all(1, buf, (size_t)(body - buf)) < 0) return 1;

    long got = (long)(buf + have - body);
    if (got && write_all(1, body, (size_t)got) < 0) return 1;
    static char chunk[4096];
    for (;;) {
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r == 0) break;
        if (r < 0) {
            dprintf(2, "fetch: %s after %ld bytes\n", errno == EAGAIN ? "no more data in time" : strerror(errno), got);
            return 1;
        }
        if (write_all(1, chunk, (size_t)r) < 0) {
            dprintf(2, "fetch: writing: %s\n", strerror(errno));
            return 1;
        }
        got += r;
    }
    close(fd);
    if (length >= 0 && got != length) {
        dprintf(2, "fetch: got %ld of %ld bytes\n", got, length);
        return 1;
    }
    if (status < 200 || status > 299) {
        char *eol = strstr(buf, "\r\n");
        *eol = '\0';
        dprintf(2, "fetch: %s\n", buf + 9);
        return 1;
    }
    return 0;
}
