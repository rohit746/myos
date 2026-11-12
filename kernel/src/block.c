// Block devices: the list of disks, range-checked reads and writes, the boot
// self-test and the `disk` shell command.

#include <block.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <shell.h>
#include <string.h>

#define MAX_DISKS 32

static struct block_device *disks[MAX_DISKS];
static size_t disk_count;

void block_register(struct block_device *dev) {
    if (disk_count == MAX_DISKS) {
        kprintf("  block: more than %d disks, %s left out\n", MAX_DISKS, dev->name);
        return;
    }
    if (block_find(dev->name)) panic("block_register: two disks called %s", dev->name);
    disks[disk_count++] = dev;
}

size_t block_device_count(void) { return disk_count; }

struct block_device *block_device(size_t i) { return i < disk_count ? disks[i] : NULL; }

struct block_device *block_find(const char *name) {
    for (size_t i = 0; i < disk_count; i++)
        if (strcmp(disks[i]->name, name) == 0) return disks[i];
    return NULL;
}

static bool in_range(const struct block_device *dev, uint64_t lba, uint32_t count) {
    return count && lba < dev->sector_count && count <= dev->sector_count - lba;
}

int block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!in_range(dev, lba, count)) return -EINVAL;
    return dev->ops->read(dev, lba, count, buf);
}

int block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!in_range(dev, lba, count)) return -EINVAL;
    return dev->ops->write(dev, lba, count, buf);
}

// "1.0 MiB", "931.5 GiB", as a disk's size.
static void format_size(char *buf, size_t size, uint64_t bytes) {
    static const char *const units[] = {"bytes", "KiB", "MiB", "GiB", "TiB", "PiB"};
    unsigned u = 0;
    uint64_t scaled = bytes * 10; // tenths
    while (scaled >= 10240 && u < 5) {
        scaled /= 1024;
        u++;
    }
    if (u == 0) ksnprintf(buf, size, "%lu bytes", bytes);
    else ksnprintf(buf, size, "%lu.%lu %s", scaled / 10, scaled % 10, units[u]);
}

static void print_disk(const struct block_device *d) {
    char sz[24];
    format_size(sz, sizeof sz, d->sector_count * d->sector_size);
    kprintf("%-7s %10s  %lu sectors of %u bytes  %s  (%s)\n", d->name, sz, d->sector_count, d->sector_size, d->model,
            d->where);
}

// ---------------------------------------------------------------------------
// Boot self-test
// ---------------------------------------------------------------------------

static uint32_t test_word(uint64_t sector, unsigned i) { return (uint32_t)(sector * 128 + i); }

// The first sector of `want` sectors at `lba` in `buf` that doesn't hold the
// test pattern (inverted with `inverted`), or -1.
static int64_t check_pattern(const uint8_t *buf, uint64_t lba, uint32_t count, uint32_t sector_size, bool inverted) {
    for (uint32_t s = 0; s < count; s++) {
        const uint8_t *sec = buf + (uint64_t)s * sector_size;
        for (unsigned i = 0; i < 128; i++) {
            uint32_t w;
            memcpy(&w, sec + i * 4, 4);
            uint32_t want = test_word(lba + s, i);
            if (w != (inverted ? ~want : want)) return (int64_t)(lba + s);
        }
    }
    return -1;
}

static bool scratch_test(struct block_device *d, char *line, size_t size) {
    uint32_t ss = d->sector_size;
    if (ss < 512 || d->sector_count < BLOCK_TEST_WRITE_LBA + BLOCK_TEST_WRITE_COUNT + 1) {
        ksnprintf(line, size, "disk self-test: %s has the test marker but is too small", d->name);
        return false;
    }
    uint8_t *buf = kmalloc((uint64_t)BLOCK_TEST_READ_COUNT * ss);
    if (buf == NULL) {
        ksnprintf(line, size, "disk self-test: no memory");
        return false;
    }
    bool ok = false;
    int64_t bad;

    // A long read, which takes several commands.
    int err = block_read(d, 1, BLOCK_TEST_READ_COUNT, buf);
    if (err) {
        ksnprintf(line, size, "disk self-test: reading %s sectors 1-%d: %s", d->name, BLOCK_TEST_READ_COUNT,
                  strerror(err));
        goto out;
    }
    if ((bad = check_pattern(buf, 1, BLOCK_TEST_READ_COUNT, ss, false)) >= 0) {
        ksnprintf(line, size, "disk self-test: %s sector %ld does not hold what the test disk has there", d->name,
                  bad);
        goto out;
    }

    // Write the inverted pattern, then read it back with a sector of the
    // original pattern on either side.
    uint64_t lba = BLOCK_TEST_WRITE_LBA;
    for (uint32_t s = 0; s < BLOCK_TEST_WRITE_COUNT; s++)
        for (unsigned i = 0; i < 128; i++) {
            uint32_t w = ~test_word(lba + s, i);
            memcpy(buf + (uint64_t)s * ss + i * 4, &w, 4);
        }
    if ((err = block_write(d, lba, BLOCK_TEST_WRITE_COUNT, buf))) {
        ksnprintf(line, size, "disk self-test: writing %s sectors %lu-%lu: %s", d->name, lba,
                  lba + BLOCK_TEST_WRITE_COUNT - 1, strerror(err));
        goto out;
    }
    memset(buf, 0, (uint64_t)(BLOCK_TEST_WRITE_COUNT + 2) * ss);
    if ((err = block_read(d, lba - 1, BLOCK_TEST_WRITE_COUNT + 2, buf))) {
        ksnprintf(line, size, "disk self-test: reading back %s sectors %lu-%lu: %s", d->name, lba - 1,
                  lba + BLOCK_TEST_WRITE_COUNT, strerror(err));
        goto out;
    }
    if ((bad = check_pattern(buf + ss, lba, BLOCK_TEST_WRITE_COUNT, ss, true)) >= 0 ||
        (bad = check_pattern(buf, lba - 1, 1, ss, false)) >= 0 ||
        (bad = check_pattern(buf + (uint64_t)(BLOCK_TEST_WRITE_COUNT + 1) * ss, lba + BLOCK_TEST_WRITE_COUNT, 1, ss,
                             false)) >= 0) {
        ksnprintf(line, size, "disk self-test: %s sector %ld reads back wrong after the write", d->name, bad);
        goto out;
    }
    ksnprintf(line, size, "%s: read sectors 1-%d, wrote %lu-%lu and read them back", d->name, BLOCK_TEST_READ_COUNT,
              lba, lba + BLOCK_TEST_WRITE_COUNT - 1);
    ok = true;
out:
    kfree(buf);
    return ok;
}

bool block_self_test(char *line, size_t size) {
    line[0] = '\0';
    char passed[200] = ""; // what each scratch disk's test did, "; " between them
    size_t used = 0;
    for (size_t i = 0; i < disk_count; i++) {
        kprintf("  ");
        print_disk(disks[i]);
    }
    for (size_t i = 0; i < disk_count; i++) {
        struct block_device *d = disks[i];
        if (d->sector_size < 512) continue;
        uint8_t *first = kmalloc(d->sector_size);
        if (first == NULL) {
            ksnprintf(line, size, "disk self-test: no memory");
            return false;
        }
        int err = block_read(d, 0, 1, first);
        bool scratch = err == 0 && memcmp(first, BLOCK_TEST_MAGIC, sizeof BLOCK_TEST_MAGIC - 1) == 0;
        kfree(first);
        if (err) {
            ksnprintf(line, size, "disk self-test: reading %s sector 0: %s", d->name, strerror(err));
            return false;
        }
        if (!scratch) continue;
        char one[96];
        if (!scratch_test(d, one, sizeof one)) {
            ksnprintf(line, size, "%s", one);
            return false;
        }
        if (used < sizeof passed)
            used += ksnprintf(passed + used, sizeof passed - used, "%s%s", used ? "; " : "", one);
    }
    if (passed[0]) ksnprintf(line, size, "disk self-test passed (%s)", passed);
    return true;
}

// ---------------------------------------------------------------------------
// Shell
// ---------------------------------------------------------------------------

// A decimal number, or hex with 0x. False if it isn't one.
static bool parse_u64(const char *s, uint64_t *out) {
    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (*s == '\0') return false;
    uint64_t v = 0;
    for (; *s; s++) {
        unsigned digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else return false;
        if (v > (UINT64_MAX - digit) / base) return false;
        v = v * base + digit;
    }
    *out = v;
    return true;
}

// As `hexdump -C` prints it.
static void hexdump(const uint8_t *p, size_t n) {
    for (size_t off = 0; off < n; off += 16) {
        kprintf("%08lx ", off);
        for (size_t i = 0; i < 16; i++) kprintf("%s%02x", i == 8 ? "  " : " ", p[off + i]);
        kprintf("  |");
        for (size_t i = 0; i < 16; i++) {
            uint8_t c = p[off + i];
            kprintf("%c", c >= 0x20 && c < 0x7f ? c : '.');
        }
        kprintf("|\n");
    }
}

// Write a pattern to one sector, read it back, and put back what was there.
static void disk_test(struct block_device *d, uint64_t lba) {
    uint32_t ss = d->sector_size;
    uint8_t *saved = kmalloc(ss), *pattern = kmalloc(ss), *check = kmalloc(ss);
    int err;
    if (saved == NULL || pattern == NULL || check == NULL) {
        kprintf("disk: no memory\n");
        goto out;
    }
    for (uint32_t i = 0; i < ss; i++) pattern[i] = (uint8_t)(i * 7 + lba + 1);
    if ((err = block_read(d, lba, 1, saved))) {
        kprintf("disk: reading %s sector %lu: %s\n", d->name, lba, strerror(err));
        goto out;
    }
    // Put the sector back even if the write failed: it may have half happened.
    int wrote = block_write(d, lba, 1, pattern);
    err = wrote ? wrote : block_read(d, lba, 1, check);
    bool same = err == 0 && memcmp(check, pattern, ss) == 0;
    int restore = block_write(d, lba, 1, saved);
    if (restore == 0) restore = block_read(d, lba, 1, check);
    bool restored = restore == 0 && memcmp(check, saved, ss) == 0;
    if (wrote) kprintf("disk: writing %s sector %lu: %s\n", d->name, lba, strerror(wrote));
    else if (err) kprintf("disk: reading back %s sector %lu: %s\n", d->name, lba, strerror(err));
    else if (!same) kprintf("disk: %s sector %lu reads back different from what was written\n", d->name, lba);
    if (!restored)
        kprintf("disk: could not put %s sector %lu back as it was%s%s\n", d->name, lba, restore ? ": " : "",
                restore ? strerror(restore) : "");
    if (err == 0 && same && restored) kprintf("%s: sector %lu written, read back and restored\n", d->name, lba);
out:
    kfree(saved);
    kfree(pattern);
    kfree(check);
}

SHELL_COMMAND(disk, "[read|test <disk> <sector>]",
              "list disks; print a sector; or write one, read it back and restore it");
static void cmd_disk(int argc, char **argv) {
    if (argc == 1) {
        if (disk_count == 0) kprintf("disk: no disks found\n");
        for (size_t i = 0; i < disk_count; i++) print_disk(disks[i]);
        return;
    }
    uint64_t lba;
    bool read = argc == 4 && strcmp(argv[1], "read") == 0, test = argc == 4 && strcmp(argv[1], "test") == 0;
    if ((!read && !test) || !parse_u64(argv[3], &lba)) {
        kprintf("usage: disk [read|test <disk> <sector>]\n");
        return;
    }
    struct block_device *d = block_find(argv[2]);
    if (d == NULL) {
        kprintf("disk: %s: no such disk\n", argv[2]);
        return;
    }
    if (lba >= d->sector_count) {
        kprintf("disk: %s has %lu sectors\n", d->name, d->sector_count);
        return;
    }
    if (test) {
        disk_test(d, lba);
        return;
    }
    uint8_t *buf = kmalloc(d->sector_size);
    if (buf == NULL) {
        kprintf("disk: no memory\n");
        return;
    }
    int err = block_read(d, lba, 1, buf);
    if (err) kprintf("disk: reading %s sector %lu: %s\n", d->name, lba, strerror(err));
    else hexdump(buf, d->sector_size);
    kfree(buf);
}
