// The virtual filesystem: path lookup, mounts, the open-file table and the
// file calls. See vfs.h.

#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <string.h>
#include <sync.h>
#include <vfs.h>

struct file {
    unsigned refs; // 0: the slot is free
    int flags;
    struct vnode *vnode;
    uint64_t offset; // in bytes; for a directory, the index of the next entry
};

static struct mutex vfs_lock;
static struct vnode *root;
static struct file files[VFS_MAX_FILES];

// ---------------------------------------------------------------------------
// Path lookup
// ---------------------------------------------------------------------------

// A filesystem mounted on `v` stands in for it (and one mounted on that...).
static struct vnode *covering(struct vnode *v) {
    while (v->mounted) v = v->mounted;
    return v;
}

// The directories walked so far, root first, so ".." can go back up across
// mount points.
struct walk {
    struct vnode *dirs[VFS_DEPTH_MAX + 1];
    int depth; // entries in dirs
};

static struct vnode *walk_top(const struct walk *w) { return w->dirs[w->depth - 1]; }

// Step into the component `name` (`len` bytes, no slash) from the top.
static int walk_step(struct walk *w, const char *name, size_t len) {
    struct vnode *dir = walk_top(w);
    if (dir->type != VNODE_DIR) return -ENOTDIR;
    if (len == 1 && name[0] == '.') return 0;
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        if (w->depth > 1) w->depth--;
        return 0;
    }
    if (len > VFS_NAME_MAX) return -ENAMETOOLONG;
    if (w->depth > VFS_DEPTH_MAX) return -ENAMETOOLONG;
    char buf[VFS_NAME_MAX + 1];
    memcpy(buf, name, len);
    buf[len] = '\0';
    struct vnode *v;
    if (dir->ops->lookup == NULL) return -ENOENT;
    int err = dir->ops->lookup(dir, buf, &v);
    if (err) return err;
    w->dirs[w->depth++] = covering(v);
    return 0;
}

// Walk the first `len` bytes of `path`.
static int walk_path(struct walk *w, const char *path, size_t len) {
    if (root == NULL) return -ENOENT;
    w->dirs[0] = covering(root);
    w->depth = 1;
    size_t i = 0;
    for (;;) {
        while (i < len && path[i] == '/') i++;
        if (i == len) return 0;
        size_t start = i;
        while (i < len && path[i] != '/') i++;
        int err = walk_step(w, path + start, i - start);
        if (err) return err;
    }
}

// The vnode `path` names.
static int resolve(const char *path, struct vnode **out) {
    size_t len = strlen(path);
    if (len > VFS_PATH_MAX) return -ENAMETOOLONG;
    struct walk w;
    int err = walk_path(&w, path, len);
    if (err) return err;
    struct vnode *v = walk_top(&w);
    if (len > 0 && path[len - 1] == '/' && v->type != VNODE_DIR) return -ENOTDIR; // "/etc/motd/"
    *out = v;
    return 0;
}

// The directory `path` would be made in, and the name it would have there.
// -EEXIST if the last component is "/", "." or ".." (they always exist).
static int resolve_parent(const char *path, struct vnode **dir, char name[VFS_NAME_MAX + 1]) {
    size_t len = strlen(path);
    if (len > VFS_PATH_MAX) return -ENAMETOOLONG;
    size_t end = len;
    while (end > 0 && path[end - 1] == '/') end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t n = end - start;
    if (n == 0 || (n == 1 && path[start] == '.') || (n == 2 && path[start] == '.' && path[start + 1] == '.'))
        return -EEXIST;
    if (n > VFS_NAME_MAX) return -ENAMETOOLONG;
    struct walk w;
    int err = walk_path(&w, path, start);
    if (err) return err;
    *dir = walk_top(&w);
    if ((*dir)->type != VNODE_DIR) return -ENOTDIR;
    memcpy(name, path + start, n);
    name[n] = '\0';
    return 0;
}

static int create_in(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out) {
    if (dir->ops->create == NULL) return -EROFS;
    struct vnode *v;
    if (dir->ops->lookup && dir->ops->lookup(dir, name, &v) == 0) return -EEXIST;
    return dir->ops->create(dir, name, type, out);
}

// ---------------------------------------------------------------------------
// The calls
// ---------------------------------------------------------------------------

static struct file *get_file(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FILES || files[fd].refs == 0) return NULL;
    return &files[fd];
}

static int do_mount(const char *path, struct vnode *fs_root) {
    if (root == NULL) {
        if (strcmp(path, "/") != 0) return -ENOENT;
        root = fs_root;
        return 0;
    }
    struct vnode *v;
    int err = resolve(path, &v);
    if (err) return err;
    if (v->type != VNODE_DIR) return -ENOTDIR;
    v->mounted = fs_root; // `v` is already the topmost filesystem on that directory
    return 0;
}

int vfs_mount(const char *path, struct vnode *fs_root) {
    mutex_lock(&vfs_lock);
    int err = do_mount(path, fs_root);
    mutex_unlock(&vfs_lock);
    return err;
}

static int do_open(const char *path, int flags) {
    int mode = flags & O_ACCMODE;
    if (mode == O_ACCMODE) return -EINVAL;
    struct vnode *v;
    int err = resolve(path, &v);
    if (err == -ENOENT && (flags & O_CREAT)) {
        size_t len = strlen(path);
        if (len > 0 && path[len - 1] == '/') return -EISDIR; // only directories end in "/"
        struct vnode *dir;
        char name[VFS_NAME_MAX + 1];
        err = resolve_parent(path, &dir, name);
        if (err == 0) err = create_in(dir, name, VNODE_FILE, &v);
    }
    if (err) return err;
    if (v->type == VNODE_DIR && mode != O_RDONLY) return -EISDIR;

    int fd = 0;
    while (fd < VFS_MAX_FILES && files[fd].refs) fd++;
    if (fd == VFS_MAX_FILES) return -EMFILE;

    if ((flags & O_TRUNC) && mode != O_RDONLY && v->type == VNODE_FILE) {
        if (v->ops->truncate == NULL) return -EPERM;
        err = v->ops->truncate(v, 0);
        if (err) return err;
    }
    files[fd] = (struct file){.refs = 1, .flags = flags, .vnode = v};
    return fd;
}

int vfs_open(const char *path, int flags) {
    mutex_lock(&vfs_lock);
    int fd = do_open(path, flags);
    mutex_unlock(&vfs_lock);
    return fd;
}

int vfs_open_vnode(struct vnode *v, int flags) {
    mutex_lock(&vfs_lock);
    int fd = 0;
    while (fd < VFS_MAX_FILES && files[fd].refs) fd++;
    if (fd < VFS_MAX_FILES) files[fd] = (struct file){.refs = 1, .flags = flags, .vnode = v};
    mutex_unlock(&vfs_lock);
    return fd < VFS_MAX_FILES ? fd : -EMFILE;
}

struct vnode *vfs_hold_vnode(int fd) {
    mutex_lock(&vfs_lock);
    struct file *f = get_file(fd);
    if (f) f->refs++;
    mutex_unlock(&vfs_lock);
    return f ? f->vnode : NULL;
}

int vfs_hold(int fd) {
    mutex_lock(&vfs_lock);
    struct file *f = get_file(fd);
    if (f) f->refs++;
    mutex_unlock(&vfs_lock);
    return f ? 0 : -EBADF;
}

// The device `fd` is open on for `mode` (O_RDONLY: reading, O_WRONLY:
// writing), or NULL if it isn't a device, or not open that way.
static struct vnode *device(int fd, int mode) {
    struct file *f = get_file(fd);
    if (f == NULL || f->vnode->type != VNODE_CHARDEV || (f->flags & O_ACCMODE) == (mode ^ 1)) return NULL;
    return f->vnode;
}

static int64_t do_read(int fd, void *buf, size_t size) {
    struct file *f = get_file(fd);
    if (f == NULL || (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    struct vnode *v = f->vnode;
    if (v->type == VNODE_DIR) return -EISDIR;
    if (v->ops->read == NULL) return -EPERM;
    int64_t n = v->ops->read(v, f->offset, buf, size);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

int64_t vfs_read(int fd, void *buf, size_t size) {
    mutex_lock(&vfs_lock);
    struct vnode *dev = device(fd, O_RDONLY);
    if (dev && dev->ops->read) {
        mutex_unlock(&vfs_lock);
        return dev->ops->read(dev, 0, buf, size); // may wait: without the lock
    }
    int64_t n = do_read(fd, buf, size);
    mutex_unlock(&vfs_lock);
    return n;
}

static int64_t do_write(int fd, const void *buf, size_t size) {
    struct file *f = get_file(fd);
    if (f == NULL || (f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    struct vnode *v = f->vnode; // never a directory: those only open read-only
    if (v->ops->write == NULL) return -EPERM;
    if (f->flags & O_APPEND) f->offset = v->size;
    int64_t n = v->ops->write(v, f->offset, buf, size);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

int64_t vfs_write(int fd, const void *buf, size_t size) {
    mutex_lock(&vfs_lock);
    struct vnode *dev = device(fd, O_WRONLY);
    if (dev && dev->ops->write) {
        mutex_unlock(&vfs_lock);
        return dev->ops->write(dev, 0, buf, size);
    }
    int64_t n = do_write(fd, buf, size);
    mutex_unlock(&vfs_lock);
    return n;
}

static int do_readdir(int fd, struct vfs_dirent *out) {
    struct file *f = get_file(fd);
    if (f == NULL) return -EBADF;
    struct vnode *v = f->vnode;
    if (v->type != VNODE_DIR) return -ENOTDIR;
    if (v->ops->readdir == NULL) return -EPERM;
    int err = v->ops->readdir(v, f->offset, out);
    if (err == -ENOENT) return 0;
    if (err) return err;
    f->offset++;
    return 1;
}

int vfs_readdir(int fd, struct vfs_dirent *out) {
    mutex_lock(&vfs_lock);
    int r = do_readdir(fd, out);
    mutex_unlock(&vfs_lock);
    return r;
}

int vfs_close(int fd) {
    mutex_lock(&vfs_lock);
    struct file *f = get_file(fd);
    struct vnode *released = NULL;
    if (f && --f->refs == 0 && f->vnode->ops->release) released = f->vnode;
    mutex_unlock(&vfs_lock);
    if (released) released->ops->release(released); // a socket: it may wait for the network
    return f ? 0 : -EBADF;
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    mutex_lock(&vfs_lock);
    struct vnode *v;
    int err = resolve(path, &v);
    if (err == 0) *st = (struct vfs_stat){.type = v->type, .ino = v->ino, .size = v->size, .fs = v->ops->fs_name};
    mutex_unlock(&vfs_lock);
    return err;
}

int vfs_mkdir(const char *path) {
    mutex_lock(&vfs_lock);
    struct vnode *dir, *v;
    char name[VFS_NAME_MAX + 1];
    int err = resolve_parent(path, &dir, name);
    if (err == 0) err = create_in(dir, name, VNODE_DIR, &v);
    mutex_unlock(&vfs_lock);
    return err;
}

static int do_unlink(const char *path) {
    struct vnode *dir, *v;
    char name[VFS_NAME_MAX + 1];
    int err = resolve_parent(path, &dir, name);
    if (err) return err == -EEXIST ? -EBUSY : err; // "/", "." or ".."
    if (dir->ops->lookup == NULL) return -ENOENT;
    err = dir->ops->lookup(dir, name, &v);
    if (err) return err;
    size_t len = strlen(path);
    if (len && path[len - 1] == '/' && v->type != VNODE_DIR) return -ENOTDIR;
    if (v->mounted) return -EBUSY;
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (files[i].refs && files[i].vnode == v) return -EBUSY;
    if (dir->ops->unlink == NULL) return -EPERM;
    return dir->ops->unlink(dir, name);
}

int vfs_unlink(const char *path) {
    mutex_lock(&vfs_lock);
    int err = do_unlink(path);
    mutex_unlock(&vfs_lock);
    return err;
}

void vfs_init(void) {
    mutex_init(&vfs_lock, "vfs");
    struct vnode *tmp = tmpfs_new();
    if (tmp == NULL) panic("vfs: no memory for the root tmpfs");
    int err = vfs_mount("/", tmp);
    if (err == 0) err = vfs_mkdir("/dev");
    if (err == 0) err = vfs_mount("/dev", devfs_root());
    if (err) panic("vfs: could not set up / and /dev: %s", strerror(err));
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

static void check(int64_t got, int64_t want, const char *what) {
    if (got != want)
        panic("vfs self-test: %s: got %ld (%s), expected %ld (%s)", what, got, got < 0 ? strerror((int)got) : "",
              want, want < 0 ? strerror((int)want) : "");
}

static int open_ok(const char *path, int flags) {
    int fd = vfs_open(path, flags);
    if (fd < 0) panic("vfs self-test: open %s: %s", path, strerror(fd));
    return fd;
}

static uint64_t ino_of(const char *path) {
    struct vfs_stat st;
    check(vfs_stat(path, &st), 0, path);
    return st.ino;
}

#define TEST_FILE_SIZE 10000 // bytes: grows the file a few times, across pages

void vfs_self_test(char *line, size_t size) {
    uint64_t allocs0 = kheap_get_stats().allocations;
    struct vnode *test_root = tmpfs_new();
    if (test_root == NULL) panic("vfs self-test: no memory");
    mutex_lock(&vfs_lock);
    struct vnode *real_root = root;
    root = test_root;
    mutex_unlock(&vfs_lock);
    for (int i = 0; i < VFS_MAX_FILES; i++)
        if (files[i].refs) panic("vfs self-test: file %d is open", i);

    // Directories and mounts.
    check(vfs_mkdir("/dev"), 0, "mkdir /dev");
    check(vfs_mount("/dev", devfs_root()), 0, "mount devfs on /dev");
    check(vfs_mkdir("/a"), 0, "mkdir /a");
    check(vfs_mkdir("/a/b/"), 0, "mkdir /a/b/");
    check(vfs_mkdir("/a"), -EEXIST, "mkdir /a again");
    check(vfs_mkdir("/"), -EEXIST, "mkdir /");
    check(vfs_mkdir("/a/.."), -EEXIST, "mkdir /a/..");
    check(vfs_mkdir("/nope/c"), -ENOENT, "mkdir /nope/c");
    check(vfs_mkdir("/dev/d"), -EROFS, "mkdir in devfs");
    char long_name[VFS_NAME_MAX + 3] = "/";
    memset(long_name + 1, 'x', VFS_NAME_MAX + 1);
    check(vfs_mkdir(long_name), -ENAMETOOLONG, "mkdir with a long name");
    long_name[VFS_NAME_MAX + 1] = '\0';
    check(vfs_mkdir(long_name), 0, "mkdir with the longest name");

    struct vfs_stat st;
    check(vfs_stat("/a/b", &st), 0, "stat /a/b");
    if (st.type != VNODE_DIR || strcmp(st.fs, "tmpfs") != 0) panic("vfs self-test: /a/b is not a tmpfs directory");
    check((int64_t)ino_of("//a/./b/../b//"), (int64_t)st.ino, "/a/b by a roundabout path");
    check((int64_t)ino_of("/a/../../.."), (int64_t)ino_of("/"), ".. from /");
    check((int64_t)ino_of("/dev/.."), (int64_t)ino_of("/"), ".. out of a mount");
    check(vfs_stat("/dev", &st), 0, "stat /dev");
    if (strcmp(st.fs, "devfs") != 0 || st.size != 3) panic("vfs self-test: /dev is not the devfs root");
    check(vfs_stat("/a/missing", &st), -ENOENT, "stat a missing file");

    // A file: write it in pieces, read it back in others.
    int fd = open_ok("/a/b/f", O_RDWR | O_CREAT);
    static uint8_t buf[TEST_FILE_SIZE], back[TEST_FILE_SIZE];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 7 + i / 251);
    for (size_t off = 0; off < sizeof buf; off += 999) {
        size_t n = sizeof buf - off < 999 ? sizeof buf - off : 999;
        check(vfs_write(fd, buf + off, n), (int64_t)n, "write a piece");
    }
    check(vfs_read(fd, back, 10), 0, "read at the end");
    check(vfs_close(fd), 0, "close");
    check(vfs_close(fd), -EBADF, "close again");
    check(vfs_stat("/a/b/f", &st), 0, "stat the file");
    if (st.type != VNODE_FILE || st.size != sizeof buf)
        panic("vfs self-test: the file is %lu bytes, expected %zu", st.size, sizeof buf);
    fd = open_ok("/a/b/f", O_RDONLY);
    for (size_t off = 0; off < sizeof back;) {
        int64_t n = vfs_read(fd, back + off, 4096 + 13);
        if (n <= 0) panic("vfs self-test: read returned %ld at %zu", n, off);
        off += (size_t)n;
    }
    if (memcmp(buf, back, sizeof buf) != 0) panic("vfs self-test: the file reads back different");
    check(vfs_read(fd, back, 1), 0, "read past the end");
    check(vfs_write(fd, buf, 1), -EBADF, "write to a read-only file");
    check(vfs_readdir(fd, NULL), -ENOTDIR, "readdir on a file");
    // A second reference keeps it open, with its offset, through a close.
    check(vfs_hold(fd), 0, "hold");
    check(vfs_close(fd), 0, "close one of two references");
    check(vfs_read(fd, back, 1), 0, "read after closing one reference");
    check(vfs_close(fd), 0, "close the last reference");
    check(vfs_hold(fd), -EBADF, "hold a closed file");

    fd = open_ok("/a/b/f", O_WRONLY | O_TRUNC);
    check(vfs_read(fd, back, 1), -EBADF, "read from a write-only file");
    check(vfs_write(fd, "hi", 2), 2, "write after O_TRUNC");
    vfs_close(fd);
    check(vfs_stat("/a/b/f", &st), 0, "stat the truncated file");
    check((int64_t)st.size, 2, "size after O_TRUNC");
    fd = open_ok("/a/b/f", O_WRONLY | O_APPEND);
    check(vfs_write(fd, "!", 1), 1, "write with O_APPEND");
    vfs_close(fd);
    check(vfs_stat("/a/b/f", &st), 0, "stat after O_APPEND");
    check((int64_t)st.size, 3, "size after O_APPEND");

    check(vfs_open("/a/b/f/", O_RDONLY), -ENOTDIR, "open a file with a trailing slash");
    check(vfs_open("/a/b/f/g", O_RDWR | O_CREAT), -ENOTDIR, "create under a file");
    check(vfs_open("/a/new/", O_RDWR | O_CREAT), -EISDIR, "create a file with a trailing slash");
    check(vfs_open("/a/nope", O_RDONLY), -ENOENT, "open a missing file");
    check(vfs_open("/a", O_WRONLY), -EISDIR, "open a directory for writing");
    check(vfs_open("/a", O_ACCMODE), -EINVAL, "open with a bad mode");
    check(vfs_open("/dev/new", O_WRONLY | O_CREAT), -EROFS, "create in devfs");

    // Directory listing, in creation order.
    check(vfs_mkdir("/a/c"), 0, "mkdir /a/c");
    fd = open_ok("/a", O_RDONLY);
    struct vfs_dirent d;
    check(vfs_read(fd, back, 1), -EISDIR, "read a directory");
    check(vfs_readdir(fd, &d), 1, "first entry");
    if (strcmp(d.name, "b") != 0 || d.type != VNODE_DIR) panic("vfs self-test: first entry of /a is %s", d.name);
    check(vfs_readdir(fd, &d), 1, "second entry");
    if (strcmp(d.name, "c") != 0) panic("vfs self-test: second entry of /a is %s", d.name);
    check(vfs_readdir(fd, &d), 0, "end of /a");
    vfs_close(fd);

    // Deleting.
    fd = open_ok("/a/b/f", O_RDONLY);
    check(vfs_unlink("/a/b/f"), -EBUSY, "unlink an open file");
    vfs_close(fd);
    check(vfs_unlink("/a/b"), -ENOTEMPTY, "unlink a directory that isn't empty");
    check(vfs_unlink("/a/b/f/"), -ENOTDIR, "unlink a file with a trailing slash");
    check(vfs_unlink("/a/b/f"), 0, "unlink a file");
    check(vfs_stat("/a/b/f", &st), -ENOENT, "stat a deleted file");
    check(vfs_unlink("/a/b/f"), -ENOENT, "unlink it again");
    check(vfs_unlink("/a/b"), 0, "unlink an empty directory");
    check(vfs_unlink("/dev"), -EBUSY, "unlink a mount point");
    check(vfs_unlink("/dev/null"), -EPERM, "unlink in devfs");
    check(vfs_unlink("/"), -EBUSY, "unlink /");
    check(vfs_mkdir("/a/d"), 0, "mkdir /a/d after an unlink");
    fd = open_ok("/a", O_RDONLY);
    check(vfs_readdir(fd, &d), 1, "first entry after the unlink");
    if (strcmp(d.name, "c") != 0) panic("vfs self-test: first entry of /a is %s after the unlink", d.name);
    check(vfs_readdir(fd, &d), 1, "second entry after the unlink");
    if (strcmp(d.name, "d") != 0) panic("vfs self-test: second entry of /a is %s after the unlink", d.name);
    check(vfs_readdir(fd, &d), 0, "end of /a after the unlink");
    vfs_close(fd);

    // Devices.
    fd = open_ok("/dev/null", O_RDWR);
    check(vfs_write(fd, buf, sizeof buf), sizeof buf, "write /dev/null");
    check(vfs_read(fd, back, 100), 0, "read /dev/null");
    vfs_close(fd);
    fd = open_ok("/dev/zero", O_RDONLY);
    memset(back, 0xaa, 300);
    check(vfs_read(fd, back, 300), 300, "read /dev/zero");
    for (int i = 0; i < 300; i++)
        if (back[i]) panic("vfs self-test: /dev/zero gave a %u", back[i]);
    vfs_close(fd);
    fd = open_ok("/dev/console", O_RDWR);
    check(vfs_write(fd, "", 0), 0, "write nothing to /dev/console");
    check(vfs_read(fd, back, 0), 0, "read nothing from /dev/console"); // more would wait for a key
    vfs_close(fd);
    fd = open_ok("/dev/null", O_WRONLY);
    check(vfs_read(fd, back, 1), -EBADF, "read a device open write-only");
    vfs_close(fd);
    fd = open_ok("/dev", O_RDONLY);
    int devices = 0;
    while (vfs_readdir(fd, &d) > 0) devices++;
    check(devices, 3, "entries in /dev");
    vfs_close(fd);

    // Running out of descriptors.
    int fds[VFS_MAX_FILES];
    for (int i = 0; i < VFS_MAX_FILES; i++) fds[i] = open_ok("/dev/null", O_RDONLY);
    check(vfs_open("/dev/null", O_RDONLY), -EMFILE, "open one file too many");
    for (int i = 0; i < VFS_MAX_FILES; i++) vfs_close(fds[i]);

    // Put the real root back and free everything.
    mutex_lock(&vfs_lock);
    root = real_root;
    mutex_unlock(&vfs_lock);
    tmpfs_free(test_root);
    uint64_t leaked = kheap_get_stats().allocations - allocs0;
    if (leaked) panic("vfs self-test: %lu allocations leaked", leaked);

    ksnprintf(line, size,
              "VFS self-test passed (tmpfs files and directories, unlink, devfs, mounts, \"..\", %d open files, the errors)",
              VFS_MAX_FILES);
}
