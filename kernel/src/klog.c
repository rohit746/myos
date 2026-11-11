#include <klog.h>
#include <stdint.h>

static char ring[KLOG_SIZE];
static uint64_t written; // bytes logged since boot; the newest is at (written - 1) % KLOG_SIZE

void klog_putc(char c) {
    ring[written % KLOG_SIZE] = c;
    written++;
}

size_t klog_size(void) {
    return written < KLOG_SIZE ? (size_t)written : KLOG_SIZE;
}

size_t klog_read(size_t offset, char *buf, size_t size) {
    uint64_t pos = written - klog_size() + offset;
    size_t n = 0;
    while (n < size && pos < written) buf[n++] = ring[pos++ % KLOG_SIZE];
    return n;
}
