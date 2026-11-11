#pragma once
// The virtual filesystem: one file API for every kind of file.
//
// Every file, directory and device is a vnode. Its filesystem gives it a
// table of operations (struct vnode_ops), and the VFS calls through that,
// so open, read, write and readdir work the same on tmpfs (files in RAM,
// tmpfs.c), devfs (/dev, devfs.c) and, later, filesystems on disks.
//
// Paths: there is no working directory until there are processes, so every
// path starts at "/" ("etc/motd" is "/etc/motd"). "." and ".." are resolved
// by name while the path is walked, and repeated slashes are one.
//
// Mounting a filesystem on a directory makes its root stand in for that
// directory in every path from then on; ".." from the root of a mounted
// filesystem goes to the directory above the mount point ("/dev/.." is "/").
//
// Vnodes live as long as their filesystem. Nothing can be deleted yet, so
// they need no reference counts.
//
// Open files live in one table for the whole kernel (VFS_MAX_FILES), and a
// file descriptor is an index into it. Processes will get their own tables.
//
// One mutex covers the VFS and every filesystem in it: these calls may
// sleep, so not from interrupt handlers, and not holding a spinlock.
//
// Errors are negative errno values (errno.h).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_NAME_MAX  63  // bytes in one path component
#define VFS_PATH_MAX  255 // bytes in a path
#define VFS_DEPTH_MAX 64  // directories deep a path may go
#define VFS_MAX_FILES 32  // files open at once

// vfs_open flags (Linux's values).
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0100  // create the file if it does not exist
#define O_TRUNC   01000 // empty the file (when opened for writing)

enum vnode_type { VNODE_FILE, VNODE_DIR, VNODE_CHARDEV };

struct vnode;

struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    enum vnode_type type;
    uint64_t ino;
};

struct vfs_stat {
    enum vnode_type type;
    uint64_t ino;
    uint64_t size;  // bytes in a file, entries in a directory, 0 for a device
    const char *fs; // the filesystem's name: "tmpfs"
};

// What a filesystem implements. An operation it leaves NULL fails with
// -EPERM (-EROFS for create). The VFS lock is held during every call, and
// the VFS has already checked the vnode's type and the file's access mode.
struct vnode_ops {
    const char *fs_name;

    // Files and devices: move up to `size` bytes at `offset`. Return how
    // many, or an error; read returns 0 at the end of a file.
    int64_t (*read)(struct vnode *v, uint64_t offset, void *buf, size_t size);
    int64_t (*write)(struct vnode *v, uint64_t offset, const void *buf, size_t size);
    // Files: set the size, filling any new bytes with zeroes.
    int (*truncate)(struct vnode *v, uint64_t size);

    // Directories. `name` is one component, not "." or "..".
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out); // -ENOENT if missing
    int (*create)(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out);
    // The entry at `index` (0, 1, ...), or -ENOENT past the last one.
    int (*readdir)(struct vnode *dir, uint64_t index, struct vfs_dirent *out);
};

struct vnode {
    enum vnode_type type;
    uint64_t ino;               // unique within its filesystem
    uint64_t size;              // see vfs_stat
    const struct vnode_ops *ops;
    void *data;                 // the filesystem's own
    struct vnode *mounted;      // the root of a filesystem mounted on this directory, or NULL
};

// Mount a tmpfs on "/" and a devfs on "/dev". Needs the heap and the
// scheduler.
void vfs_init(void);

// Mount the filesystem whose root is `root` on the directory `path`.
// Mounting on "/" before there is a root makes the root.
int vfs_mount(const char *path, struct vnode *root);

// Open a file, directory or device. Returns a file descriptor (>= 0).
// Directories open only read-only.
int vfs_open(const char *path, int flags);

// Read or write at the file's offset, and move it past what was moved.
int64_t vfs_read(int fd, void *buf, size_t size);
int64_t vfs_write(int fd, const void *buf, size_t size);

// The next entry of an open directory: 1, or 0 after the last one.
int vfs_readdir(int fd, struct vfs_dirent *out);

int vfs_close(int fd);
int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_mkdir(const char *path);

// Boot self-test on a private tmpfs (with a devfs on its /dev), swapped in
// as "/" while it runs: files, directories, mounts, devices, and the
// errors. Frees all it made. Writes a line for the boot log; panics on
// failure. Only while nothing else uses the VFS.
void vfs_self_test(char *line, size_t size);

// ---------------------------------------------------------------------------
// Filesystems
// ---------------------------------------------------------------------------

// A new, empty tmpfs: returns its root directory, or NULL if out of memory.
struct vnode *tmpfs_new(void);

// Free a tmpfs and everything in it. Nothing in it may be open, and
// nothing mounted on it.
void tmpfs_free(struct vnode *root);

// The devfs root: console (writes go to the screen and serial port, reads
// get end of file until there is a terminal driver), null and zero.
struct vnode *devfs_root(void);
