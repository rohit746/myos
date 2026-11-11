// System calls: the `syscall` instruction's setup, SMEP/SMAP, and the calls
// themselves. See syscall.h.

#include <arch.h>
#include <cpu.h>
#include <errno.h>
#include <kprintf.h>
#include <memory.h>
#include <pmm.h>
#include <proc.h>
#include <sched.h>
#include <string.h>
#include <syscall.h>
#include <time.h>
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

static int64_t sys_write(int64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return -EBADF; // no per-process files yet
    // Check it all first, so a bad buffer writes nothing.
    if (!user_range_ok(buf, len, false)) return -EFAULT;
    struct process *p = process_current();
    char chunk[256];
    for (uint64_t done = 0; done < len;) {
        size_t n = len - done < sizeof chunk ? len - done : sizeof chunk;
        if (copy_from_user(chunk, buf + done, n)) return done ? (int64_t)done : -EFAULT;
        process_output(p, chunk, n);
        done += n;
    }
    return (int64_t)len;
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
    case SYS_write: ret = sys_write((int64_t)a0, a1, a2); break;
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
