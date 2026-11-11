// User processes. See proc.h.

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
#include <vmm.h>

// syscall.S
__attribute__((noreturn)) void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg);

static uint64_t next_pid = 1;

struct process *process_current(void) {
    struct thread *t = thread_current();
    return t ? t->proc : NULL;
}

// Map `pages` fresh zeroed pages at `virt` in `p`, copying `size` bytes of
// `src` (may be NULL) to the start. False if there is no memory; what was
// mapped is freed with the space.
static bool map_pages(struct process *p, uint64_t virt, uint64_t pages, const void *src, size_t size,
                      uint32_t flags) {
    for (uint64_t i = 0; i < pages; i++) {
        preempt_disable(); // the page allocator isn't thread-safe
        uint64_t page = pmm_alloc_page();
        if (page) {
            uint8_t *dst = phys_to_virt(page);
            memset(dst, 0, PAGE_SIZE);
            size_t off = i * PAGE_SIZE;
            if (src && off < size) memcpy(dst, (const uint8_t *)src + off, size - off < PAGE_SIZE ? size - off : PAGE_SIZE);
            vmm_map(&p->space, virt + off, page, flags);
        }
        preempt_enable();
        if (page == 0) return false;
    }
    return true;
}

struct process *process_create(const char *name, const void *code, size_t size, uint64_t arg) {
    struct process *p = kcalloc(1, sizeof *p);
    if (p == NULL) return NULL;
    preempt_disable();
    bool ok = vmm_space_create(&p->space);
    preempt_enable();
    if (!ok) {
        kfree(p);
        return NULL;
    }
    size_t n = strlen(name);
    if (n >= sizeof p->name) n = sizeof p->name - 1;
    memcpy(p->name, name, n);
    p->arg = arg;
    p->mmap_next = USER_MMAP_BASE;
    p->fault = -1;
    sem_init(&p->done, 0);

    if (!map_pages(p, USER_CODE_BASE, ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE, code, size, VMM_USER | VMM_EXEC) ||
        !map_pages(p, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE, USER_STACK_PAGES, NULL, 0,
                   VMM_USER | VMM_WRITE)) {
        process_free(p);
        return NULL;
    }
    p->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
    return p;
}

// The process's thread: move into its address space and drop to ring 3.
static void process_main(void *arg) {
    struct process *p = arg;
    cli(); // the scheduler reloads CR3 from t->cr3: change both at once
    struct thread *t = thread_current();
    t->proc = p;
    t->cr3 = p->space.pml4;
    write_cr3(t->cr3);
    enter_user(USER_CODE_BASE, USER_STACK_TOP, p->arg);
}

bool process_start(struct process *p) { return thread_create(p->name, process_main, p) != NULL; }

void process_wait(struct process *p) { sem_down(&p->done); }

void process_free(struct process *p) {
    if (p->space.pml4) {
        preempt_disable();
        vmm_space_destroy(&p->space);
        preempt_enable();
    }
    kfree(p);
}

void process_exit(void) {
    struct thread *t = thread_current();
    struct process *p = t->proc;
    p->ticks = t->ticks;
    p->switches = t->switches;

    // Back to the kernel's page tables, then free the process's.
    cli();
    t->proc = NULL;
    t->cr3 = kernel_space.pml4;
    write_cr3(t->cr3);
    sti();
    preempt_disable();
    vmm_space_destroy(&p->space);
    preempt_enable();

    // From sem_up on, process_wait() may free `p`. With interrupts off, it
    // can't run until this thread is gone.
    cli();
    sem_up(&p->done);
    thread_exit();
}

void process_output(struct process *p, const char *s, size_t n) {
    if (p->out == NULL) {
        kwrite(s, n);
        return;
    }
    size_t room = p->out_size - 1 - p->out_len;
    if (n > room) n = room;
    memcpy(p->out + p->out_len, s, n);
    p->out_len += n;
    p->out[p->out_len] = '\0';
}

#define PF_PRESENT (1u << 0)
#define PF_WRITE   (1u << 1)
#define PF_FETCH   (1u << 4)

void process_fault(struct interrupt_frame *f) {
    uint64_t cr2 = read_cr2();
    struct process *p = process_current();
    if (p == NULL) panic("exception %lu in user mode, but no process is running", f->vector);
    p->fault = (int)f->vector;
    p->fault_addr = f->vector == 14 ? cr2 : 0;
    p->fault_error = f->error_code;
    p->fault_rip = f->rip;
    p->status = -1;

    char msg[160];
    if (f->vector == 14)
        ksnprintf(msg, sizeof msg, "process %lu (%s) killed: page fault at %p (%s, %s), rip %p\n", p->pid, p->name,
                  (void *)cr2, f->error_code & PF_FETCH ? "fetch" : f->error_code & PF_WRITE ? "write" : "read",
                  f->error_code & PF_PRESENT ? "protection violation" : "not mapped", (void *)f->rip);
    else
        ksnprintf(msg, sizeof msg, "process %lu (%s) killed: %s (exception %lu, error 0x%lx), rip %p\n", p->pid,
                  p->name, exception_name(f->vector), f->vector, f->error_code, (void *)f->rip);
    process_output(p, msg, strlen(msg));

    // The fault came from user mode, so the kernel was in the middle of
    // nothing: this thread can go on as if it had called exit().
    sti();
    process_exit();
}

// ---------------------------------------------------------------------------
// User memory
// ---------------------------------------------------------------------------

bool user_range_ok(uint64_t addr, size_t n, bool write) {
    if (n == 0) return true;
    if (addr >= USER_END || n > USER_END - addr) return false;
    struct process *p = process_current();
    if (p == NULL) return false;
    // One thread per process: nothing can unmap these pages while the
    // system call that checked them runs.
    for (uint64_t page = ALIGN_DOWN(addr, PAGE_SIZE); page < addr + n; page += PAGE_SIZE)
        if (!vmm_user_accessible(&p->space, page, write)) return false;
    return true;
}

// With SMAP on, the kernel can only touch user pages while RFLAGS.AC is set.
static inline void user_access_begin(void) {
    if (smap_enabled) __asm__ volatile("stac" ::: "memory");
}
static inline void user_access_end(void) {
    if (smap_enabled) __asm__ volatile("clac" ::: "memory");
}

int copy_from_user(void *dst, uint64_t user_src, size_t n) {
    if (!user_range_ok(user_src, n, false)) return -EFAULT;
    user_access_begin();
    memcpy(dst, (const void *)user_src, n);
    user_access_end();
    return 0;
}

int copy_to_user(uint64_t user_dst, const void *src, size_t n) {
    if (!user_range_ok(user_dst, n, true)) return -EFAULT;
    user_access_begin();
    memcpy((void *)user_dst, src, n);
    user_access_end();
    return 0;
}

// ---------------------------------------------------------------------------
// The built-in programs (user_programs.S)
// ---------------------------------------------------------------------------

extern const char user_hello[], user_hello_end[];
extern const char user_kread[], user_kread_end[];
extern const char user_priv[], user_priv_end[];
extern const char user_spin[], user_spin_end[];

static const struct program {
    const char *name;
    const char *start, *end;
    const char *what;
} programs[] = {
    {"hello", user_hello, user_hello_end, "print a greeting, then test every system call"},
    {"kread", user_kread, user_kread_end, "read a kernel address (gets killed)"},
    {"priv", user_priv, user_priv_end, "run cli, a privileged instruction (gets killed)"},
    {"spin", user_spin, user_spin_end, "spin in ring 3 for a second without a system call"},
};

#define PROGRAMS (sizeof programs / sizeof programs[0])

static const struct program *find_program(const char *name) {
    for (size_t i = 0; i < PROGRAMS; i++)
        if (strcmp(programs[i].name, name) == 0) return &programs[i];
    return NULL;
}

// TSC ticks in `ms` milliseconds: what `spin` takes as its argument.
static uint64_t tsc_ticks(uint64_t ms) { return time_get_info().tsc_hz * ms / 1000; }

static struct process *spawn(const char *name, uint64_t arg, char *out, size_t out_size) {
    const struct program *prog = find_program(name);
    struct process *p = process_create(name, prog->start, (size_t)(prog->end - prog->start), arg);
    if (p == NULL) return NULL;
    if (out) {
        p->out = out;
        p->out_size = out_size;
        out[0] = '\0';
    }
    if (!process_start(p)) {
        process_free(p);
        return NULL;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Boot self-test
// ---------------------------------------------------------------------------

#define TEST_SPINNERS 2
#define TEST_SPIN_MS  50

// Pages that are neither free nor in use by the heap or the page tables:
// what processes hold. Back where it started once they are all gone.
static uint64_t pages_held(void) {
    return pmm_get_stats().total_pages - pmm_get_stats().free_pages - kheap_get_stats().heap_bytes / PAGE_SIZE -
           vmm_get_stats().table_pages;
}

static void expect_killed(const char *name, int vector, uint64_t error, uint64_t addr) {
    static char out[256];
    struct process *p = spawn(name, 0, out, sizeof out);
    if (p == NULL) panic("user self-test: no memory");
    process_wait(p);
    if (p->fault != vector)
        panic("user self-test: %s ended with status %d, fault %d; expected to be killed by exception %d (output: %s)",
              name, p->status, p->fault, vector, out);
    if (p->fault_error != error || p->fault_addr != addr || p->fault_rip < USER_CODE_BASE ||
        p->fault_rip >= USER_CODE_BASE + PAGE_SIZE)
        panic("user self-test: %s was killed with error 0x%lx at %p, rip %p", name, p->fault_error,
              (void *)p->fault_addr, (void *)p->fault_rip);
    char want[64];
    ksnprintf(want, sizeof want, "process %lu (%s) killed: ", p->pid, name);
    if (memcmp(out, want, strlen(want)) != 0) panic("user self-test: %s was killed, but reported \"%s\"", name, out);
    process_free(p);
}

void process_self_test(char *line, size_t size) {
    uint64_t held0 = pages_held();
    uint64_t allocs0 = kheap_get_stats().allocations;
    static char out[256];

    // Every system call, from ring 3.
    struct process *p = spawn("hello", 0, out, sizeof out);
    if (p == NULL) panic("user self-test: no memory");
    process_wait(p);
    if (p->fault != -1)
        panic("user self-test: hello was killed by exception %d at rip %p (output: %s)", p->fault,
              (void *)p->fault_rip, out);
    if (p->status != 0) panic("user self-test: hello failed its check %d (output: %s)", p->status, out);
    if (strcmp(out, "Hello from ring 3!\n") != 0) panic("user self-test: hello wrote \"%s\"", out);
    process_free(p);

    // A user read of a mapped kernel page: present, user mode, read.
    expect_killed("kread", 14, PF_PRESENT | 4, 0xffffffff80000000);
    expect_killed("priv", 13, 0, 0);

    // Processes that never make a system call still get preempted, and
    // each keeps its own memory at the same addresses.
    struct process *spin[TEST_SPINNERS];
    for (int i = 0; i < TEST_SPINNERS; i++) {
        spin[i] = spawn("spin", tsc_ticks(TEST_SPIN_MS), NULL, 0);
        if (spin[i] == NULL) panic("user self-test: no memory");
    }
    uint64_t least = UINT64_MAX;
    for (int i = 0; i < TEST_SPINNERS; i++) {
        process_wait(spin[i]);
        if (spin[i]->fault != -1 || spin[i]->status != 0)
            panic("user self-test: spinner %d ended with status %d, fault %d", i, spin[i]->status, spin[i]->fault);
        if (spin[i]->switches < least) least = spin[i]->switches;
        process_free(spin[i]);
    }
    if (least < 2) panic("user self-test: a spinner got the CPU only %lu times", least);

    uint64_t held = pages_held();
    if (held != held0) panic("user self-test: %lu pages held before, %lu after", held0, held);
    uint64_t allocs = kheap_get_stats().allocations;
    if (allocs != allocs0) panic("user self-test: %lu heap allocations before, %lu after", allocs0, allocs);

    ksnprintf(line, size,
              "user mode self-test passed (syscalls checked from ring 3; kernel read, cli killed; "
              "%d spinners preempted, %lu+ turns each)",
              TEST_SPINNERS, least);
}

// ---------------------------------------------------------------------------
// Shell command
// ---------------------------------------------------------------------------

#define SHELL_SPIN_MS 1000

SHELL_COMMAND(user, "<program>", "run a built-in program in user mode (`user` lists them)");
static void cmd_user(int argc, char **argv) {
    const struct program *prog = argc == 2 ? find_program(argv[1]) : NULL;
    if (prog == NULL) {
        if (argc == 2) kprintf("user: no program called %s\n", argv[1]);
        kprintf("usage: user <program>\n");
        for (size_t i = 0; i < PROGRAMS; i++) kprintf("  %-6s %s\n", programs[i].name, programs[i].what);
        return;
    }
    struct process *p = spawn(prog->name, strcmp(prog->name, "spin") == 0 ? tsc_ticks(SHELL_SPIN_MS) : 0, NULL, 0);
    if (p == NULL) {
        kprintf("user: out of memory\n");
        return;
    }
    uint64_t pid = p->pid;
    process_wait(p);
    if (p->fault == -1) kprintf("process %lu (%s) exited with status %d\n", pid, prog->name, p->status);
    process_free(p);
}
