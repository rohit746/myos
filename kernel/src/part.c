// Partition tables: GPT and MBR. Each partition becomes a block device
// whose reads and writes go to a range of its disk. See block.h.
//
// MBR: sector 0 ends in 55 aa and holds four entries (type, first sector,
// sector count). An extended partition (type 05, 0f or 85) holds a chain
// of EBRs, each describing one logical partition and where the next EBR
// is; logical partitions are numbered from 5.
//
// GPT: an MBR with one entry of type ee (the "protective MBR") says the
// disk has a GPT. Sector 1 holds its header ("EFI PART", CRC-32 checked),
// which says where the array of partition entries is (also CRC-32
// checked); each entry has a type GUID (zero: unused), first and last
// sector, and a name in UTF-16.

#include <block.h>
#include <crc32.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <string.h>

#define MBR_SIGNATURE_OFFSET 510
#define MBR_ENTRIES_OFFSET   446
#define MBR_TYPE_GPT         0xee
#define MAX_LOGICAL          64  // EBRs followed in one extended partition
#define GPT_MAX_ENTRIES      1024

struct mbr_entry {
    uint8_t status; // 0x80: bootable
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba;
    uint32_t sectors;
} __attribute__((packed));

struct gpt_header {
    char signature[8]; // "EFI PART"
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t my_lba, alternate_lba;
    uint64_t first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t type[16];
    uint8_t unique[16];
    uint64_t first_lba, last_lba; // inclusive
    uint64_t attributes;
    uint16_t name[36]; // UTF-16LE
} __attribute__((packed));

static int part_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    return block_read(dev->parent, dev->start + lba, count, buf);
}

static int part_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    return block_write(dev->parent, dev->start + lba, count, buf);
}

static const struct block_ops part_ops = {.read = part_read, .write = part_write};

// Register partition `number` of `disk`: `count` sectors from `first`.
// `scheme` is "MBR" or "GPT"; `what` describes it ("EFI system").
static bool add_partition(struct block_device *disk, unsigned number, uint64_t first, uint64_t count,
                          const char *scheme, const char *what) {
    if (count == 0 || first >= disk->sector_count || count > disk->sector_count - first) {
        kprintf("  %s: partition %u (sectors %lu+%lu) goes past the end of the disk, left out\n", disk->name, number,
                first, count);
        return false;
    }
    struct block_device *p = kcalloc(1, sizeof *p);
    if (p == NULL) return false;
    size_t len = strlen(disk->name);
    bool digit = len && disk->name[len - 1] >= '0' && disk->name[len - 1] <= '9';
    ksnprintf(p->name, sizeof p->name, "%s%s%u", disk->name, digit ? "p" : "", number);
    ksnprintf(p->model, sizeof p->model, "%s", what);
    ksnprintf(p->where, sizeof p->where, "%s partition %u of %s", scheme, number, disk->name);
    p->sector_size = disk->sector_size;
    p->sector_count = count;
    p->ops = &part_ops;
    p->parent = disk;
    p->start = first;
    block_register(p);
    return true;
}

// ---------------------------------------------------------------------------
// MBR
// ---------------------------------------------------------------------------

static const char *mbr_type_name(uint8_t type, char *buf, size_t size) {
    switch (type) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0e: return "FAT16";
    case 0x07: return "NTFS/exFAT";
    case 0x0b: case 0x0c: return "FAT32";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xef: return "EFI system";
    }
    ksnprintf(buf, size, "MBR type %02x", type);
    return buf;
}

static bool is_extended(uint8_t type) { return type == 0x05 || type == 0x0f || type == 0x85; }

// Whether sector 0 holds an MBR partition table: the signature, and four
// entries that make sense, at least one of them used. A FAT boot sector on
// a disk without partitions has the signature too, but no valid entries.
static bool valid_mbr(const uint8_t *sector) {
    if (sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xaa) return false;
    bool used = false;
    for (int i = 0; i < 4; i++) {
        struct mbr_entry e;
        memcpy(&e, sector + MBR_ENTRIES_OFFSET + i * sizeof e, sizeof e);
        if (e.status != 0 && e.status != 0x80) return false;
        if (e.type == 0) continue;
        if (e.lba == 0 || e.sectors == 0) return false;
        used = true;
    }
    return used;
}

// The logical partitions in the extended partition at `ext` (sectors),
// numbered from 5. Returns how many were added.
static unsigned scan_logical(struct block_device *disk, uint64_t ext, uint64_t ext_sectors, uint8_t *sector) {
    unsigned added = 0, number = 5;
    uint64_t ebr = ext;
    for (int n = 0; n < MAX_LOGICAL; n++) {
        if (block_read(disk, ebr, 1, sector) != 0) break;
        if (sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xaa) break;
        struct mbr_entry part, next;
        memcpy(&part, sector + MBR_ENTRIES_OFFSET, sizeof part);
        memcpy(&next, sector + MBR_ENTRIES_OFFSET + sizeof part, sizeof next);
        if (part.type != 0 && part.sectors != 0) {
            char buf[16];
            if (add_partition(disk, number, ebr + part.lba, part.sectors, "MBR",
                              mbr_type_name(part.type, buf, sizeof buf)))
                added++;
            number++;
        }
        // The next EBR is relative to the start of the extended partition.
        if (!is_extended(next.type) || next.lba == 0 || next.lba >= ext_sectors) break;
        ebr = ext + next.lba;
    }
    return added;
}

static unsigned scan_mbr(struct block_device *disk, uint8_t *sector) {
    struct mbr_entry e[4];
    memcpy(e, sector + MBR_ENTRIES_OFFSET, sizeof e);
    unsigned added = 0;
    for (int i = 0; i < 4; i++) {
        if (e[i].type == 0) continue;
        if (is_extended(e[i].type)) {
            added += scan_logical(disk, e[i].lba, e[i].sectors, sector);
            continue;
        }
        char buf[16];
        if (add_partition(disk, i + 1, e[i].lba, e[i].sectors, "MBR", mbr_type_name(e[i].type, buf, sizeof buf)))
            added++;
    }
    return added;
}

// ---------------------------------------------------------------------------
// GPT
// ---------------------------------------------------------------------------

// "C12A7328-F81F-11D2-BA4B-00A0C93EC93B": the first three fields are
// little-endian on disk, the last two big-endian.
static void format_guid(const uint8_t *g, char *out) {
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    static const char hex[] = "0123456789ABCDEF";
    char *o = out;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *o++ = '-';
        *o++ = hex[g[order[i]] >> 4];
        *o++ = hex[g[order[i]] & 15];
    }
    *o = '\0';
}

static const struct {
    const char *guid, *name;
} gpt_types[] = {
    {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system"},
    {"21686148-6449-6E6F-744E-656564454649", "BIOS boot"},
    {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data"},
    {"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved"},
    {"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows recovery"},
    {"0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem"},
    {"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap"},
    {"4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux root (x86-64)"},
    {"7C3457EF-0000-11AA-AA11-00306543ECAC", "Apple APFS"},
    {"48465300-0000-11AA-AA11-00306543ECAC", "Apple HFS+"},
};

static bool zero_guid(const uint8_t *g) {
    for (int i = 0; i < 16; i++)
        if (g[i]) return false;
    return true;
}

// What to call a GPT partition: its name if it has one (non-ASCII
// characters as '?'), else what its type is.
static void gpt_describe(const struct gpt_entry *e, char *out, size_t size) {
    size_t n = 0;
    for (int i = 0; i < 36 && e->name[i] && n + 1 < size; i++) out[n++] = e->name[i] < 0x80 ? (char)e->name[i] : '?';
    out[n] = '\0';
    if (n) return;
    char guid[37];
    format_guid(e->type, guid);
    for (size_t i = 0; i < sizeof gpt_types / sizeof gpt_types[0]; i++) {
        if (strcmp(gpt_types[i].guid, guid) == 0) {
            ksnprintf(out, size, "%s", gpt_types[i].name);
            return;
        }
    }
    ksnprintf(out, size, "type %s", guid);
}

// The GPT whose header is in `sector` (sector 1). Returns the partitions
// added, or a negative errno with the reason in `why`.
static int scan_gpt(struct block_device *disk, uint8_t *sector, const char **why) {
    uint32_t ss = disk->sector_size;
    struct gpt_header h;
    memcpy(&h, sector, sizeof h);
    if (memcmp(h.signature, "EFI PART", 8) != 0) {
        *why = "the protective MBR says GPT, but sector 1 has no GPT header";
        return -EINVAL;
    }
    if (h.header_size < sizeof h || h.header_size > ss) {
        *why = "the GPT header has a bad size";
        return -EINVAL;
    }
    uint32_t want = h.header_crc;
    memset(sector + offsetof(struct gpt_header, header_crc), 0, 4);
    if (crc32(0, sector, h.header_size) != want) {
        *why = "the GPT header's CRC is wrong";
        return -EINVAL;
    }
    if (h.entry_size < sizeof(struct gpt_entry) || h.entry_size % 8 || h.entry_size > 4096 || h.entry_count > GPT_MAX_ENTRIES ||
        h.entries_lba < 2 || h.entries_lba >= disk->sector_count) {
        *why = "the GPT header describes its partition entries wrongly";
        return -EINVAL;
    }
    uint64_t bytes = (uint64_t)h.entry_count * h.entry_size;
    uint32_t sectors = (uint32_t)((bytes + ss - 1) / ss);
    if (sectors == 0) return 0;
    if (sectors > disk->sector_count - h.entries_lba) {
        *why = "the GPT's partition entries go past the end of the disk";
        return -EINVAL;
    }
    uint8_t *entries = kmalloc((uint64_t)sectors * ss);
    if (entries == NULL) {
        *why = "no memory";
        return -ENOMEM;
    }
    int added = 0;
    int err = block_read(disk, h.entries_lba, sectors, entries);
    if (err) {
        *why = "the GPT's partition entries could not be read";
    } else if (crc32(0, entries, bytes) != h.entries_crc) {
        *why = "the CRC of the GPT's partition entries is wrong";
        err = -EINVAL;
    } else {
        for (uint32_t i = 0; i < h.entry_count; i++) {
            struct gpt_entry e;
            memcpy(&e, entries + (uint64_t)i * h.entry_size, sizeof e);
            if (zero_guid(e.type)) continue;
            if (e.last_lba < e.first_lba) continue;
            char what[sizeof ((struct block_device *)0)->model];
            gpt_describe(&e, what, sizeof what);
            if (add_partition(disk, i + 1, e.first_lba, e.last_lba - e.first_lba + 1, "GPT", what)) added++;
        }
    }
    kfree(entries);
    return err ? err : added;
}

// ---------------------------------------------------------------------------

bool partition_scan(char *line, size_t size) {
    line[0] = '\0';
    size_t used = 0;
    char bad[96] = ""; // the first broken table
    size_t disks = block_device_count(); // the partitions added go after these
    for (size_t i = 0; i < disks; i++) {
        struct block_device *disk = block_device(i);
        if (disk->parent || disk->sector_size < 512 || disk->sector_count < 2) continue;
        uint8_t *sector = kmalloc(disk->sector_size);
        if (sector == NULL) return false;
        const char *scheme = NULL, *why = NULL;
        int found = 0;
        if (block_read(disk, 0, 1, sector) == 0 && valid_mbr(sector)) {
            bool gpt = false;
            for (int k = 0; k < 4; k++)
                if (sector[MBR_ENTRIES_OFFSET + k * 16 + 4] == MBR_TYPE_GPT) gpt = true;
            if (gpt) {
                scheme = "GPT";
                if (block_read(disk, 1, 1, sector) == 0) found = scan_gpt(disk, sector, &why);
                else why = "sector 1 could not be read", found = -EIO;
            } else {
                scheme = "MBR";
                found = (int)scan_mbr(disk, sector);
            }
        }
        kfree(sector);
        if (scheme == NULL) continue;
        if (found < 0) {
            if (bad[0] == '\0') ksnprintf(bad, sizeof bad, "partitions: %s: %s", disk->name, why);
            continue;
        }
        if (used < size)
            used += ksnprintf(line + used, size - used, "%s%s %s (%d)", used ? ", " : "partitions: ", disk->name,
                              scheme, found);
    }
    if (bad[0]) ksnprintf(line, size, "%s", bad);
    return bad[0] == '\0';
}
