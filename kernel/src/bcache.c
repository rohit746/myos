// The block cache: recently used sectors, kept in memory by (disk, sector).
// See block.h.
//
// BCACHE_SECTORS entries, each holding one sector, in a hash table for
// lookups and a list in the order they were last used: a new sector takes
// the place of the one used longest ago.

#include <block.h>
#include <errno.h>
#include <kheap.h>
#include <string.h>
#include <sync.h>

#define BUCKETS 512 // a power of two

struct entry {
    struct block_device *disk; // a whole disk; NULL while the entry is free
    uint64_t lba;
    struct entry *hash_next;
    struct entry *newer, *older; // the use list
    uint8_t *data;
    uint32_t size; // bytes `data` holds: the disk's sector size
};

static struct mutex lock;
static bool ready;
static struct entry entries[BCACHE_SECTORS];
static struct entry *buckets[BUCKETS];
static struct entry *newest, *oldest;

static void init(void) {
    mutex_init(&lock, "bcache");
    for (int i = 0; i < BCACHE_SECTORS; i++) {
        struct entry *e = &entries[i];
        e->older = i + 1 < BCACHE_SECTORS ? &entries[i + 1] : NULL;
        e->newer = i ? &entries[i - 1] : NULL;
    }
    newest = &entries[0];
    oldest = &entries[BCACHE_SECTORS - 1];
    ready = true;
}

static void lock_cache(void) {
    if (!ready) init(); // the first caller is the boot thread, before anything else can be
    mutex_lock(&lock);
}

// A partition's sectors are its disk's: the cache is kept by whole disk.
static struct block_device *whole_disk(struct block_device *dev, uint64_t *lba) {
    while (dev->parent) {
        *lba += dev->start;
        dev = dev->parent;
    }
    return dev;
}

static struct entry **bucket(const struct block_device *disk, uint64_t lba) {
    uint64_t h = (lba ^ ((uintptr_t)disk >> 4)) * 0x9e3779b97f4a7c15ull;
    return &buckets[(h >> 32) & (BUCKETS - 1)];
}

static struct entry *find(const struct block_device *disk, uint64_t lba) {
    for (struct entry *e = *bucket(disk, lba); e; e = e->hash_next)
        if (e->disk == disk && e->lba == lba) return e;
    return NULL;
}

static void unlink_use(struct entry *e) {
    if (e->newer) e->newer->older = e->older;
    else newest = e->older;
    if (e->older) e->older->newer = e->newer;
    else oldest = e->newer;
}

static void make_newest(struct entry *e) {
    unlink_use(e);
    e->newer = NULL;
    e->older = newest;
    if (newest) newest->newer = e;
    newest = e;
    if (oldest == NULL) oldest = e;
}

static void make_oldest(struct entry *e) {
    unlink_use(e);
    e->older = NULL;
    e->newer = oldest;
    if (oldest) oldest->older = e;
    oldest = e;
    if (newest == NULL) newest = e;
}

// Take `e` out of the hash table, and put it first in line for reuse.
static void drop(struct entry *e) {
    struct entry **p = bucket(e->disk, e->lba);
    while (*p != e) p = &(*p)->hash_next;
    *p = e->hash_next;
    e->disk = NULL;
    make_oldest(e);
}

// Remember one sector's contents (replacing what was cached for it).
static void store(struct block_device *disk, uint64_t lba, const void *data) {
    struct entry *e = find(disk, lba);
    if (e == NULL) {
        e = oldest;
        if (e->disk) drop(e);
        if (e->size != disk->sector_size) {
            uint8_t *d = krealloc(e->data, disk->sector_size);
            if (d == NULL) return; // not cached, that's all
            e->data = d;
            e->size = disk->sector_size;
        }
        e->disk = disk;
        e->lba = lba;
        struct entry **b = bucket(disk, lba);
        e->hash_next = *b;
        *b = e;
    }
    memcpy(e->data, data, e->size);
    make_newest(e);
}

static void forget(struct block_device *disk, uint64_t lba, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        struct entry *e = find(disk, lba + i);
        if (e) drop(e);
    }
}

// Read the uncached sectors [lba, lba + count) of `disk` into `buf`,
// reading ahead past them when the disk goes on, and cache them all.
static int fill(struct block_device *disk, uint64_t lba, uint32_t count, uint8_t *buf) {
    uint32_t ss = disk->sector_size;
    uint32_t total = count;
    if (total < BCACHE_READ_AHEAD) total = BCACHE_READ_AHEAD;
    if (total > disk->sector_count - lba) total = (uint32_t)(disk->sector_count - lba);
    uint8_t *tmp = total > count ? kmalloc((uint64_t)total * ss) : NULL;
    if (tmp == NULL) total = count; // no read-ahead then
    int err = block_read(disk, lba, total, tmp ? tmp : buf);
    if (err) {
        kfree(tmp);
        return err;
    }
    if (tmp) memcpy(buf, tmp, (uint64_t)count * ss);
    const uint8_t *src = tmp ? tmp : buf;
    for (uint32_t i = 0; i < total; i++) {
        // Don't let read-ahead replace newer cached data with the disk's.
        if (i >= count && find(disk, lba + i)) continue;
        store(disk, lba + i, src + (uint64_t)i * ss);
    }
    kfree(tmp);
    return 0;
}

int bcache_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    if (count == 0 || lba >= dev->sector_count || count > dev->sector_count - lba) return -EINVAL;
    struct block_device *disk = whole_disk(dev, &lba);
    uint32_t ss = disk->sector_size;
    uint8_t *out = buf;
    int err = 0;
    lock_cache();
    for (uint32_t i = 0; i < count && err == 0;) {
        struct entry *e = find(disk, lba + i);
        if (e) {
            memcpy(out + (uint64_t)i * ss, e->data, ss);
            make_newest(e);
            i++;
            continue;
        }
        uint32_t run = 1; // uncached sectors in a row, read with one command
        while (i + run < count && find(disk, lba + i + run) == NULL) run++;
        err = fill(disk, lba + i, run, out + (uint64_t)i * ss);
        i += run;
    }
    mutex_unlock(&lock);
    return err;
}

int bcache_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (count == 0 || lba >= dev->sector_count || count > dev->sector_count - lba) return -EINVAL;
    struct block_device *disk = whole_disk(dev, &lba);
    lock_cache();
    int err = block_write(disk, lba, count, buf); // drops the old copies
    if (err == 0)
        for (uint32_t i = 0; i < count; i++) store(disk, lba + i, (const uint8_t *)buf + (uint64_t)i * disk->sector_size);
    mutex_unlock(&lock);
    return err;
}

void bcache_forget(struct block_device *dev, uint64_t lba, uint32_t count) {
    struct block_device *disk = whole_disk(dev, &lba);
    bool mine = ready && mutex_held(&lock); // called from bcache_write's block_write
    if (!mine) lock_cache();
    forget(disk, lba, count);
    if (!mine) mutex_unlock(&lock);
}
