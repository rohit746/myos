// cat: print files.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Copy everything from `fd` to stdout. 0, or an errno value.
static int copy(int fd) {
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) return 0;
        if (n < 0) return errno;
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(STDOUT_FILENO, buf + done, (size_t)(n - done));
            if (w <= 0) return w < 0 ? errno : EIO;
            done += w;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: cat <path...>\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        int err = fd < 0 ? errno : copy(fd);
        if (fd >= 0) close(fd);
        if (err) {
            dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
    }
    return status;
}
