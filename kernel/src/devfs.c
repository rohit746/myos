// devfs: the device files in /dev. See vfs.h.

#include <errno.h>
#include <kprintf.h>
#include <string.h>
#include <vfs.h>

static int64_t null_read(struct vnode *v, uint64_t offset, void *buf, size_t size) { return 0; }

static int64_t discard(struct vnode *v, uint64_t offset, const void *buf, size_t size) { return (int64_t)size; }

static int64_t zero_read(struct vnode *v, uint64_t offset, void *buf, size_t size) {
    memset(buf, 0, size);
    return (int64_t)size;
}

static int64_t console_write(struct vnode *v, uint64_t offset, const void *buf, size_t size) {
    kwrite(buf, size);
    return (int64_t)size;
}

static const struct vnode_ops console_ops = {.fs_name = "devfs", .read = null_read, .write = console_write};
static const struct vnode_ops null_ops = {.fs_name = "devfs", .read = null_read, .write = discard};
static const struct vnode_ops zero_ops = {.fs_name = "devfs", .read = zero_read, .write = discard};

static struct device {
    const char *name;
    struct vnode vnode;
} devices[] = {
    {"console", {.type = VNODE_CHARDEV, .ino = 2, .ops = &console_ops}},
    {"null", {.type = VNODE_CHARDEV, .ino = 3, .ops = &null_ops}},
    {"zero", {.type = VNODE_CHARDEV, .ino = 4, .ops = &zero_ops}},
};

#define DEVICES (sizeof devices / sizeof devices[0])

static int dir_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    for (size_t i = 0; i < DEVICES; i++) {
        if (strcmp(devices[i].name, name) == 0) {
            *out = &devices[i].vnode;
            return 0;
        }
    }
    return -ENOENT;
}

static int dir_readdir(struct vnode *dir, uint64_t index, struct vfs_dirent *out) {
    if (index >= DEVICES) return -ENOENT;
    size_t len = strlen(devices[index].name);
    memcpy(out->name, devices[index].name, len + 1);
    out->type = devices[index].vnode.type;
    out->ino = devices[index].vnode.ino;
    return 0;
}

static const struct vnode_ops dir_ops = {.fs_name = "devfs", .lookup = dir_lookup, .readdir = dir_readdir};

static struct vnode root = {.type = VNODE_DIR, .ino = 1, .size = DEVICES, .ops = &dir_ops};

struct vnode *devfs_root(void) { return &root; }
