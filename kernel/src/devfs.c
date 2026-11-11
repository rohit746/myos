// devfs: the device files in /dev. See vfs.h.

#include <errno.h>
#include <keyboard.h>
#include <kprintf.h>
#include <sched.h>
#include <string.h>
#include <sync.h>
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

// Console input: bytes from keys not read yet (an escape sequence can be
// longer than what a read asks for).
static struct spinlock input_lock = SPINLOCK_INIT("console input");
static char pending[8];
static size_t pending_pos, pending_len;

// The bytes a terminal sends for a key press (none for a release, or a key
// that types nothing).
static size_t key_bytes(const struct key_event *ev, char *out) {
    if (!ev->pressed) return 0;
    const char *seq = NULL;
    switch (ev->key) {
    case KEY_UP: seq = "\033[A"; break;
    case KEY_DOWN: seq = "\033[B"; break;
    case KEY_RIGHT: seq = "\033[C"; break;
    case KEY_LEFT: seq = "\033[D"; break;
    case KEY_HOME: seq = "\033[H"; break;
    case KEY_END: seq = "\033[F"; break;
    case KEY_DELETE: seq = "\033[3~"; break;
    }
    if (seq) {
        size_t n = strlen(seq);
        memcpy(out, seq, n);
        return n;
    }
    out[0] = ev->ch;
    return ev->ch ? 1 : 0;
}

// Wait for at least one byte; return what is there, up to `size`.
static int64_t console_read(struct vnode *v, uint64_t offset, void *buf, size_t size) {
    if (size == 0) return 0;
    for (;;) {
        spin_lock(&input_lock);
        struct key_event ev;
        while (pending_pos == pending_len && keyboard_read(&ev)) {
            pending_pos = 0;
            pending_len = key_bytes(&ev, pending);
        }
        size_t n = pending_len - pending_pos;
        if (n > size) n = size;
        memcpy(buf, pending + pending_pos, n);
        pending_pos += n;
        spin_unlock(&input_lock);
        if (n) return (int64_t)n;
        thread_sleep(1); // as the kernel shell does: a key waits a tick or two
    }
}

static const struct vnode_ops console_ops = {.fs_name = "devfs", .read = console_read, .write = console_write};
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
