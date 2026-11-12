// System calls: the `syscall` instruction's setup, SMEP/SMAP, and the calls
// themselves. See syscall.h.

#include <arch.h>
#include <cpu.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pmm.h>
#include <proc.h>
#include <sched.h>
#include <shell.h>
#include <string.h>
#include <syscall.h>
#include <time.h>
#include <vfs.h>
#include <vmm.h>

#define EFER_SCE (1ull << 0) // syscall/sysret enabled

#define RFLAGS_TF (1ull << 8)
#define RFLAGS_DF (1ull << 10)
#define RFLAGS_NT (1ull << 14)
#define RFLAGS_AC (1ull << 18)

#define CR4_SMEP (1ull << 20)
#define CR4_SMAP (1ull << 21)

bool smap_enabled;

void syscall_entry(void); // syscall.S

void syscall_init(char *line, size_t size) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    // STAR[47:32]: `syscall` loads CS from it (0x08) and SS from it + 8
    // (0x10). STAR[63:48]: `sysret` loads SS from it + 8 (0x18 | 3) and CS
    // from it + 16 (0x20 | 3). That is why the GDT has user data before
    // user code.
    wrmsr(MSR_STAR, (uint64_t)GDT_KERNEL_DATA << 48 | (uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    // Cleared on entry: IF until the entry code is on the kernel stack, DF
    // for the C code, TF so a single-stepping user doesn't trap in the
    // kernel, AC so SMAP holds, NT so nothing odd happens on a later iret.
    wrmsr(MSR_FMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF | RFLAGS_AC | RFLAGS_NT);

    // SMEP: ring 0 can't run code from user pages. SMAP: ring 0 can't touch
    // user pages unless RFLAGS.AC is set (stac/clac, in copy_*_user).
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    bool smep = false, smap = false;
    if (a >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        smep = b & (1u << 7);
        smap = b & (1u << 20);
    }
    uint64_t cr4 = read_cr4();
    if (smep) cr4 |= CR4_SMEP;
    if (smap) cr4 |= CR4_SMAP;
    write_cr4(cr4);
    smap_enabled = smap;

    ksnprintf(line, size, "system calls via syscall/sysret; SMEP %s, SMAP %s",
              smep ? "on" : "not supported", smap ? "on" : "not supported");
}

// ---------------------------------------------------------------------------
// The calls
// ---------------------------------------------------------------------------

// The VFS file behind descriptor `fd`, or -EBADF.
static int file_of(struct process *p, int64_t fd) {
    if (fd < 0 || fd >= PROC_MAX_FDS || p->fds[fd] < 0) return -EBADF;
    return p->fds[fd];
}

// The lowest free descriptor, or -EMFILE.
static int free_fd(struct process *p) {
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fds[i] < 0) return i;
    return -EMFILE;
}

#define IO_CHUNK 512 // bytes moved through the kernel stack at a time

static int64_t sys_read(int64_t fd, uint64_t buf, uint64_t len) {
    struct process *p = process_current();
    int file = file_of(p, fd);
    if (file < 0) return file;
    if (!user_range_ok(buf, len, true)) return -EFAULT;
    char chunk[IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        size_t want = len - done < sizeof chunk ? len - done : sizeof chunk;
        int64_t n = vfs_read(file, chunk, want);
        if (n < 0) return done ? (int64_t)done : n;
        if (n > 0 && copy_to_user(buf + done, chunk, (size_t)n)) return done ? (int64_t)done : -EFAULT;
        done += (uint64_t)n;
        if ((size_t)n < want) break; // the end of a file, or all the console had
    }
    return (int64_t)done;
}

static int64_t sys_write(int64_t fd, uint64_t buf, uint64_t len) {
    struct process *p = process_current();
    // The self-test's processes write fd 1 and 2 into a buffer (proc.h).
    bool to_buffer = p->out && (fd == 1 || fd == 2);
    int file = to_buffer ? 0 : file_of(p, fd);
    if (file < 0) return file;
    // Check it all first, so a bad buffer writes nothing.
    if (!user_range_ok(buf, len, false)) return -EFAULT;
    char chunk[IO_CHUNK];
    for (uint64_t done = 0; done < len;) {
        size_t n = len - done < sizeof chunk ? len - done : sizeof chunk;
        if (copy_from_user(chunk, buf + done, n)) return done ? (int64_t)done : -EFAULT;
        if (to_buffer) {
            process_output(p, chunk, n);
        } else {
            int64_t w = vfs_write(file, chunk, n);
            if (w < 0) return done ? (int64_t)done : w;
            if ((size_t)w < n) return (int64_t)(done + (uint64_t)w); // the device or filesystem is full
        }
        done += n;
    }
    return (int64_t)len;
}

static int64_t sys_open(uint64_t user_path, uint64_t flags, uint64_t mode) {
    if (flags & ~(uint64_t)(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) return -EINVAL;
    char path[VFS_PATH_MAX + 1];
    int64_t err = copy_string_from_user(path, user_path, sizeof path);
    if (err < 0) return err;
    struct process *p = process_current();
    int fd = free_fd(p);
    if (fd < 0) return fd;
    int file = vfs_open(path, (int)flags);
    if (file < 0) return file;
    p->fds[fd] = file;
    return fd;
}

static int64_t sys_close(int64_t fd) {
    struct process *p = process_current();
    int file = file_of(p, fd);
    if (file < 0) return file;
    p->fds[fd] = -1;
    return vfs_close(file);
}

static int64_t sys_dup(int64_t fd) {
    struct process *p = process_current();
    int file = file_of(p, fd);
    if (file < 0) return file;
    int new_fd = free_fd(p);
    if (new_fd < 0) return new_fd;
    int err = vfs_hold(file);
    if (err) return err;
    p->fds[new_fd] = file;
    return new_fd;
}

static int64_t sys_dup2(int64_t fd, int64_t new_fd) {
    struct process *p = process_current();
    int file = file_of(p, fd);
    if (file < 0) return file;
    if (new_fd < 0 || new_fd >= PROC_MAX_FDS) return -EBADF;
    if (new_fd == fd) return new_fd;
    int err = vfs_hold(file);
    if (err) return err;
    if (p->fds[new_fd] >= 0) vfs_close(p->fds[new_fd]);
    p->fds[new_fd] = file;
    return new_fd;
}

// A list of strings (argv, envp) copied in from user space.
#define STRV_MAX 128 // strings in one list
struct strv {
    char *v[STRV_MAX + 1]; // NULL-terminated
    char buf[USER_ARGS_MAX];
};

// Copy the NULL-terminated array of strings at `user_vec` (NULL: an empty
// list) into a new strv. Sets *out, or returns -EFAULT, -E2BIG or -ENOMEM.
static int copy_strv(uint64_t user_vec, struct strv **out) {
    struct strv *s = kmalloc(sizeof *s);
    if (s == NULL) return -ENOMEM;
    size_t n = 0, used = 0;
    for (; user_vec; n++) {
        uint64_t str;
        int64_t err = copy_from_user(&str, user_vec + n * sizeof str, sizeof str);
        if (err == 0 && str == 0) break;
        if (err == 0 && n == STRV_MAX) err = -E2BIG;
        if (err == 0) err = copy_string_from_user(s->buf + used, str, sizeof s->buf - used);
        if (err < 0) {
            kfree(s);
            return err == -ENAMETOOLONG ? -E2BIG : (int)err;
        }
        s->v[n] = s->buf + used;
        used += (size_t)err + 1;
    }
    s->v[n] = NULL;
    *out = s;
    return 0;
}

static int64_t sys_spawn(uint64_t user_path, uint64_t user_argv, uint64_t user_envp) {
    char path[VFS_PATH_MAX + 1];
    int64_t err = copy_string_from_user(path, user_path, sizeof path);
    if (err < 0) return err;
    struct strv *argv, *envp = NULL;
    err = copy_strv(user_argv, &argv);
    if (err) return err;
    err = copy_strv(user_envp, &envp);
    if (err == 0) err = process_spawn(process_current(), path, argv->v, envp->v);
    kfree(argv);
    kfree(envp);
    return err;
}

// A stopgap until the kernel shell's commands (mem, ps, reboot, ...) have
// system calls of their own and become programs: the user-space shell runs
// them here, in its own thread, and they print straight to the console.
static int64_t sys_kernel_command(uint64_t user_argv) {
    struct strv *argv;
    int err = copy_strv(user_argv, &argv);
    if (err) return err;
    int argc = 0;
    while (argv->v[argc]) argc++;
    const struct shell_command *c = argc ? shell_find_command(argv->v[0]) : NULL;
    if (c) c->run(argc, argv->v);
    kfree(argv);
    return argc == 0 ? -EINVAL : c ? 0 : -ENOENT;
}

static int64_t sys_wait4(int64_t pid, uint64_t user_status, int64_t options, uint64_t rusage) {
    if (rusage) return -EINVAL; // not kept
    int status;
    int64_t r = process_waitpid(process_current(), pid, &status, (int)options);
    // The child is reaped either way; a bad pointer only loses its status.
    if (r > 0 && user_status && copy_to_user(user_status, &status, sizeof status)) return -EFAULT;
    return r;
}

// Anonymous memory, zero-filled, at an address the kernel picks.
static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
    if (len == 0 || (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))) return -EINVAL;
    if (!(flags & MAP_ANONYMOUS)) return -EINVAL; // no files to map yet
    if (flags & MAP_FIXED) return -EINVAL;        // the address is only a hint, and ignored
    if ((flags & (MAP_SHARED | MAP_PRIVATE)) == 0 ||
        (flags & ~(uint64_t)(MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS)))
        return -EINVAL;

    struct process *p = process_current();
    if (len > USER_MMAP_END - p->mmap_next) return -ENOMEM;
    uint64_t size = ALIGN_UP(len, PAGE_SIZE);
    if (size > USER_MMAP_END - p->mmap_next) return -ENOMEM;
    uint64_t start = p->mmap_next;

    // PROT_NONE: the range is reserved and stays unmapped. x86 pages are
    // always readable, so PROT_WRITE and PROT_EXEC imply PROT_READ.
    if (prot != PROT_NONE) {
        uint32_t vflags = VMM_USER | (prot & PROT_WRITE ? VMM_WRITE : 0) | (prot & PROT_EXEC ? VMM_EXEC : 0);
        for (uint64_t done = 0; done < size; done += PAGE_SIZE) {
            preempt_disable(); // the page allocator isn't thread-safe
            uint64_t page = pmm_alloc_page();
            if (page) {
                memset(phys_to_virt(page), 0, PAGE_SIZE);
                vmm_map(&p->space, start + done, page, vflags);
            }
            preempt_enable();
            if (page == 0) {
                // Give back what was mapped so far.
                for (uint64_t v = start; v < start + done; v += PAGE_SIZE) {
                    uint64_t phys;
                    vmm_virt_to_phys(&p->space, v, &phys);
                    preempt_disable();
                    vmm_unmap(&p->space, v);
                    pmm_free_page(phys);
                    preempt_enable();
                }
                return -ENOMEM;
            }
        }
    }
    p->mmap_next = start + size;
    return (int64_t)start;
}

static int64_t sys_nanosleep(uint64_t req, uint64_t rem) {
    struct timespec ts;
    if (copy_from_user(&ts, req, sizeof ts)) return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= (int64_t)NS_PER_SEC) return -EINVAL;
    // Whole milliseconds, rounded up: never shorter than asked. Nothing
    // interrupts a sleep yet, so `rem` is never written. Over a century is
    // as good as forever, and keeps the arithmetic from overflowing.
    if (ts.tv_sec > 100ll * 365 * 24 * 3600) ts.tv_sec = 100ll * 365 * 24 * 3600;
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + ((uint64_t)ts.tv_nsec + NS_PER_MS - 1) / NS_PER_MS;
    thread_sleep(ms);
    return 0;
}

static int64_t sys_clock_gettime(uint64_t clock, uint64_t user_ts) {
    int64_t ns;
    if (clock == CLOCK_MONOTONIC) ns = (int64_t)clock_monotonic_ns();
    else if (clock != CLOCK_REALTIME || !clock_realtime_ns(&ns)) return -EINVAL; // no date from the bootloader
    struct timespec ts = {ns / (int64_t)NS_PER_SEC, ns % (int64_t)NS_PER_SEC};
    return copy_to_user(user_ts, &ts, sizeof ts);
}

void syscall_dispatch(struct interrupt_frame *f) {
    struct process *p = process_current();
    if (p == NULL) panic("syscall from a thread with no process");
    uint64_t a0 = f->rdi, a1 = f->rsi, a2 = f->rdx, a3 = f->r10, a4 = f->r8, a5 = f->r9;
    int64_t ret;
    switch (f->rax) {
    case SYS_read: ret = sys_read((int64_t)a0, a1, a2); break;
    case SYS_write: ret = sys_write((int64_t)a0, a1, a2); break;
    case SYS_open: ret = sys_open(a0, a1, a2); break;
    case SYS_close: ret = sys_close((int64_t)a0); break;
    case SYS_dup: ret = sys_dup((int64_t)a0); break;
    case SYS_dup2: ret = sys_dup2((int64_t)a0, (int64_t)a1); break;
    case SYS_wait4: ret = sys_wait4((int64_t)a0, a1, (int64_t)a2, a3); break;
    case SYS_getppid: ret = (int64_t)process_parent_pid(p); break;
    case SYS_spawn: ret = sys_spawn(a0, a1, a2); break;
    case SYS_kernel_command: ret = sys_kernel_command(a0); break;
    case SYS_mmap: ret = sys_mmap(a0, a1, a2, a3, (int64_t)a4, a5); break;
    case SYS_nanosleep: ret = sys_nanosleep(a0, a1); break;
    case SYS_getpid: ret = (int64_t)p->pid; break;
    case SYS_clock_gettime: ret = sys_clock_gettime(a0, a1); break;
    case SYS_exit:
        p->status = (int)(a0 & 0xff); // what a wait() would see, as on Linux
        p->fault = -1;
        process_exit();
    default: ret = -ENOSYS; break;
    }
    f->rax = (uint64_t)ret;
}
