// Unpack the initramfs (a USTAR archive) into the VFS. See initramfs.h.
//
// A tar archive is a series of 512-byte blocks: a header block for each
// entry, followed by its data rounded up to whole blocks, and two zero
// blocks at the end. Numbers in headers are octal text.

#include <errno.h>
#include <initramfs.h>
#include <kprintf.h>
#include <memory.h>
#include <stdint.h>
#include <string.h>
#include <vfs.h>

#define BLOCK 512

struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char linkname[100];
    char magic[6]; // "ustar\0" (POSIX) or "ustar " (old GNU)
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155]; // the path is prefix + "/" + name when this is set
    char pad[12];
};

_Static_assert(sizeof(struct ustar_header) == BLOCK, "a tar header is one block");

// An octal field: digits, maybe padded with spaces or NULs. False if there
// is anything else (such as GNU's base-256 numbers for huge values).
static bool octal(const char *f, size_t n, uint64_t *out) {
    size_t i = 0;
    while (i < n && f[i] == ' ') i++;
    uint64_t v = 0;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v >> 60) return false;
        v = v * 8 + (uint64_t)(f[i] - '0');
    }
    for (; i < n; i++)
        if (f[i] != ' ' && f[i] != '\0') return false;
    *out = v;
    return true;
}

static bool zero_block(const uint8_t *b) {
    for (int i = 0; i < BLOCK; i++)
        if (b[i]) return false;
    return true;
}

// The header's checksum: the sum of its bytes, with the checksum field
// itself counted as spaces.
static bool checksum_ok(const struct ustar_header *h) {
    uint64_t want;
    if (!octal(h->checksum, sizeof h->checksum, &want)) return false;
    const uint8_t *b = (const uint8_t *)h;
    uint64_t sum = 0;
    for (size_t i = 0; i < BLOCK; i++)
        sum += i >= offsetof(struct ustar_header, checksum) && i < offsetof(struct ustar_header, type) ? ' ' : b[i];
    return sum == want;
}

// Append a field that is NUL-terminated only if it is shorter than `n`.
static size_t append(char *out, size_t at, size_t cap, const char *f, size_t n) {
    for (size_t i = 0; i < n && f[i] && at + 1 < cap; i++) out[at++] = f[i];
    out[at] = '\0';
    return at;
}

// "/" + prefix + "/" + name. False if it does not fit.
static bool entry_path(const struct ustar_header *h, char *out, size_t cap) {
    size_t at = append(out, 0, cap, "/", 1);
    if (h->prefix[0]) {
        at = append(out, at, cap, h->prefix, sizeof h->prefix);
        at = append(out, at, cap, "/", 1);
    }
    size_t before = at;
    at = append(out, at, cap, h->name, sizeof h->name);
    size_t name_len = 0;
    while (name_len < sizeof h->name && h->name[name_len]) name_len++;
    return at - before == name_len && at <= VFS_PATH_MAX;
}

// mkdir, where it existing already is fine.
static int make_dir(const char *path) {
    int err = vfs_mkdir(path);
    return err == -EEXIST ? 0 : err;
}

// Make each directory above the last component of `path`.
static int make_parents(char *path) {
    for (size_t i = 1; path[i]; i++) {
        if (path[i] != '/') continue;
        path[i] = '\0';
        int err = make_dir(path);
        path[i] = '/';
        if (err) return err;
    }
    return 0;
}

static int write_file(const char *path, const uint8_t *data, uint64_t size) {
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return fd;
    int64_t n = vfs_write(fd, data, size);
    vfs_close(fd);
    if (n < 0) return (int)n;
    return (uint64_t)n == size ? 0 : -EIO;
}

bool initramfs_unpack(const void *tar, size_t size, char *line, size_t line_size) {
    const uint8_t *base = tar;
    uint64_t files = 0, dirs = 0, bytes = 0, skipped = 0;
    char path[VFS_PATH_MAX + 2];
    size_t off = 0;
    const char *problem = NULL;
    int err = 0;

    while (off + BLOCK <= size) {
        const struct ustar_header *h = (const void *)(base + off);
        if (zero_block(base + off)) break; // the end
        uint64_t len;
        if (memcmp(h->magic, "ustar", 5) != 0) problem = "not a USTAR header";
        else if (!checksum_ok(h)) problem = "bad header checksum";
        else if (!octal(h->size, sizeof h->size, &len)) problem = "bad size field";
        else if (len > size - off - BLOCK) problem = "archive cut short";
        else if (!entry_path(h, path, sizeof path)) problem = "path too long";
        if (problem) break;
        const uint8_t *data = base + off + BLOCK;

        if (h->type == '0' || h->type == '\0') { // regular file
            err = make_parents(path);
            if (err == 0) err = write_file(path, data, len);
            files += err == 0;
            bytes += err == 0 ? len : 0;
        } else if (h->type == '5') { // directory
            err = make_parents(path);
            if (err == 0) err = make_dir(path);
            dirs += err == 0;
        } else { // links, devices, FIFOs, extended headers
            skipped++;
        }
        if (err) {
            problem = strerror(err);
            break;
        }
        off += BLOCK + ALIGN_UP(len, BLOCK);
    }

    if (problem) {
        ksnprintf(line, line_size, "initramfs: %s%s%s at byte %zu; unpacked %lu files before it", err ? path : "",
                  err ? ": " : "", problem, off, files);
        return false;
    }
    ksnprintf(line, line_size, "initramfs: unpacked %lu files (%lu bytes) and %lu directories into / (%lu skipped)",
              files, bytes, dirs, skipped);
    return true;
}
