// FAT12, FAT16 and FAT32 filesystems on block devices: read and write.
// See vfs.h (fat_mount_all) and https://wiki.osdev.org/FAT.
//
// A FAT volume starts with a boot sector whose BIOS parameter block (BPB)
// gives the layout: reserved sectors, then the FATs (usually two copies),
// then, on FAT12 and FAT16, a fixed-size root directory, then the data
// area, cut into clusters numbered from 2. The FAT has one entry per
// cluster: the next cluster of the file it belongs to, an end-of-chain
// mark, or 0 when it is free. How many clusters there are decides the FAT
// type (and so whether an entry is 12, 16 or 32 bits), not what the boot
// sector calls it.
//
// A directory is an array of 32-byte entries: an 8.3 "short" name,
// attributes, times, first cluster and size. A long name goes in entries
// just before it (attribute 0x0f, 13 UTF-16 characters each, last part
// first), tied to the short entry by a checksum of the short name. The
// first byte of a name is e5 for a deleted entry, and 00 after the last one.
//
// Every file and directory that has been looked at gets a fat_node, which
// stays in memory until it is deleted; a directory reads all its entries
// the first time it is used. Names match without regard to ASCII case, as
// on Windows, by their long or their short name.
//
// Writes go through the block cache straight to the disk. Both FAT copies
// are kept the same. On FAT32 the FSInfo sector's free-cluster count is
// set to "unknown" by the first change rather than kept up to date.

#include <block.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <shell.h>
#include <string.h>
#include <time.h>
#include <vfs.h>

#define DIRENT_SIZE    32
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0f // read-only, hidden, system and volume ID at once
#define ENTRY_DELETED  0xe5
#define LFN_LAST       0x40 // in the sequence number of the first long-name entry
#define LFN_CHARS      13   // UTF-16 characters in one long-name entry
#define LFN_MAX        255  // characters in a long name
#define LFN_MAX_SLOTS  ((LFN_MAX + LFN_CHARS - 1) / LFN_CHARS)
#define CASE_LOWER_BASE 0x08 // in the short entry: show the name part in lower case
#define CASE_LOWER_EXT  0x10 // ... and the extension
#define DIR_MAX_BYTES  (65536 * DIRENT_SIZE) // a directory has at most 65536 entries
#define FILE_MAX       0xffffffffull        // the size field is 32 bits
#define MAX_MOUNTS     16

// The boot sector, as far as it is the same for every FAT type.
struct bpb {
    uint8_t jump[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fats;
    uint16_t root_entries; // 0 on FAT32
    uint16_t total16;
    uint8_t media;
    uint16_t fat_size16; // 0 on FAT32
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total32;
} __attribute__((packed));

// What follows it on FAT32.
struct bpb32 {
    uint32_t fat_size32;
    uint16_t ext_flags; // bit 7: only one FAT is used, the one in bits 0-3
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo;
    uint16_t backup_boot;
    uint8_t reserved[12];
} __attribute__((packed));

// The extended boot record: after struct bpb on FAT12/16, after struct
// bpb32 on FAT32.
struct ebr {
    uint8_t drive;
    uint8_t reserved;
    uint8_t signature; // 0x29: the next three fields are there
    uint32_t volume_id;
    char label[11];
    char fs_type[8];
} __attribute__((packed));

struct dirent {
    uint8_t name[11]; // 8.3, space padded, no dot
    uint8_t attr;
    uint8_t nt_case; // CASE_LOWER_*
    uint8_t ctime_tenths;
    uint16_t ctime, cdate, adate;
    uint16_t cluster_hi; // FAT32
    uint16_t mtime, mdate;
    uint16_t cluster_lo;
    uint32_t size;
} __attribute__((packed));

_Static_assert(sizeof(struct dirent) == DIRENT_SIZE, "a directory entry is 32 bytes");
_Static_assert(sizeof(struct bpb) == 36 && sizeof(struct bpb32) == 28, "BPB layout");

// Where the 13 characters of a long-name entry are, in bytes.
static const uint8_t lfn_offsets[LFN_CHARS] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
#define LFN_CHECKSUM_OFFSET 13

struct fat_fs {
    struct block_device *dev;
    unsigned type; // 12, 16 or 32
    const struct vnode_ops *ops;
    uint32_t sector_size;
    uint32_t cluster_sectors, cluster_size; // cluster_size in bytes
    uint32_t fat_start, fat_sectors, fats;  // in sectors from the start of the volume
    uint32_t active_fat;                    // the FAT read
    bool mirror;                            // write every FAT (else only the active one)
    uint32_t root_start, root_sectors, root_entries; // FAT12/16: the fixed root directory
    uint32_t data_start;                    // sector of cluster 2
    uint32_t clusters;                      // clusters 2 .. clusters + 1 exist
    uint32_t fsinfo;                        // FAT32: the FSInfo sector, or 0
    bool fsinfo_stale;                      // its free count has been set to unknown
    uint32_t next_free;                     // where to start looking for a free cluster
    uint64_t next_ino;
    struct fat_node *root;
    char label[12];
    char path[VFS_PATH_MAX + 1];            // where it is mounted
    uint8_t *buf;                           // one sector, for file and directory data
    uint8_t *fat_buf;                       // one sector, for the FAT
};

struct fat_node {
    struct vnode vnode; // vnode.data points back here
    struct fat_fs *fs;
    char name[VFS_NAME_MAX + 1];
    uint8_t short_name[11];
    uint32_t first_cluster; // 0: none (an empty file), or the FAT12/16 root
    bool fixed_root;        // the FAT12/16 root directory, outside the clusters
    struct fat_node *parent;
    uint64_t entry_pos;     // where its short entry is in the parent, in bytes
    unsigned entry_slots;   // entries it takes there, long-name ones included
    // Directories:
    bool loaded;            // children read from the disk
    struct fat_node *children, *last_child, *next;
    // The last cluster found by index, so reading on from there is quick.
    uint32_t hint_index, hint_cluster; // hint_cluster 0: none
};

static struct fat_fs *mounts[MAX_MOUNTS];
static unsigned mount_count;

static struct fat_node *node(struct vnode *v) { return v->data; }

static uint32_t min32(uint64_t a, uint64_t b) { return (uint32_t)(a < b ? a : b); }

static char ascii_upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (ascii_upper(*a) != ascii_upper(*b)) return false;
    return *a == *b;
}

// ---------------------------------------------------------------------------
// The FAT
// ---------------------------------------------------------------------------

static uint32_t end_of_chain(const struct fat_fs *fs) {
    return fs->type == 12 ? 0xfff : fs->type == 16 ? 0xffff : 0x0fffffff;
}

static bool is_end(const struct fat_fs *fs, uint32_t v) {
    return v >= (fs->type == 12 ? 0xff8u : fs->type == 16 ? 0xfff8u : 0x0ffffff8u);
}

static bool valid_cluster(const struct fat_fs *fs, uint32_t c) { return c >= 2 && c < fs->clusters + 2; }

// Read or write `n` bytes of FAT number `copy` at byte `off` (a FAT12 entry
// can straddle two sectors).
static int fat_bytes(struct fat_fs *fs, unsigned copy, uint64_t off, uint8_t *bytes, unsigned n, bool write) {
    uint32_t ss = fs->sector_size;
    uint64_t base = fs->fat_start + (uint64_t)copy * fs->fat_sectors;
    for (unsigned i = 0; i < n;) {
        uint64_t sector = base + (off + i) / ss;
        uint32_t in = (uint32_t)((off + i) % ss);
        unsigned take = min32(n - i, ss - in);
        int err = bcache_read(fs->dev, sector, 1, fs->fat_buf);
        if (err) return err;
        if (write) {
            memcpy(fs->fat_buf + in, bytes + i, take);
            if ((err = bcache_write(fs->dev, sector, 1, fs->fat_buf))) return err;
        } else {
            memcpy(bytes + i, fs->fat_buf + in, take);
        }
        i += take;
    }
    return 0;
}

static uint64_t entry_offset(const struct fat_fs *fs, uint32_t c) {
    return fs->type == 12 ? c + c / 2 : (uint64_t)c * (fs->type / 8);
}

static int fat_get(struct fat_fs *fs, uint32_t c, uint32_t *out) {
    if (!valid_cluster(fs, c)) return -EIO;
    uint8_t b[4] = {0};
    int err = fat_bytes(fs, fs->active_fat, entry_offset(fs, c), b, fs->type == 32 ? 4 : 2, false);
    if (err) return err;
    uint32_t v = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    if (fs->type == 12) v = c & 1 ? v >> 4 : v & 0xfff;
    else if (fs->type == 32) v &= 0x0fffffff;
    *out = v;
    return 0;
}

// The first change to a FAT32 volume marks the FSInfo free count unknown,
// so nothing trusts a count that is no longer right.
static int fsinfo_stale(struct fat_fs *fs) {
    if (fs->type != 32 || fs->fsinfo == 0 || fs->fsinfo_stale) return 0;
    int err = bcache_read(fs->dev, fs->fsinfo, 1, fs->fat_buf);
    if (err) return err;
    memset(fs->fat_buf + 488, 0xff, 4);
    if ((err = bcache_write(fs->dev, fs->fsinfo, 1, fs->fat_buf))) return err;
    fs->fsinfo_stale = true;
    return 0;
}

static int fat_set(struct fat_fs *fs, uint32_t c, uint32_t value) {
    if (!valid_cluster(fs, c)) return -EIO;
    int err = fsinfo_stale(fs);
    if (err) return err;
    unsigned n = fs->type == 32 ? 4 : 2;
    uint64_t off = entry_offset(fs, c);
    for (unsigned copy = 0; copy < fs->fats; copy++) {
        if (!fs->mirror && copy != fs->active_fat) continue;
        uint8_t b[4] = {0};
        if ((err = fat_bytes(fs, copy, off, b, n, false))) return err;
        uint32_t v = b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        if (fs->type == 12) v = c & 1 ? (v & 0x000f) | value << 4 : (v & 0xf000) | (value & 0xfff);
        else if (fs->type == 16) v = value & 0xffff;
        else v = (v & 0xf0000000) | (value & 0x0fffffff); // the top 4 bits are reserved
        for (unsigned i = 0; i < n; i++) b[i] = (uint8_t)(v >> (8 * i));
        if ((err = fat_bytes(fs, copy, off, b, n, true))) return err;
    }
    return 0;
}

// The cluster after `c` in its chain, or 0 at the end.
static int next_cluster(struct fat_fs *fs, uint32_t c, uint32_t *next) {
    uint32_t v;
    int err = fat_get(fs, c, &v);
    if (err) return err;
    if (is_end(fs, v)) v = 0;
    else if (!valid_cluster(fs, v)) return -EIO; // free or bad, in the middle of a chain
    *next = v;
    return 0;
}

// Take a free cluster, mark it the end of a chain, and link it after `prev`
// (if not 0).
static int alloc_cluster(struct fat_fs *fs, uint32_t prev, uint32_t *out) {
    uint32_t c = valid_cluster(fs, fs->next_free) ? fs->next_free : 2;
    for (uint32_t tries = 0; tries < fs->clusters; tries++, c = c + 1 < fs->clusters + 2 ? c + 1 : 2) {
        uint32_t v;
        int err = fat_get(fs, c, &v);
        if (err) return err;
        if (v != 0) continue;
        if ((err = fat_set(fs, c, end_of_chain(fs)))) return err;
        if (prev && (err = fat_set(fs, prev, c))) return err;
        fs->next_free = c + 1;
        *out = c;
        return 0;
    }
    return -ENOSPC;
}

static int free_chain(struct fat_fs *fs, uint32_t c) {
    for (uint32_t n = 0; c && n < fs->clusters; n++) {
        uint32_t next;
        int err = next_cluster(fs, c, &next);
        if (err) return err;
        if ((err = fat_set(fs, c, 0))) return err;
        if (c < fs->next_free) fs->next_free = c;
        c = next;
    }
    return 0;
}

static uint64_t cluster_sector(const struct fat_fs *fs, uint32_t c) {
    return fs->data_start + (uint64_t)(c - 2) * fs->cluster_sectors;
}

// ---------------------------------------------------------------------------
// File and directory data
// ---------------------------------------------------------------------------

// Cluster number `index` of `n`'s chain. -ENOENT past its end.
static int cluster_at(struct fat_node *n, uint32_t index, uint32_t *out) {
    uint32_t c = n->first_cluster, i = 0;
    if (n->hint_cluster && n->hint_index <= index) {
        c = n->hint_cluster;
        i = n->hint_index;
    }
    if (c == 0) return -ENOENT;
    for (; i < index; i++) {
        int err = next_cluster(n->fs, c, &c);
        if (err) return err;
        if (c == 0) return -ENOENT;
    }
    n->hint_index = index;
    n->hint_cluster = c;
    *out = c;
    return 0;
}

// The sector that holds byte `pos` of `n`, and how many sectors from there
// on are in a row on the disk (to the end of its cluster). -ENOENT past the
// end of the chain, or of the fixed root directory.
static int locate(struct fat_node *n, uint64_t pos, uint64_t *sector, uint32_t *run) {
    struct fat_fs *fs = n->fs;
    uint32_t ss = fs->sector_size;
    if (n->fixed_root) {
        if (pos >= (uint64_t)fs->root_entries * DIRENT_SIZE) return -ENOENT;
        *sector = fs->root_start + pos / ss;
        *run = fs->root_sectors - (uint32_t)(pos / ss);
        return 0;
    }
    if (pos / fs->cluster_size > UINT32_MAX) return -ENOENT;
    uint32_t c;
    int err = cluster_at(n, (uint32_t)(pos / fs->cluster_size), &c);
    if (err) return err;
    uint32_t in = (uint32_t)(pos % fs->cluster_size) / ss;
    *sector = cluster_sector(fs, c) + in;
    *run = fs->cluster_sectors - in;
    return 0;
}

// Read or write `size` bytes of `n`'s clusters at `pos`, which must be
// there already. A write with `buf` NULL writes zeroes.
static int data_io(struct fat_node *n, uint64_t pos, uint8_t *buf, size_t size, bool write) {
    struct fat_fs *fs = n->fs;
    uint32_t ss = fs->sector_size;
    while (size) {
        uint64_t sector;
        uint32_t run;
        int err = locate(n, pos, &sector, &run);
        if (err) return err;
        uint32_t in = (uint32_t)(pos % ss);
        size_t done;
        if (in == 0 && size >= ss && buf) {
            uint32_t count = min32(run, size / ss); // whole sectors, straight to or from `buf`
            err = write ? bcache_write(fs->dev, sector, count, buf) : bcache_read(fs->dev, sector, count, buf);
            done = (size_t)count * ss;
        } else {
            done = min32(ss - in, size);
            err = bcache_read(fs->dev, sector, 1, fs->buf);
            if (err == 0 && write) {
                if (buf) memcpy(fs->buf + in, buf, done);
                else memset(fs->buf + in, 0, done);
                err = bcache_write(fs->dev, sector, 1, fs->buf);
            } else if (err == 0) {
                memcpy(buf, fs->buf + in, done);
            }
        }
        if (err) return err;
        pos += done;
        size -= done;
        if (buf) buf += done;
    }
    return 0;
}

static int read_entry(struct fat_node *dir, uint64_t pos, struct dirent *e) {
    return data_io(dir, pos, (uint8_t *)e, sizeof *e, false);
}

static int write_entry(struct fat_node *dir, uint64_t pos, const struct dirent *e) {
    return data_io(dir, pos, (uint8_t *)e, sizeof *e, true);
}

static int zero_cluster(struct fat_fs *fs, uint32_t c) {
    memset(fs->buf, 0, fs->sector_size);
    for (uint32_t i = 0; i < fs->cluster_sectors; i++) {
        int err = bcache_write(fs->dev, cluster_sector(fs, c) + i, 1, fs->buf);
        if (err) return err;
    }
    return 0;
}

// The date and time to stamp on what is written now.
static void now(uint16_t *date, uint16_t *time) {
    int64_t ns;
    struct date d = {.year = 1980, .month = 1, .day = 1};
    if (clock_realtime_ns(&ns)) unix_to_date(ns / (int64_t)NS_PER_SEC, &d);
    if (d.year < 1980) d = (struct date){.year = 1980, .month = 1, .day = 1};
    if (d.year > 2107) d = (struct date){.year = 2107, .month = 12, .day = 31, .hour = 23, .minute = 59, .second = 58};
    *date = (uint16_t)((d.year - 1980) << 9 | d.month << 5 | d.day);
    *time = (uint16_t)(d.hour << 11 | d.minute << 5 | d.second / 2);
}

// Write `n`'s size and first cluster into its directory entry, as modified
// now.
static int update_entry(struct fat_node *n) {
    if (n->parent == NULL) return 0; // the root has no entry
    struct dirent e;
    int err = read_entry(n->parent, n->entry_pos, &e);
    if (err) return err;
    e.cluster_lo = (uint16_t)n->first_cluster;
    e.cluster_hi = n->fs->type == 32 ? (uint16_t)(n->first_cluster >> 16) : 0;
    e.size = n->vnode.type == VNODE_FILE ? (uint32_t)n->vnode.size : 0;
    e.attr |= ATTR_ARCHIVE;
    uint16_t date, time;
    now(&date, &time);
    e.mdate = e.adate = date;
    e.mtime = time;
    return write_entry(n->parent, n->entry_pos, &e);
}

// Make `n`'s chain long enough for `bytes` bytes.
static int ensure_clusters(struct fat_node *n, uint64_t bytes) {
    struct fat_fs *fs = n->fs;
    uint64_t need = (bytes + fs->cluster_size - 1) / fs->cluster_size;
    if (need == 0) return 0;
    uint32_t last;
    uint64_t have;
    int err;
    if (n->first_cluster == 0) {
        if ((err = alloc_cluster(fs, 0, &last))) return err;
        n->first_cluster = last;
        n->hint_index = 0;
        n->hint_cluster = last;
        have = 1;
        if ((err = update_entry(n))) return err; // so the cluster is never lost
    } else {
        have = n->vnode.size ? (n->vnode.size + fs->cluster_size - 1) / fs->cluster_size : 1;
        if ((err = cluster_at(n, (uint32_t)(have - 1), &last))) return err == -ENOENT ? -EIO : err;
    }
    for (; have < need; have++) {
        uint32_t next;
        if ((err = next_cluster(fs, last, &next))) return err; // a chain may be longer than the size says
        if (next == 0 && (err = alloc_cluster(fs, last, &next))) return err;
        last = next;
        n->hint_index = (uint32_t)have;
        n->hint_cluster = last;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// The short name as text: "README.TXT", or "readme.txt" with the case flags.
static void short_to_text(const uint8_t *sn, uint8_t nt_case, char *out) {
    size_t n = 0;
    for (int i = 0; i < 8 && sn[i] != ' '; i++) {
        char c = i == 0 && sn[0] == 0x05 ? (char)0xe5 : (char)sn[i];
        if ((uint8_t)c >= 0x80) c = '?'; // a code page we don't know
        out[n++] = (nt_case & CASE_LOWER_BASE) && c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    if (sn[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && sn[i] != ' '; i++) {
            char c = (uint8_t)sn[i] >= 0x80 ? '?' : (char)sn[i];
            out[n++] = (nt_case & CASE_LOWER_EXT) && c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
        }
    }
    out[n] = '\0';
}

static uint8_t short_checksum(const uint8_t *sn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

// UTF-16 (ending at a 0 or after `n` units) to UTF-8 in `out` (`size`
// bytes with the NUL). False if it doesn't fit or isn't valid.
static bool utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xd800 && cp < 0xdc00) {
            if (i + 1 >= n || s[i + 1] < 0xdc00 || s[i + 1] >= 0xe000) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00);
        } else if (cp >= 0xdc00 && cp < 0xe000) {
            return false;
        }
        uint8_t b[4];
        size_t len;
        if (cp < 0x80) b[0] = (uint8_t)cp, len = 1;
        else if (cp < 0x800) b[0] = (uint8_t)(0xc0 | cp >> 6), b[1] = (uint8_t)(0x80 | (cp & 0x3f)), len = 2;
        else if (cp < 0x10000)
            b[0] = (uint8_t)(0xe0 | cp >> 12), b[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3f)),
            b[2] = (uint8_t)(0x80 | (cp & 0x3f)), len = 3;
        else
            b[0] = (uint8_t)(0xf0 | cp >> 18), b[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3f)),
            b[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3f)), b[3] = (uint8_t)(0x80 | (cp & 0x3f)), len = 4;
        if (o + len >= size) return false;
        memcpy(out + o, b, len);
        o += len;
    }
    if (o == 0) return false;
    out[o] = '\0';
    return true;
}

// UTF-8 to UTF-16: the number of units, or -1 if it isn't valid UTF-8 or
// has more than `max` units.
static int utf8_to_utf16(const char *s, uint16_t *out, int max) {
    int n = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p;) {
        uint32_t cp;
        int extra;
        if (*p < 0x80) cp = *p, extra = 0;
        else if ((*p & 0xe0) == 0xc0) cp = *p & 0x1f, extra = 1;
        else if ((*p & 0xf0) == 0xe0) cp = *p & 0x0f, extra = 2;
        else if ((*p & 0xf8) == 0xf0) cp = *p & 0x07, extra = 3;
        else return -1;
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xc0) != 0x80) return -1;
            cp = cp << 6 | (*p & 0x3f);
        }
        if (cp >= 0xd800 && cp < 0xe000) return -1;
        if (cp >= 0x10000) {
            if (n + 2 > max) return -1;
            out[n++] = (uint16_t)(0xd800 + ((cp - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xdc00 + ((cp - 0x10000) & 0x3ff));
        } else {
            if (n + 1 > max) return -1;
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

static bool short_char_ok(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    for (const char *p = "$%'-_@~`!(){}^#&"; *p; p++)
        if (*p == c) return true;
    return false;
}

static bool has_short_name(struct fat_node *dir, const uint8_t *sn) {
    for (struct fat_node *c = dir->children; c; c = c->next)
        if (memcmp(c->short_name, sn, 11) == 0) return true;
    return false;
}

// Make the short name for a new entry `name` in `dir`: the name itself when
// it is a valid upper-case 8.3 name (no long name needed), else one made
// from it, with a "~1" (~2, ...) tail when something had to be left out
// or it is taken.
static int make_short_name(struct fat_node *dir, const char *name, uint8_t *sn, bool *need_lfn) {
    memset(sn, ' ', 11);
    bool lossy = false;
    const char *start = name;
    while (*start == '.') start++, lossy = true;
    const char *dot = NULL;
    for (const char *p = start; *p; p++)
        if (*p == '.') dot = p;
    size_t base_len = 0, ext_len = 0;
    for (const char *p = start; *p && p != dot; p++) {
        if ((uint8_t)*p >= 0x80 && ((uint8_t)*p & 0xc0) == 0x80) continue; // UTF-8 continuation: one '_' a character
        char c = ascii_upper(*p);
        if (c == ' ' || c == '.') {
            lossy = true;
            continue;
        }
        if (!short_char_ok(c)) c = '_', lossy = true;
        if (base_len == 8) {
            lossy = true;
            break;
        }
        sn[base_len++] = (uint8_t)c;
    }
    if (dot) {
        for (const char *p = dot + 1; *p; p++) {
            if ((uint8_t)*p >= 0x80 && ((uint8_t)*p & 0xc0) == 0x80) continue;
            char c = ascii_upper(*p);
            if (c == ' ') {
                lossy = true;
                continue;
            }
            if (!short_char_ok(c)) c = '_', lossy = true;
            if (ext_len == 3) {
                lossy = true;
                break;
            }
            sn[8 + ext_len++] = (uint8_t)c;
        }
    }
    if (base_len == 0) sn[base_len++] = '_', lossy = true;

    // A long name too, unless the short one says it all.
    char text[13];
    short_to_text(sn, 0, text);
    *need_lfn = lossy || strcmp(text, name) != 0;
    if (!lossy && !has_short_name(dir, sn)) return 0;
    for (unsigned n = 1; n < 1000000; n++) {
        char tail[8];
        int tail_len = ksnprintf(tail, sizeof tail, "~%u", n);
        size_t keep = base_len < 8u - (size_t)tail_len ? base_len : 8u - (size_t)tail_len;
        uint8_t try[11];
        memcpy(try, sn, 11);
        memset(try + keep, ' ', 8 - keep);
        memcpy(try + keep, tail, (size_t)tail_len);
        if (!has_short_name(dir, try)) {
            memcpy(sn, try, 11);
            return 0;
        }
    }
    return -EEXIST;
}

// Whether `name` can be a long name: no control characters, none of
// " * / : < > ? \ |, and not ending in a dot or a space (Windows drops those).
static bool valid_long_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || name[len - 1] == '.' || name[len - 1] == ' ') return false;
    for (const char *p = name; *p; p++) {
        if ((uint8_t)*p < 0x20 || *p == 0x7f) return false;
        for (const char *bad = "\"*/:<>?\\|"; *bad; bad++)
            if (*p == *bad) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

static const struct vnode_ops fat12_ops, fat16_ops, fat32_ops;

static struct fat_node *new_node(struct fat_fs *fs, enum vnode_type type) {
    struct fat_node *n = kcalloc(1, sizeof *n);
    if (n == NULL) return NULL;
    n->vnode = (struct vnode){.type = type, .ino = fs->next_ino++, .ops = fs->ops, .data = n};
    n->fs = fs;
    return n;
}

static void add_child(struct fat_node *dir, struct fat_node *c) {
    c->parent = dir;
    if (dir->last_child) dir->last_child->next = c;
    else dir->children = c;
    dir->last_child = c;
    dir->vnode.size++;
}

static void free_children(struct fat_node *dir) {
    for (struct fat_node *c = dir->children, *next; c; c = next) {
        next = c->next;
        free_children(c);
        kfree(c);
    }
    dir->children = dir->last_child = NULL;
    dir->vnode.size = 0;
    dir->loaded = false;
}

// Read the entries of `dir` from the disk, once.
static int load_dir(struct fat_node *dir) {
    if (dir->loaded) return 0;
    struct fat_fs *fs = dir->fs;
    uint16_t lfn[LFN_MAX_SLOTS * LFN_CHARS];
    unsigned lfn_slots = 0, lfn_next = 0; // lfn_next: the sequence number expected next, 0 when all are in
    uint8_t lfn_sum = 0;
    bool lfn_ok = false;
    int err = 0;
    for (uint64_t pos = 0; pos < DIR_MAX_BYTES; pos += DIRENT_SIZE) {
        struct dirent e;
        err = read_entry(dir, pos, &e);
        if (err == -ENOENT) {
            err = 0;
            break;
        }
        if (err) break;
        if (e.name[0] == 0) break;
        if (e.name[0] == ENTRY_DELETED) {
            lfn_ok = false;
            continue;
        }
        if ((e.attr & 0x3f) == ATTR_LFN) {
            const uint8_t *raw = (const uint8_t *)&e;
            unsigned seq = raw[0] & 0x1f;
            if (raw[0] & LFN_LAST) {
                lfn_ok = seq >= 1 && seq <= LFN_MAX_SLOTS;
                lfn_slots = seq;
                lfn_next = seq;
                lfn_sum = raw[LFN_CHECKSUM_OFFSET];
            } else if (!lfn_ok || seq != lfn_next || raw[LFN_CHECKSUM_OFFSET] != lfn_sum) {
                lfn_ok = false;
            }
            if (lfn_ok) {
                for (int i = 0; i < LFN_CHARS; i++)
                    lfn[(seq - 1) * LFN_CHARS + i] = (uint16_t)(raw[lfn_offsets[i]] | raw[lfn_offsets[i] + 1] << 8);
                lfn_next = seq - 1;
            }
            continue;
        }
        bool had_lfn = lfn_ok && lfn_next == 0 && short_checksum(e.name) == lfn_sum;
        lfn_ok = false;
        if (e.attr & ATTR_VOLUME_ID) continue;
        if (e.name[0] == '.' && (e.name[1] == ' ' || (e.name[1] == '.' && e.name[2] == ' '))) continue;

        struct fat_node *c = new_node(fs, e.attr & ATTR_DIRECTORY ? VNODE_DIR : VNODE_FILE);
        if (c == NULL) {
            err = -ENOMEM;
            break;
        }
        // A long name too long for VFS_NAME_MAX shows as the short one, but
        // its entries still belong to this file.
        if (!had_lfn || !utf16_to_utf8(lfn, lfn_slots * LFN_CHARS, c->name, sizeof c->name)) {
            short_to_text(e.name, e.nt_case, c->name);
        }
        memcpy(c->short_name, e.name, 11);
        c->first_cluster = e.cluster_lo | (fs->type == 32 ? (uint32_t)e.cluster_hi << 16 : 0);
        if (c->vnode.type == VNODE_FILE) c->vnode.size = e.size;
        c->entry_pos = pos;
        c->entry_slots = had_lfn ? lfn_slots + 1 : 1;
        add_child(dir, c);
    }
    if (err) {
        free_children(dir);
        return err;
    }
    dir->loaded = true;
    return 0;
}

static struct fat_node *find_child(struct fat_node *dir, const char *name) {
    for (struct fat_node *c = dir->children; c; c = c->next) {
        if (same_name(c->name, name)) return c;
        char sn[13];
        short_to_text(c->short_name, 0, sn);
        if (same_name(sn, name)) return c;
    }
    return NULL;
}

static int fat_lookup(struct vnode *dirv, const char *name, struct vnode **out) {
    struct fat_node *dir = node(dirv);
    int err = load_dir(dir);
    if (err) return err;
    struct fat_node *c = find_child(dir, name);
    if (c == NULL) return -ENOENT;
    // Read a directory as it is looked up, so stat gives its number of entries.
    if (c->vnode.type == VNODE_DIR && (err = load_dir(c))) return err;
    *out = &c->vnode;
    return 0;
}

static int fat_readdir(struct vnode *dirv, uint64_t index, struct vfs_dirent *out) {
    struct fat_node *dir = node(dirv);
    int err = load_dir(dir);
    if (err) return err;
    struct fat_node *c = dir->children;
    for (; c && index; index--) c = c->next;
    if (c == NULL) return -ENOENT;
    memcpy(out->name, c->name, sizeof out->name);
    out->type = c->vnode.type;
    out->ino = c->vnode.ino;
    return 0;
}

// Find `slots` free entries in a row in `dir`, growing it by a cluster if
// there aren't (the FAT12/16 root can't grow). Their position in bytes.
static int find_slots(struct fat_node *dir, unsigned slots, uint64_t *out) {
    struct fat_fs *fs = dir->fs;
    uint64_t start = 0;
    unsigned run = 0;
    for (uint64_t pos = 0; pos < DIR_MAX_BYTES; pos += DIRENT_SIZE) {
        struct dirent e;
        int err = read_entry(dir, pos, &e);
        if (err == -ENOENT && !dir->fixed_root) {
            uint32_t last, c;
            if ((err = cluster_at(dir, (uint32_t)(pos / fs->cluster_size) - 1, &last))) return err;
            if ((err = alloc_cluster(fs, last, &c))) return err;
            if ((err = zero_cluster(fs, c))) return err;
            err = read_entry(dir, pos, &e);
        }
        if (err == -ENOENT) return -ENOSPC;
        if (err) return err;
        if (e.name[0] != 0 && e.name[0] != ENTRY_DELETED) {
            run = 0;
            continue;
        }
        if (run++ == 0) start = pos;
        if (run == slots) {
            *out = start;
            return 0;
        }
    }
    return -ENOSPC;
}

static int fat_create(struct vnode *dirv, const char *name, enum vnode_type type, struct vnode **out) {
    struct fat_node *dir = node(dirv);
    struct fat_fs *fs = dir->fs;
    if (type == VNODE_CHARDEV) return -EPERM;
    if (!valid_long_name(name)) return -EINVAL;
    int err = load_dir(dir);
    if (err) return err;
    uint16_t lfn[LFN_MAX];
    int lfn_len = utf8_to_utf16(name, lfn, LFN_MAX);
    if (lfn_len <= 0) return -EINVAL;
    uint8_t sn[11];
    bool need_lfn;
    if ((err = make_short_name(dir, name, sn, &need_lfn))) return err;
    unsigned slots = need_lfn ? (unsigned)(lfn_len + LFN_CHARS - 1) / LFN_CHARS + 1 : 1;
    uint64_t pos;
    if ((err = find_slots(dir, slots, &pos))) return err;

    struct fat_node *n = new_node(fs, type);
    if (n == NULL) return -ENOSPC;
    uint16_t date, time;
    now(&date, &time);
    // A directory gets a cluster with "." and "..".
    if (type == VNODE_DIR) {
        uint32_t c;
        if ((err = alloc_cluster(fs, 0, &c))) goto fail;
        n->first_cluster = c;
        if ((err = zero_cluster(fs, c))) goto fail;
        struct dirent dots[2] = {0};
        uint32_t up = dir->parent ? dir->first_cluster : 0; // ".." to the root is cluster 0
        for (int i = 0; i < 2; i++) {
            memset(dots[i].name, ' ', 11);
            dots[i].name[0] = '.';
            if (i) dots[i].name[1] = '.';
            uint32_t to = i ? up : c;
            dots[i].attr = ATTR_DIRECTORY;
            dots[i].cluster_lo = (uint16_t)to;
            dots[i].cluster_hi = fs->type == 32 ? (uint16_t)(to >> 16) : 0;
            dots[i].cdate = dots[i].mdate = dots[i].adate = date;
            dots[i].ctime = dots[i].mtime = time;
        }
        memcpy(fs->buf, dots, sizeof dots);
        memset(fs->buf + sizeof dots, 0, fs->sector_size - sizeof dots);
        if ((err = bcache_write(fs->dev, cluster_sector(fs, c), 1, fs->buf))) goto fail;
        n->loaded = true;
    }

    // The long-name entries, last part first, then the short entry.
    uint8_t sum = short_checksum(sn);
    for (unsigned k = 0; k + 1 < slots; k++) {
        unsigned seq = slots - 1 - k;
        uint8_t raw[DIRENT_SIZE] = {0};
        raw[0] = (uint8_t)(seq | (k == 0 ? LFN_LAST : 0));
        raw[11] = ATTR_LFN;
        raw[LFN_CHECKSUM_OFFSET] = sum;
        for (int i = 0; i < LFN_CHARS; i++) {
            int at = (int)(seq - 1) * LFN_CHARS + i;
            uint16_t ch = at < lfn_len ? lfn[at] : at == lfn_len ? 0 : 0xffff;
            raw[lfn_offsets[i]] = (uint8_t)ch;
            raw[lfn_offsets[i] + 1] = (uint8_t)(ch >> 8);
        }
        if ((err = write_entry(dir, pos + (uint64_t)k * DIRENT_SIZE, (const struct dirent *)raw))) goto fail;
    }
    struct dirent e = {0};
    memcpy(e.name, sn, 11);
    e.attr = type == VNODE_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    e.cdate = e.mdate = e.adate = date;
    e.ctime = e.mtime = time;
    e.cluster_lo = (uint16_t)n->first_cluster;
    e.cluster_hi = fs->type == 32 ? (uint16_t)(n->first_cluster >> 16) : 0;
    uint64_t entry_pos = pos + (uint64_t)(slots - 1) * DIRENT_SIZE;
    if ((err = write_entry(dir, entry_pos, &e))) goto fail;

    memcpy(n->name, name, strlen(name) + 1);
    memcpy(n->short_name, sn, 11);
    n->entry_pos = entry_pos;
    n->entry_slots = slots;
    add_child(dir, n);
    *out = &n->vnode;
    return 0;
fail:
    if (n->first_cluster) free_chain(fs, n->first_cluster);
    kfree(n);
    return err;
}

static int fat_unlink(struct vnode *dirv, const char *name) {
    struct fat_node *dir = node(dirv);
    struct fat_fs *fs = dir->fs;
    int err = load_dir(dir);
    if (err) return err;
    struct fat_node *c = find_child(dir, name);
    if (c == NULL) return -ENOENT;
    if (c->vnode.type == VNODE_DIR) {
        if ((err = load_dir(c))) return err;
        if (c->children) return -ENOTEMPTY;
    }
    // Mark its entries deleted, then free its clusters.
    for (unsigned i = 0; i < c->entry_slots; i++) {
        uint64_t pos = c->entry_pos - (uint64_t)(c->entry_slots - 1 - i) * DIRENT_SIZE;
        struct dirent e;
        if ((err = read_entry(dir, pos, &e))) return err;
        e.name[0] = ENTRY_DELETED;
        if ((err = write_entry(dir, pos, &e))) return err;
    }
    struct fat_node *prev = NULL;
    for (struct fat_node *p = dir->children; p != c; p = p->next) prev = p;
    if (prev) prev->next = c->next;
    else dir->children = c->next;
    if (dir->last_child == c) dir->last_child = prev;
    dirv->size--;
    uint32_t first = c->first_cluster;
    kfree(c);
    return first ? free_chain(fs, first) : 0;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static int64_t fat_read(struct vnode *v, uint64_t offset, void *buf, size_t size) {
    if (offset >= v->size) return 0;
    if (size > v->size - offset) size = (size_t)(v->size - offset);
    int err = data_io(node(v), offset, buf, size, false);
    if (err) return err == -ENOENT ? -EIO : err; // the chain is shorter than the size
    return (int64_t)size;
}

static int fat_truncate(struct vnode *v, uint64_t size);

static int64_t fat_write(struct vnode *v, uint64_t offset, const void *buf, size_t size) {
    struct fat_node *n = node(v);
    if (size == 0) return 0;
    if (offset > FILE_MAX || size > FILE_MAX - offset) return -EFBIG;
    uint64_t end = offset + size;
    int err = 0;
    if (end > v->size) err = ensure_clusters(n, end);
    if (err == 0 && offset > v->size) err = data_io(n, v->size, NULL, offset - v->size, true); // the gap reads as zeroes
    if (err == 0) err = data_io(n, offset, (uint8_t *)buf, size, true);
    if (err) {
        fat_truncate(v, v->size); // give back the clusters added for it
        return err;
    }
    if (end > v->size) v->size = end;
    err = update_entry(n);
    return err ? err : (int64_t)size;
}

static int fat_truncate(struct vnode *v, uint64_t size) {
    struct fat_node *n = node(v);
    struct fat_fs *fs = n->fs;
    if (size > FILE_MAX) return -EFBIG;
    int err;
    if (size > v->size) {
        if ((err = ensure_clusters(n, size)) || (err = data_io(n, v->size, NULL, size - v->size, true))) return err;
        v->size = size;
        return update_entry(n);
    }
    uint32_t release = 0; // the first cluster no longer needed
    uint64_t keep = (size + fs->cluster_size - 1) / fs->cluster_size;
    if (n->first_cluster && keep == 0) {
        release = n->first_cluster;
        n->first_cluster = 0;
    } else if (n->first_cluster) {
        uint32_t last;
        if ((err = cluster_at(n, (uint32_t)(keep - 1), &last)) || (err = next_cluster(fs, last, &release))) return err;
        if (release && (err = fat_set(fs, last, end_of_chain(fs)))) return err;
    }
    n->hint_cluster = 0;
    v->size = size;
    if ((err = update_entry(n))) return err; // the entry first: a failure then loses clusters, never shares them
    return release ? free_chain(fs, release) : 0;
}

#define FAT_OPS(name, fs)                                                                                         \
    static const struct vnode_ops name = {                                                                        \
        .fs_name = fs,                                                                                            \
        .read = fat_read,                                                                                         \
        .write = fat_write,                                                                                       \
        .truncate = fat_truncate,                                                                                 \
        .lookup = fat_lookup,                                                                                     \
        .create = fat_create,                                                                                     \
        .readdir = fat_readdir,                                                                                   \
        .unlink = fat_unlink,                                                                                     \
    }

FAT_OPS(fat12_ops, "fat12");
FAT_OPS(fat16_ops, "fat16");
FAT_OPS(fat32_ops, "fat32");

// ---------------------------------------------------------------------------
// Mounting
// ---------------------------------------------------------------------------

static bool power_of_two(uint32_t x) { return x && !(x & (x - 1)); }

// Read the boot sector of `dev` and set up `fs`. -EINVAL if it isn't a FAT
// volume; another error with the reason in `why` if it is one we can't use.
static int read_bpb(struct block_device *dev, struct fat_fs *fs, const char **why) {
    uint32_t ss = dev->sector_size;
    if (ss < 512 || ss > 4096 || dev->sector_count < 8) return -EINVAL;
    int err = bcache_read(dev, 0, 1, fs->buf);
    if (err) {
        *why = "the boot sector could not be read";
        return err;
    }
    const uint8_t *s = fs->buf;
    struct bpb b;
    struct bpb32 b32;
    memcpy(&b, s, sizeof b);
    memcpy(&b32, s + sizeof b, sizeof b32);
    if ((s[0] != 0xeb && s[0] != 0xe9) || s[510] != 0x55 || s[511] != 0xaa) return -EINVAL;
    if (!power_of_two(b.bytes_per_sector) || b.bytes_per_sector < 512 || b.bytes_per_sector > 4096 ||
        !power_of_two(b.sectors_per_cluster) || b.reserved_sectors == 0 || b.fats == 0)
        return -EINVAL;
    uint32_t total = b.total16 ? b.total16 : b.total32;
    uint32_t fat_size = b.fat_size16 ? b.fat_size16 : b32.fat_size32;
    if (total == 0 || fat_size == 0) return -EINVAL;
    if (b.bytes_per_sector != ss) {
        *why = "its sectors are not the disk's size";
        return -ENODEV;
    }
    if (total > dev->sector_count) {
        *why = "it says it is bigger than the partition";
        return -ENODEV;
    }
    fs->dev = dev;
    fs->sector_size = ss;
    fs->cluster_sectors = b.sectors_per_cluster;
    fs->cluster_size = ss * b.sectors_per_cluster;
    fs->fat_start = b.reserved_sectors;
    fs->fat_sectors = fat_size;
    fs->fats = b.fats;
    fs->root_entries = b.root_entries;
    fs->root_start = fs->fat_start + fs->fats * fat_size;
    fs->root_sectors = (b.root_entries * DIRENT_SIZE + ss - 1) / ss;
    fs->data_start = fs->root_start + fs->root_sectors;
    if ((uint64_t)fs->fat_start + (uint64_t)fs->fats * fat_size + fs->root_sectors >= total) return -EINVAL;
    fs->clusters = (total - fs->data_start) / fs->cluster_sectors;
    if (fs->clusters == 0) return -EINVAL;
    fs->type = fs->clusters < 4085 ? 12 : fs->clusters < 65525 ? 16 : 32;
    if ((fs->type == 32) != (b.fat_size16 == 0 && b.root_entries == 0)) {
        *why = "its BPB doesn't match its FAT type";
        return -ENODEV;
    }
    if (((uint64_t)fs->clusters + 2) * (fs->type == 12 ? 3 : fs->type / 4) > (uint64_t)fat_size * ss * 2) {
        *why = "its FAT is too small for its clusters";
        return -ENODEV;
    }
    fs->ops = fs->type == 12 ? &fat12_ops : fs->type == 16 ? &fat16_ops : &fat32_ops;
    fs->active_fat = 0;
    fs->mirror = true;
    struct ebr ebr;
    if (fs->type == 32) {
        if (b32.ext_flags & 0x80) {
            fs->mirror = false;
            fs->active_fat = b32.ext_flags & 0x0f;
            if (fs->active_fat >= fs->fats) {
                *why = "its active FAT doesn't exist";
                return -ENODEV;
            }
        }
        if (!valid_cluster(fs, b32.root_cluster)) {
            *why = "its root directory cluster doesn't exist";
            return -ENODEV;
        }
        fs->next_free = 2;
        if (b32.fsinfo && b32.fsinfo < fs->fat_start && bcache_read(dev, b32.fsinfo, 1, fs->fat_buf) == 0) {
            const uint8_t *f = fs->fat_buf;
            uint32_t lead, mid, next;
            memcpy(&lead, f, 4);
            memcpy(&mid, f + 484, 4);
            memcpy(&next, f + 492, 4);
            if (lead == 0x41615252 && mid == 0x61417272) {
                fs->fsinfo = b32.fsinfo;
                if (valid_cluster(fs, next)) fs->next_free = next;
            }
        }
        memcpy(&ebr, s + sizeof b + sizeof b32, sizeof ebr);
    } else {
        memcpy(&ebr, s + sizeof b, sizeof ebr);
        fs->next_free = 2;
    }
    fs->label[0] = '\0';
    if (ebr.signature == 0x29) {
        memcpy(fs->label, ebr.label, 11);
        int n = 11;
        while (n > 0 && fs->label[n - 1] == ' ') n--;
        fs->label[n] = '\0';
        if (strcmp(fs->label, "NO NAME") == 0) fs->label[0] = '\0';
    }

    struct fat_node *root = new_node(fs, VNODE_DIR);
    if (root == NULL) {
        *why = "no memory";
        return -ENOMEM;
    }
    if (fs->type == 32) root->first_cluster = b32.root_cluster;
    else root->fixed_root = true;
    fs->root = root;
    if ((err = load_dir(root))) {
        *why = "its root directory could not be read";
        kfree(root);
        return err;
    }
    return 0;
}

// Whether some partition is on `disk`.
static bool has_partitions(const struct block_device *disk) {
    for (size_t i = 0; i < block_device_count(); i++)
        if (block_device(i)->parent == disk) return true;
    return false;
}

static void free_fs(struct fat_fs *fs) {
    kfree(fs->buf);
    kfree(fs->fat_buf);
    kfree(fs);
}

bool fat_mount_all(char *line, size_t size) {
    line[0] = '\0';
    char bad[96] = "";
    size_t used = 0;
    for (size_t i = 0; i < block_device_count() && mount_count < MAX_MOUNTS; i++) {
        struct block_device *dev = block_device(i);
        if (!dev->parent && has_partitions(dev)) continue;
        struct fat_fs *fs = kcalloc(1, sizeof *fs);
        if (fs) fs->buf = kmalloc(dev->sector_size), fs->fat_buf = kmalloc(dev->sector_size);
        if (fs == NULL || fs->buf == NULL || fs->fat_buf == NULL) {
            if (fs) free_fs(fs);
            ksnprintf(line, size, "FAT: no memory");
            return false;
        }
        fs->next_ino = 1;
        const char *why = "";
        int err = read_bpb(dev, fs, &why);
        if (err == 0) {
            ksnprintf(fs->path, sizeof fs->path, "/mnt/%s", dev->name);
            err = vfs_mkdir("/mnt");
            if (err == 0 || err == -EEXIST) err = vfs_mkdir(fs->path);
            if (err == 0 || err == -EEXIST) err = vfs_mount(fs->path, &fs->root->vnode);
            if (err) why = strerror(err);
            if (err) free_children(fs->root), kfree(fs->root);
        }
        if (err) {
            if (err != -EINVAL && bad[0] == '\0') ksnprintf(bad, sizeof bad, "FAT: %s: %s", dev->name, why);
            free_fs(fs);
            continue;
        }
        mounts[mount_count++] = fs;
        if (used < size)
            used += ksnprintf(line + used, size - used, "%sFAT%u on %s", used ? ", " : "", fs->type, fs->path);
    }
    if (bad[0]) ksnprintf(line, size, "%s", bad);
    return bad[0] == '\0';
}

SHELL_COMMAND(mount, "", "list the FAT filesystems mounted from disks");
static void cmd_mount(int argc, char **argv) {
    if (mount_count == 0) kprintf("mount: no FAT filesystems found on the disks\n");
    for (unsigned i = 0; i < mount_count; i++) {
        struct fat_fs *fs = mounts[i];
        kprintf("%s on %s type fat%u (%u clusters of %u bytes%s%s)\n", fs->dev->name, fs->path, fs->type,
                fs->clusters, fs->cluster_size, fs->label[0] ? ", label " : "", fs->label);
    }
}
