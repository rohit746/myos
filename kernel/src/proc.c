// User processes. See proc.h.

#include <arch.h>
#include <cpu.h>
#include <elf.h>
#include <errno.h>
#include <kheap.h>
#include <kprintf.h>
#include <memory.h>
#include <pmm.h>
#include <proc.h>
#include <sched.h>
#include <shell.h>
#include <smp.h>
#include <string.h>
#include <syscall.h>
#include <time.h>
#include <vfs.h>
#include <vmm.h>

// syscall.S
__attribute__((noreturn)) void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg);

static uint64_t next_pid = 1;

// Every process's parent, children, sibling, zombie and orphan. Never held
// together with the VFS lock: files are closed before or after.
static struct mutex tree_lock = {.guard = SPINLOCK_INIT(NULL), .dep = {.name = "process tree"}};

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
        uint64_t page = pmm_alloc_page();
        if (page) {
            uint8_t *dst = phys_to_virt(page);
            memset(dst, 0, PAGE_SIZE);
            size_t off = i * PAGE_SIZE;
            if (src && off < size) memcpy(dst, (const uint8_t *)src + off, size - off < PAGE_SIZE ? size - off : PAGE_SIZE);
            vmm_map(&p->space, virt + off, page, flags);
        }
        if (page == 0) return false;
    }
    return true;
}

_Static_assert(ELF_LOAD_END == USER_MMAP_BASE, "ELF segments go below what mmap hands out");
_Static_assert(USER_CODE_BASE >= ELF_LOAD_MIN, "built-in programs go where ELF segments may");

// A process with an empty address space but for its stack, and no pid yet.
static struct process *process_alloc(const char *name) {
    struct process *p = kcalloc(1, sizeof *p);
    if (p == NULL) return NULL;
    if (!vmm_space_create(&p->space)) {
        kfree(p);
        return NULL;
    }
    size_t n = strlen(name);
    if (n >= sizeof p->name) n = sizeof p->name - 1;
    memcpy(p->name, name, n);
    p->mmap_next = USER_MMAP_BASE;
    p->fault = -1;
    p->user_rsp = USER_STACK_TOP;
    sem_init(&p->done, 0);
    cond_init(&p->child_ended);
    for (int i = 0; i < PROC_MAX_FDS; i++) p->fds[i] = -1;
    if (!map_pages(p, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE, USER_STACK_PAGES, NULL, 0,
                   VMM_USER | VMM_WRITE)) {
        process_free(p);
        return NULL;
    }
    return p;
}

static void assign_pid(struct process *p) { p->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED); }

struct process *process_create(const char *name, const void *code, size_t size, uint64_t arg) {
    struct process *p = process_alloc(name);
    if (p == NULL) return NULL;
    p->arg = arg;
    p->entry = USER_CODE_BASE;
    if (!map_pages(p, USER_CODE_BASE, ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE, code, size, VMM_USER | VMM_EXEC)) {
        process_free(p);
        return NULL;
    }
    assign_pid(p);
    return p;
}

// Copy `n` bytes between `buf` and `virt` in a process that isn't running,
// through the HHDM. The pages must be mapped.
static void hhdm_copy(struct process *p, uint64_t virt, void *buf, size_t n, bool to_process) {
    uint8_t *b = buf;
    while (n > 0) {
        uint64_t phys;
        if (!vmm_virt_to_phys(&p->space, virt, &phys)) panic("process memory at %p is not mapped", (void *)virt);
        size_t chunk = PAGE_SIZE - (virt & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        if (to_process) memcpy(phys_to_virt(phys), b, chunk);
        else memcpy(b, phys_to_virt(phys), chunk);
        virt += chunk;
        b += chunk;
        n -= chunk;
    }
}

static void poke(struct process *p, uint64_t virt, const void *src, size_t n) {
    hhdm_copy(p, virt, (void *)src, n, true);
}

// How many entries `list` has, and the bytes its strings take with their
// NULs. False if that is over `max` bytes (counting a pointer for each).
static bool measure(char *const list[], size_t max, size_t *count, size_t *bytes) {
    size_t n = 0, b = 0;
    for (; list[n]; n++) {
        b += strlen(list[n]) + 1 + sizeof(uint64_t);
        if (b > max) return false;
    }
    *count = n;
    *bytes = b - n * sizeof(uint64_t);
    return true;
}

// Lay out argc, argv, envp, the auxiliary vector and the strings at the top
// of the stack (see proc.h), and point user_rsp at argc.
static int push_args(struct process *p, char *const argv[], char *const envp[], const struct elf_info *elf) {
    size_t argc, envc, argv_bytes, envp_bytes;
    if (!measure(argv, USER_ARGS_MAX, &argc, &argv_bytes) || !measure(envp, USER_ARGS_MAX, &envc, &envp_bytes))
        return -E2BIG;
    uint64_t aux[][2] = {
        {AT_PHDR, elf->phdr},   {AT_PHENT, sizeof(Elf64_Phdr)}, {AT_PHNUM, elf->phnum},
        {AT_PAGESZ, PAGE_SIZE}, {AT_ENTRY, elf->entry},         {AT_NULL, 0},
    };
    size_t aux_skip = elf->phdr ? 0 : 1; // no AT_PHDR when the headers aren't in memory
    size_t aux_words = 2 * (sizeof aux / sizeof aux[0] - aux_skip);
    size_t words = 1 + (argc + 1) + (envc + 1) + aux_words;
    size_t strings = argv_bytes + envp_bytes;
    if (strings + words * sizeof(uint64_t) + 16 > USER_ARGS_MAX) return -E2BIG;

    uint64_t *w = kmalloc(words * sizeof *w);
    if (w == NULL) return -ENOMEM;
    uint64_t str = USER_STACK_TOP - strings; // the strings, from here to the top
    uint64_t sp = ALIGN_DOWN(str - words * sizeof *w, 16);
    size_t i = 0;
    w[i++] = argc;
    char *const *lists[] = {argv, envp};
    for (size_t l = 0; l < 2; l++) {
        for (char *const *s = lists[l]; *s; s++) {
            size_t n = strlen(*s) + 1;
            poke(p, str, *s, n);
            w[i++] = str;
            str += n;
        }
        w[i++] = 0;
    }
    memcpy(&w[i], aux[aux_skip], aux_words * sizeof *w);
    poke(p, sp, w, words * sizeof *w);
    kfree(w);
    p->user_rsp = sp;
    return 0;
}

int process_create_elf(const char *name, const void *image, size_t size, char *const argv[], char *const envp[],
                       struct process **out) {
    struct process *p = process_alloc(name);
    if (p == NULL) return -ENOMEM;
    struct elf_info elf;
    int err = elf_load(&p->space, image, size, &elf);
    if (err == 0) err = push_args(p, argv, envp, &elf);
    if (err) {
        process_free(p);
        return err;
    }
    p->entry = elf.entry;
    assign_pid(p);
    *out = p;
    return 0;
}

int process_exec(const char *path, char *const argv[], char *const envp[], struct process **out) {
    struct vfs_stat st;
    int err = vfs_stat(path, &st);
    if (err) return err;
    if (st.type != VNODE_FILE) return -EACCES;
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return fd;
    // The file can't change size meanwhile: nothing else writes to it during
    // boot or while the shell waits for the program.
    uint8_t *image = kmalloc(st.size ? st.size : 1);
    size_t got = 0;
    err = image ? 0 : -ENOMEM;
    while (err == 0 && got < st.size) {
        int64_t n = vfs_read(fd, image + got, st.size - got);
        if (n < 0) err = (int)n;
        else if (n == 0) break;
        else got += (size_t)n;
    }
    vfs_close(fd);
    if (err == 0) {
        const char *name = path;
        for (const char *c = path; *c; c++)
            if (*c == '/' && c[1]) name = c + 1;
        err = process_create_elf(name, image, got, argv, envp, out);
    }
    kfree(image);
    return err;
}

char *const process_default_env[] = {"PATH=/bin", "HOME=/", NULL};

int process_run(const char *path, char *const argv[], int *status) {
    struct process *p;
    int err = process_exec(path, argv, process_default_env, &p);
    if (err) return err;
    if (!process_start(p)) {
        process_free(p);
        return -ENOMEM;
    }
    process_wait(p);
    *status = p->fault == -1 ? p->status : -1;
    process_free(p);
    return 0;
}

// The process's thread: move into its address space and drop to ring 3.
static void process_main(void *arg) {
    struct process *p = arg;
    cli();
    thread_current()->proc = p;
    sched_set_cr3(p->space.pml4);
    enter_user(p->entry, p->user_rsp, p->arg);
}

bool process_start(struct process *p) {
    // One open file, shared by stdin, stdout and stderr. If it can't be
    // opened, the process runs without them (its reads and writes fail).
    if (p->parent == NULL && p->fds[0] < 0) {
        int console = vfs_open("/dev/console", O_RDWR);
        for (int i = 0; console >= 0 && i < 3; i++)
            if (i == 0 || vfs_hold(console) == 0) p->fds[i] = console;
    }
    return thread_create(p->name, process_main, p) != NULL;
}

void process_wait(struct process *p) { sem_down(&p->done); }

static void close_files(struct process *p) {
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (p->fds[i] >= 0) vfs_close(p->fds[i]);
        p->fds[i] = -1;
    }
}

void process_free(struct process *p) {
    close_files(p);
    if (p->space.pml4) vmm_space_destroy(&p->space);
    kfree(p);
}

int64_t process_spawn(struct process *parent, const char *path, char *const argv[], char *const envp[]) {
    struct process *c;
    int err = process_exec(path, argv, envp, &c);
    if (err) return err;
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (parent->fds[i] >= 0 && vfs_hold(parent->fds[i]) == 0) c->fds[i] = parent->fds[i];
    int64_t pid = (int64_t)c->pid; // once it runs, it may end and be reaped at any time
    mutex_lock(&tree_lock);
    c->parent = parent;
    c->sibling = parent->children;
    parent->children = c;
    mutex_unlock(&tree_lock);
    if (process_start(c)) return pid;
    mutex_lock(&tree_lock);
    parent->children = c->sibling; // still the newest: only `parent` adds children, and it is here
    mutex_unlock(&tree_lock);
    process_free(c);
    return -ENOMEM;
}

int process_signal(int vector) {
    switch (vector) {
    case 0: case 16: case 19: return 8; // divide error, x87 or SIMD exception: SIGFPE
    case 1: case 3: return 5;           // debug, breakpoint: SIGTRAP
    case 6: return 4;                   // invalid opcode: SIGILL
    case 17: return 7;                  // alignment check: SIGBUS
    default: return 11;                 // page fault, general protection, ...: SIGSEGV
    }
}

int64_t process_waitpid(struct process *p, int64_t pid, int *status, int options) {
    if (pid == 0 || pid < -1 || (options & ~WNOHANG)) return -EINVAL; // no process groups
    mutex_lock(&tree_lock);
    for (;;) {
        bool any = false;
        struct process **link = NULL;
        for (struct process **c = &p->children; *c; c = &(*c)->sibling) {
            if (pid != -1 && (int64_t)(*c)->pid != pid) continue;
            any = true;
            if ((*c)->zombie) {
                link = c;
                break;
            }
        }
        if (!any || (link == NULL && (options & WNOHANG))) {
            mutex_unlock(&tree_lock);
            return any ? 0 : -ECHILD;
        }
        if (link) {
            struct process *z = *link;
            *link = z->sibling;
            mutex_unlock(&tree_lock);
            *status = z->fault == -1 ? (z->status & 0xff) << 8 : process_signal(z->fault);
            int64_t reaped = (int64_t)z->pid;
            process_free(z);
            return reaped;
        }
        cond_wait(&p->child_ended, &tree_lock);
    }
}

uint64_t process_parent_pid(struct process *p) {
    mutex_lock(&tree_lock);
    uint64_t pid = p->parent ? p->parent->pid : 0;
    mutex_unlock(&tree_lock);
    return pid;
}

void process_exit(void) {
    struct thread *t = thread_current();
    struct process *p = t->proc;
    p->ticks = t->ticks;
    p->switches = t->switches;
    close_files(p);

    // Back to the kernel's page tables, then free the process's. No other
    // CPU has them loaded: the process has just this one thread.
    t->proc = NULL;
    sched_set_cr3(kernel_space.pml4);
    vmm_space_destroy(&p->space);

    // Its children are orphans now. The ones that already ended get freed
    // here, since nobody will reap them; the others free themselves.
    struct process *dead = NULL;
    mutex_lock(&tree_lock);
    while (p->children) {
        struct process *c = p->children;
        p->children = c->sibling;
        c->parent = NULL;
        c->orphan = true;
        if (c->zombie) {
            c->sibling = dead;
            dead = c;
        }
    }
    p->zombie = true;
    struct process *parent = p->parent;
    bool orphan = p->orphan;
    if (parent) cond_broadcast(&parent->child_ended);
    mutex_unlock(&tree_lock);
    // From here on a parent may reap `p` and free it: don't touch it.
    while (dead) {
        struct process *next = dead->sibling;
        process_free(dead);
        dead = next;
    }
    if (orphan) process_free(p);

    if (parent == NULL && !orphan) {
        // From sem_up on, process_wait() may free `p` (on another CPU, at
        // once): nothing below touches it.
        cli();
        sem_up(&p->done);
    }
    thread_exit();
}

void process_output(struct process *p, const char *s, size_t n) {
    if (p->out == NULL) {
        if (p->fds[2] < 0 || vfs_write(p->fds[2], s, n) < 0) kwrite(s, n);
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
    // The fault came from user mode, so the kernel was in the middle of
    // nothing: this thread can go on as if it had called exit().
    sti();
    process_output(p, msg, strlen(msg));
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

int64_t copy_string_from_user(char *dst, uint64_t user_src, size_t size) {
    size_t i = 0;
    while (i < size) {
        // Up to the end of this page, which must be readable.
        uint64_t at = user_src + i;
        if (at < user_src || !user_range_ok(at, 1, false)) return -EFAULT;
        size_t chunk = PAGE_SIZE - (at & (PAGE_SIZE - 1));
        if (chunk > size - i) chunk = size - i;
        user_access_begin();
        for (size_t end = i + chunk; i < end; i++) {
            dst[i] = *(const volatile char *)(user_src + i);
            if (dst[i] == '\0') {
                user_access_end();
                return (int64_t)i;
            }
        }
        user_access_end();
    }
    return -ENAMETOOLONG;
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

// A built-in program's process, not started yet.
static struct process *create(const char *name, uint64_t arg) {
    const struct program *prog = find_program(name);
    return process_create(name, prog->start, (size_t)(prog->end - prog->start), arg);
}

static struct process *spawn(const char *name, uint64_t arg, char *out, size_t out_size) {
    struct process *p = create(name, arg);
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

// An ELF image of the `hello` program: one read-only, executable segment
// with the headers at its start (file offset 0) and the code at TEST_ENTRY,
// and one read/write segment that is all .bss.
#define TEST_VADDR     ((uint64_t)0x400000)
#define TEST_ENTRY     (TEST_VADDR + PAGE_SIZE)
#define TEST_BSS_VADDR ((uint64_t)0x600000)
#define TEST_BSS_SIZE  (3 * PAGE_SIZE + 5)

struct test_headers {
    Elf64_Ehdr h;
    Elf64_Phdr ph[2];
};

static size_t test_elf_size(void) { return PAGE_SIZE + (size_t)(user_hello_end - user_hello); }

static void test_elf(uint8_t *image) {
    size_t size = test_elf_size();
    struct test_headers t = {
        .h = {.e_ident = {0x7f, 'E', 'L', 'F', ELFCLASS64, ELFDATA2LSB, EV_CURRENT},
              .e_type = ET_EXEC,
              .e_machine = EM_X86_64,
              .e_version = EV_CURRENT,
              .e_entry = TEST_ENTRY,
              .e_phoff = sizeof(Elf64_Ehdr),
              .e_ehsize = sizeof(Elf64_Ehdr),
              .e_phentsize = sizeof(Elf64_Phdr),
              .e_phnum = 2},
        .ph = {{PT_LOAD, PF_R | PF_X, 0, TEST_VADDR, TEST_VADDR, size, size, PAGE_SIZE},
               {PT_LOAD, PF_R | PF_W, 0, TEST_BSS_VADDR, TEST_BSS_VADDR, 0, TEST_BSS_SIZE, PAGE_SIZE}},
    };
    memset(image, 0, size);
    memcpy(image, &t, sizeof t);
    memcpy(image + PAGE_SIZE, user_hello, (size_t)(user_hello_end - user_hello));
}

// Images elf_load must refuse: each is the good one with one thing broken.
static void break_elf(int which, struct test_headers *t, size_t *size) {
    Elf64_Phdr *code = &t->ph[0], *bss = &t->ph[1];
    switch (which) {
    case 0: t->h.e_ident[1] = 'X'; break;                     // not ELF
    case 1: t->h.e_ident[4] = 1; break;                       // 32-bit
    case 2: t->h.e_type = ET_DYN; break;                      // position-independent
    case 3: t->h.e_machine = 3; break;                        // i386
    case 4: t->h.e_phnum = 0; break;                          // no program headers
    case 5: t->h.e_phoff = *size - 8; break;                  // headers past the end of the file
    case 6: *size = sizeof t->h - 1; break;                   // truncated header
    case 7: code->p_filesz = code->p_memsz + 1; break;        // more file than memory
    case 8: code->p_offset = PAGE_SIZE; break;                // bytes past the end of the file
    case 9: bss->p_vaddr = 0xffffffff80000000; break;         // in the kernel half
    case 10: bss->p_vaddr = USER_MMAP_BASE - PAGE_SIZE; break; // runs into the mmap area
    case 11: bss->p_memsz = UINT64_MAX - PAGE_SIZE; break;    // wraps around
    case 12: bss->p_vaddr = PAGE_SIZE; break;                 // below ELF_LOAD_MIN
    case 13: bss->p_vaddr = TEST_VADDR + *size; break;        // shares a page with the code
    case 14: t->h.e_entry = TEST_BSS_VADDR; break;            // entry not in an executable segment
    case 15: bss->p_type = PT_INTERP; break;                  // wants a dynamic linker
    case 16: code->p_type = PT_NULL; break;                   // the entry isn't loaded at all
    }
}

#define BROKEN_ELFS 17

static uint64_t peek64(struct process *p, uint64_t virt) {
    uint64_t v;
    hhdm_copy(p, virt, &v, sizeof v, false);
    return v;
}

static void peek_string(struct process *p, uint64_t virt, char *buf, size_t size) {
    for (size_t i = 0; i < size; i++) {
        hhdm_copy(p, virt + i, &buf[i], 1, false);
        if (buf[i] == '\0') return;
    }
    buf[size - 1] = '\0';
}

// Load the test image, check its memory and stack before it runs, run it;
// then the broken images.
static void elf_self_test(void) {
    static char out[256];
    size_t size = test_elf_size();
    uint8_t *image = kmalloc(size), *bad = kmalloc(size);
    if (image == NULL || bad == NULL) panic("ELF self-test: no memory");
    test_elf(image);

    char *argv[] = {"hello", "two words", NULL}, *envp[] = {"A=1", NULL};
    struct process *p;
    int err = process_create_elf("hello.elf", image, size, argv, envp, &p);
    if (err) panic("ELF self-test: the test image did not load: %s", strerror(err));

    // Code read-only, .bss writable and zero to its last byte, nothing past it.
    if (!vmm_user_accessible(&p->space, TEST_ENTRY, false) || vmm_user_accessible(&p->space, TEST_ENTRY, true))
        panic("ELF self-test: the code segment is not mapped read-only");
    for (uint64_t v = TEST_BSS_VADDR; v < TEST_BSS_VADDR + TEST_BSS_SIZE; v += PAGE_SIZE)
        if (!vmm_user_accessible(&p->space, v, true)) panic("ELF self-test: .bss page %p is not writable", (void *)v);
    if (peek64(p, ALIGN_DOWN(TEST_BSS_VADDR + TEST_BSS_SIZE - 1, 8)) != 0) panic("ELF self-test: .bss is not zero");
    uint64_t past;
    if (vmm_virt_to_phys(&p->space, ALIGN_UP(TEST_BSS_VADDR + TEST_BSS_SIZE, PAGE_SIZE), &past))
        panic("ELF self-test: a page past .bss is mapped");

    // The stack: argc, argv, envp, the auxiliary vector.
    uint64_t sp = p->user_rsp;
    char s[32];
    if (sp % 16 != 0) panic("ELF self-test: the stack pointer %p is not 16-byte aligned", (void *)sp);
    if (peek64(p, sp) != 2) panic("ELF self-test: argc is %lu, not 2", peek64(p, sp));
    peek_string(p, peek64(p, sp + 16), s, sizeof s);
    if (strcmp(s, "two words") != 0 || peek64(p, sp + 24) != 0) panic("ELF self-test: argv[1] is \"%s\"", s);
    peek_string(p, peek64(p, sp + 32), s, sizeof s);
    if (strcmp(s, "A=1") != 0 || peek64(p, sp + 40) != 0) panic("ELF self-test: envp[0] is \"%s\"", s);
    uint64_t want[][2] = {{AT_PHDR, TEST_VADDR + sizeof(Elf64_Ehdr)}, {AT_PHENT, sizeof(Elf64_Phdr)},
                          {AT_PHNUM, 2}, {AT_PAGESZ, PAGE_SIZE}, {AT_ENTRY, TEST_ENTRY}, {AT_NULL, 0}};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        uint64_t at = sp + 48 + i * 16;
        if (peek64(p, at) != want[i][0] || peek64(p, at + 8) != want[i][1])
            panic("ELF self-test: auxv[%lu] is (%lu, %p), not (%lu, %p)", i, peek64(p, at), (void *)peek64(p, at + 8),
                  want[i][0], (void *)want[i][1]);
    }

    p->out = out;
    p->out_size = sizeof out;
    out[0] = '\0';
    if (!process_start(p)) panic("ELF self-test: no memory");
    process_wait(p);
    if (p->fault != -1 || p->status != 0 || strcmp(out, "Hello from ring 3!\n") != 0)
        panic("ELF self-test: the program ended with status %d, fault %d (output: %s)", p->status, p->fault, out);
    process_free(p);

    for (int i = 0; i < BROKEN_ELFS; i++) {
        memcpy(bad, image, size);
        size_t bad_size = size;
        break_elf(i, (struct test_headers *)bad, &bad_size);
        err = process_create_elf("broken.elf", bad, bad_size, argv, envp, &p);
        if (err != -ENOEXEC) panic("ELF self-test: broken image %d gave %d, not -ENOEXEC", i, err);
    }

    // Arguments that don't fit on the stack.
    char *big = kmalloc(USER_ARGS_MAX);
    if (big == NULL) panic("ELF self-test: no memory");
    memset(big, 'x', USER_ARGS_MAX - 1);
    big[USER_ARGS_MAX - 1] = '\0';
    char *big_argv[] = {"big", big, NULL};
    err = process_create_elf("big.elf", image, size, big_argv, envp, &p);
    if (err != -E2BIG) panic("ELF self-test: too many argument bytes gave %d, not -E2BIG", err);
    kfree(big);
    kfree(bad);
    kfree(image);
}

#define TEST_SPINNERS 2 // more than there are CPUs
#define TEST_SPIN_MS  100

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

    elf_self_test();

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
    // each keeps its own memory at the same addresses. More of them than
    // there are CPUs, all started at once, or each could keep one to itself.
    struct process *spin[MAX_CPUS + TEST_SPINNERS];
    unsigned spinners = smp_cpu_count() + TEST_SPINNERS;
    for (unsigned i = 0; i < spinners; i++) {
        spin[i] = create("spin", tsc_ticks(TEST_SPIN_MS));
        if (spin[i] == NULL) panic("user self-test: no memory");
    }
    for (unsigned i = 0; i < spinners; i++)
        if (!process_start(spin[i])) panic("user self-test: no memory");
    uint64_t least = UINT64_MAX;
    for (unsigned i = 0; i < spinners; i++) {
        process_wait(spin[i]);
        if (spin[i]->fault != -1 || spin[i]->status != 0)
            panic("user self-test: spinner %u ended with status %d, fault %d", i, spin[i]->status, spin[i]->fault);
        if (spin[i]->switches < least) least = spin[i]->switches;
        process_free(spin[i]);
    }
    if (least < 2) panic("user self-test: a spinner got the CPU only %lu times", least);

    uint64_t held = pages_held();
    if (held != held0) panic("user self-test: %lu pages held before, %lu after", held0, held);
    uint64_t allocs = kheap_get_stats().allocations;
    if (allocs != allocs0) panic("user self-test: %lu heap allocations before, %lu after", allocs0, allocs);

    ksnprintf(line, size,
              "user mode self-test passed (ELF loaded with argv/envp/auxv, %d broken ones refused; syscalls "
              "checked from ring 3; kernel read, cli killed; %u spinners preempted, %lu+ turns each)",
              BROKEN_ELFS, spinners, least);
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
