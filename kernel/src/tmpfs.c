// tmpfs: files and directories kept in kernel heap memory. See vfs.h.
//
// A file's bytes are one heap block that grows (doubling) as it is
// written. A directory keeps its entries in a list, in the order they were
// made, which is the order readdir gives them back in.

#include <errno.h>
#include <kheap.h>
#include <string.h>
#include <vfs.h>

#define TMPFS_FILE_MAX ((uint64_t)256 << 20) // bytes in one file
#define TMPFS_MIN_CAPACITY 64

struct tmpfs_node {
    struct vnode vnode; // vnode.data points back here
    char name[VFS_NAME_MAX + 1];
    struct tmpfs_node *next;                   // in the parent directory
    struct tmpfs_node *children, *last_child; // directories
    uint8_t *bytes;                            // files: `capacity` bytes, the first vnode.size in use
    uint64_t capacity;
};

static const struct vnode_ops tmpfs_ops;
static uint64_t next_ino = 1;

static struct tmpfs_node *node(struct vnode *v) { return v->data; }

static struct tmpfs_node *new_node(enum vnode_type type, const char *name) {
    struct tmpfs_node *n = kcalloc(1, sizeof *n);
    if (n == NULL) return NULL;
    n->vnode = (struct vnode){.type = type, .ino = next_ino++, .ops = &tmpfs_ops, .data = n};
    size_t len = strlen(name);
    memcpy(n->name, name, len + 1);
    return n;
}

// Make room for `size` bytes, zeroing the new ones up to there.
static int grow(struct tmpfs_node *n, uint64_t size) {
    if (size > TMPFS_FILE_MAX) return -EFBIG;
    if (size > n->capacity) {
        uint64_t cap = n->capacity < TMPFS_MIN_CAPACITY ? TMPFS_MIN_CAPACITY : n->capacity;
        while (cap < size) cap *= 2;
        if (cap > TMPFS_FILE_MAX) cap = TMPFS_FILE_MAX;
        uint8_t *b = krealloc(n->bytes, cap);
        if (b == NULL) return -ENOSPC;
        n->bytes = b;
        n->capacity = cap;
    }
    if (size > n->vnode.size) memset(n->bytes + n->vnode.size, 0, size - n->vnode.size);
    return 0;
}

static int64_t tmpfs_read(struct vnode *v, uint64_t offset, void *buf, size_t size) {
    if (offset >= v->size) return 0;
    if (size > v->size - offset) size = v->size - offset;
    memcpy(buf, node(v)->bytes + offset, size);
    return (int64_t)size;
}

static int64_t tmpfs_write(struct vnode *v, uint64_t offset, const void *buf, size_t size) {
    if (size == 0) return 0;
    uint64_t end = offset + size;
    if (end < offset) return -EFBIG;
    if (end > v->size) {
        int err = grow(node(v), end);
        if (err) return err;
        v->size = end;
    }
    memcpy(node(v)->bytes + offset, buf, size);
    return (int64_t)size;
}

static int tmpfs_truncate(struct vnode *v, uint64_t size) {
    struct tmpfs_node *n = node(v);
    if (size == 0) {
        kfree(n->bytes);
        n->bytes = NULL;
        n->capacity = 0;
    } else if (size > v->size) {
        int err = grow(n, size);
        if (err) return err;
    }
    v->size = size;
    return 0;
}

static int tmpfs_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    for (struct tmpfs_node *c = node(dir)->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) {
            *out = &c->vnode;
            return 0;
        }
    }
    return -ENOENT;
}

static int tmpfs_create(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out) {
    if (type == VNODE_CHARDEV) return -EPERM;
    struct tmpfs_node *d = node(dir), *n = new_node(type, name);
    if (n == NULL) return -ENOSPC;
    if (d->last_child) d->last_child->next = n;
    else d->children = n;
    d->last_child = n;
    dir->size++;
    *out = &n->vnode;
    return 0;
}

static int tmpfs_readdir(struct vnode *dir, uint64_t index, struct vfs_dirent *out) {
    struct tmpfs_node *c = node(dir)->children;
    for (; c && index; index--) c = c->next;
    if (c == NULL) return -ENOENT;
    memcpy(out->name, c->name, sizeof out->name);
    out->type = c->vnode.type;
    out->ino = c->vnode.ino;
    return 0;
}

static const struct vnode_ops tmpfs_ops = {
    .fs_name = "tmpfs",
    .read = tmpfs_read,
    .write = tmpfs_write,
    .truncate = tmpfs_truncate,
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .readdir = tmpfs_readdir,
};

struct vnode *tmpfs_new(void) {
    struct tmpfs_node *n = new_node(VNODE_DIR, "");
    return n ? &n->vnode : NULL;
}

void tmpfs_free(struct vnode *root) {
    struct tmpfs_node *n = node(root);
    for (struct tmpfs_node *c = n->children, *next; c; c = next) {
        next = c->next;
        tmpfs_free(&c->vnode);
    }
    kfree(n->bytes);
    kfree(n);
}
