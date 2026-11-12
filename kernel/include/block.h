#pragma once
// Block devices: disks that read and write whole sectors. Drivers (AHCI
// now; NVMe and USB storage later) fill in a struct block_device and
// register it; everything else reads and writes through block_read() and
// block_write(), whatever the disk is attached to.
//
// Buffers are ordinary kernel memory (heap, stack, HHDM) of any alignment:
// a driver copies through its own DMA memory where it has to.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct block_device;

struct block_ops {
    // Transfer `count` sectors starting at `lba`. The range has been checked
    // against the disk's size. 0, or a negative errno. May sleep.
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
};

struct block_device {
    char name[16];      // "sda"
    char model[41];     // what the disk calls itself: "Samsung SSD 870 EVO 1TB"
    char where[48];     // what it is attached to: "AHCI 00:1f.2 port 0"
    uint32_t sector_size;
    uint64_t sector_count;
    const struct block_ops *ops;
    void *priv; // the driver's
};

// Add a disk. Its name must be unique. The device must stay in memory.
void block_register(struct block_device *dev);

// The disks, in the order they were registered.
size_t block_device_count(void);
struct block_device *block_device(size_t i);

// The disk called `name` ("sda"), or NULL.
struct block_device *block_find(const char *name);

// Read or write `count` sectors at `lba`: 0, or a negative errno (-EINVAL
// for a range past the end of the disk or a count of 0, -EIO when the disk
// reports an error or doesn't answer). May sleep.
int block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
int block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);

// A disk whose first sector starts with this text is a scratch disk the
// boot self-test may write to (`make test` gives QEMU one). Sector s, for
// s >= 1, holds the 32-bit little-endian words s * 128 + i, i = 0..127,
// until the self-test overwrites BLOCK_TEST_WRITE_COUNT sectors at
// BLOCK_TEST_WRITE_LBA with their bitwise NOT.
#define BLOCK_TEST_MAGIC       "myos test disk: the boot self-test writes to it\n"
#define BLOCK_TEST_READ_COUNT  300 // sectors 1-300, read in one call
#define BLOCK_TEST_WRITE_LBA   1000
#define BLOCK_TEST_WRITE_COUNT 8

// Boot self-test: list the disks, and on a scratch disk (BLOCK_TEST_MAGIC)
// read and check a run of sectors that takes several commands, write some,
// read them back and check their neighbours are untouched. True with a
// summary in `line` (empty when there is nothing to report beyond the
// list); false with the reason.
bool block_self_test(char *line, size_t size);
