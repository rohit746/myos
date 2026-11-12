// Kernel threads and the round-robin scheduler. See sched.h.
//
// All of the scheduler's state is touched under `sched_lock`, with
// interrupts off: from a timer tick (an interrupt gate, so IF is clear) or
// between cli and the restore of the caller's flags. Each CPU's own fields
// (struct cpu_local: current, idle, zombie) are only touched by that CPU.

#include <arch.h>
#include <cpu.h>
#include <kheap.h>
#include <kprintf.h>
#include <sched.h>
#include <shell.h>
#include <smp.h>
#include <string.h>
#include <sync.h>
#include <time.h>
#include <vmm.h>

#define STACK_CANARY 0x57ac4ca7a12ee5edull // at the bottom of every thread stack
#define QUANTUM_TICKS (SCHED_QUANTUM_MS * TIMER_HZ / 1000)

// switch.S
void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
void thread_start(void);
__attribute__((noreturn)) void thread_main(void (*entry)(void *), void *arg);

static struct raw_spinlock sched_lock;
static bool started;
static struct thread boot_thread; // kmain, on Limine's stack
static struct thread *run_head, *run_tail; // ready threads, FIFO
static struct thread *sleepers;            // BLOCKED in thread_sleep or sched_block_until, unsorted
static struct thread *all_threads;
static size_t thread_count;
static uint64_t next_id;

static uint64_t irq_save(void) {
    uint64_t flags = read_rflags();
    cli();
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & RFLAGS_IF) sti();
}

// Interrupts off, then the lock.
static uint64_t lock_sched(void) {
    uint64_t flags = irq_save();
    raw_spin_lock(&sched_lock);
    return flags;
}

static void unlock_sched(uint64_t flags) {
    raw_spin_unlock(&sched_lock);
    irq_restore(flags);
}

static void enqueue(struct thread *t) {
    t->state = THREAD_READY;
    t->next = NULL;
    if (run_tail) run_tail->next = t;
    else run_head = t;
    run_tail = t;
}

static struct thread *dequeue(void) {
    struct thread *t = run_head;
    if (t) {
        run_head = t->next;
        if (run_head == NULL) run_tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void remove_sleeper(struct thread *t) {
    for (struct thread **p = &sleepers; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            return;
        }
    }
}

static void check_stack(struct thread *t) {
    if (t->stack && *(uint64_t *)t->stack != STACK_CANARY)
        panic("thread %lu (%s) overflowed its %u-byte stack", t->id, t->name, THREAD_STACK_SIZE);
}

// The first thing after a switch, on the thread switched to, still with
// interrupts off and sched_lock held (the switching CPU took it): let go of
// the lock, and free the thread that just died on this CPU, now that its
// stack is not in use. The heap is not touched under sched_lock: a thread
// holding the heap's lock may be waiting for sched_lock in a timer tick.
static void finish_switch(void) {
    struct cpu_local *c = this_cpu();
    struct thread *z = c->zombie;
    c->zombie = NULL;
    if (z) {
        for (struct thread **p = &all_threads; *p; p = &(*p)->all_next) {
            if (*p == z) {
                *p = z->all_next;
                break;
            }
        }
    }
    raw_spin_unlock(&sched_lock);
    if (z) {
        kfree(z->stack);
        if (z != &boot_thread) kfree(z);
        // Only now: whoever waits for the count to drop expects the memory back.
        __atomic_sub_fetch(&thread_count, 1, __ATOMIC_RELEASE);
    }
}

// Put the running thread where its state says it goes (back of the run
// queue if it is still RUNNING), then run the first ready thread, or this
// CPU's idle thread. Interrupts must be off and sched_lock held; it is let
// go before this returns, when the caller next runs (maybe on another CPU).
static void schedule(void) {
    struct cpu_local *c = this_cpu();
    struct thread *prev = c->current;
    if (prev->preempt_count) panic("schedule: called with preemption disabled (%s)", prev->name);
    check_stack(prev);
    prev->need_resched = false;
    if (prev->state == THREAD_RUNNING) {
        if (prev == c->idle) prev->state = THREAD_READY; // an idle thread is never in the queue
        else enqueue(prev);
    }
    struct thread *next = dequeue();
    if (next == NULL) next = c->idle;

    next->slice_left = QUANTUM_TICKS;
    next->state = THREAD_RUNNING;
    next->cpu = c->index;
    if (next == prev) {
        raw_spin_unlock(&sched_lock);
        return;
    }

    if (prev->state == THREAD_DEAD) c->zombie = prev;
    next->switches++;
    c->current = next;
    tss_set_rsp0(next->stack_top);
    if (c->cr3 != next->cr3) {
        c->cr3 = next->cr3; // before the load: see tlb_shootdown()
        write_cr3(next->cr3);
    }
    switch_context(&prev->rsp, next->rsp);
    finish_switch();
}

// First C code of every new thread, entered from thread_start.
void thread_main(void (*entry)(void *), void *arg) {
    finish_switch();
    sti();
    entry(arg);
    thread_exit();
}

// Give `t` an id and put it on the list of threads.
static void add_thread(struct thread *t) {
    uint64_t flags = lock_sched();
    t->id = next_id++;
    t->all_next = all_threads;
    all_threads = t;
    __atomic_add_fetch(&thread_count, 1, __ATOMIC_RELAXED);
    unlock_sched(flags);
}

static void set_name(struct thread *t, const char *name) {
    size_t n = strlen(name);
    if (n >= THREAD_NAME_MAX) n = THREAD_NAME_MAX - 1;
    memcpy(t->name, name, n);
}

// A new thread whose first switch_context runs thread_start(entry, arg).
static struct thread *thread_alloc(const char *name, void (*entry)(void *), void *arg) {
    struct thread *t = kcalloc(1, sizeof *t);
    uint8_t *stack = kmalloc(THREAD_STACK_SIZE);
    if (t == NULL || stack == NULL) {
        kfree(t);
        kfree(stack);
        return NULL;
    }
    set_name(t, name);
    t->stack = stack;
    t->cr3 = kernel_space.pml4;
    *(uint64_t *)stack = STACK_CANARY;
    t->stack_top = ((uint64_t)stack + THREAD_STACK_SIZE) & ~15ull;

    // What switch_context pops: r15, r14, r13, r12, rbx, rbp, return address.
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = 0;                       // keeps thread_start's frame 16-byte aligned
    *--sp = (uint64_t)thread_start;  // ret
    *--sp = 0;                       // rbp: the end of the backtrace
    *--sp = 0;                       // rbx
    *--sp = (uint64_t)entry;         // r12
    *--sp = (uint64_t)arg;           // r13
    *--sp = 0;                       // r14
    *--sp = 0;                       // r15
    t->rsp = (uint64_t)sp;
    add_thread(t);
    return t;
}

static void idle_main(void *arg) {
    for (;;) hlt(); // interrupts are on: the next tick wakes it
}

void sched_init(void) {
    struct cpu_local *c = this_cpu();
    boot_thread = (struct thread){.state = THREAD_RUNNING, .stack_top = tss_get_rsp0(), .cr3 = kernel_space.pml4};
    set_name(&boot_thread, "main");
    next_id = 1;
    add_thread(&boot_thread);
    c->cr3 = kernel_space.pml4;

    // cpus[0]'s idle thread. The other CPUs' are made by smp_init(), and
    // run on the stack each CPU starts on.
    struct thread *idle = thread_alloc("idle0", idle_main, NULL);
    if (idle == NULL) panic("sched: no memory for the idle thread");
    idle->state = THREAD_READY;

    uint64_t flags = irq_save();
    c->idle = idle;
    boot_thread.slice_left = QUANTUM_TICKS;
    c->current = &boot_thread;
    started = true;
    irq_restore(flags);
}

struct thread *sched_create_idle(unsigned cpu) {
    struct thread *t = kcalloc(1, sizeof *t);
    if (t == NULL) return NULL;
    char name[THREAD_NAME_MAX];
    ksnprintf(name, sizeof name, "idle%u", cpu);
    set_name(t, name);
    t->state = THREAD_READY;
    t->cpu = cpu;
    t->cr3 = kernel_space.pml4;
    add_thread(t);
    return t;
}

void sched_ap_enter(void) {
    struct cpu_local *c = this_cpu();
    struct thread *t = c->idle;
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    t->stack_top = rsp & ~15ull; // never used: an idle thread doesn't enter user mode
    tss_set_rsp0(t->stack_top);
    t->slice_left = QUANTUM_TICKS;
    t->state = THREAD_RUNNING;
    t->switches = 1;
    c->current = t;
    sti();
    for (;;) hlt();
}

struct thread *thread_create(const char *name, void (*entry)(void *arg), void *arg) {
    if (!started) panic("thread_create: called before sched_init()");
    struct thread *t = thread_alloc(name, entry, arg);
    if (t == NULL) return NULL;
    uint64_t flags = lock_sched();
    enqueue(t);
    unlock_sched(flags);
    return t;
}

void thread_exit(void) {
    cli();
    raw_spin_lock(&sched_lock);
    thread_current()->state = THREAD_DEAD;
    schedule();
    panic("thread_exit: a dead thread ran again");
}

void thread_yield(void) {
    if (!started) return;
    uint64_t flags = lock_sched();
    schedule();
    irq_restore(flags);
}

void thread_sleep(uint64_t ms) {
    if (!started) {
        sleep_ms(ms);
        return;
    }
    might_sleep("thread_sleep");
    if (ms == 0) {
        thread_yield();
        return;
    }
    uint64_t flags = lock_sched();
    struct thread *t = thread_current();
    t->wake_ns = clock_monotonic_ns() + ms * NS_PER_MS;
    t->state = THREAD_BLOCKED;
    t->next = sleepers;
    sleepers = t;
    schedule();
    irq_restore(flags);
}

struct thread *thread_current(void) {
    struct thread *t;
    __asm__ volatile("movq %%gs:%c1, %0" : "=r"(t) : "i"(CPU_LOCAL_CURRENT));
    return t;
}

void sched_set_cr3(uint64_t cr3) {
    uint64_t flags = irq_save();
    struct cpu_local *c = this_cpu();
    c->current->cr3 = cr3;
    c->cr3 = cr3;
    write_cr3(cr3);
    irq_restore(flags);
}

void sched_block(void) {
    if (read_rflags() & RFLAGS_IF) panic("sched_block: called with interrupts on");
    struct thread *t = thread_current();
    raw_spin_lock(&sched_lock);
    t->blocking = false;
    if (t->wake_pending) { // woken already, by another CPU
        t->wake_pending = false;
        raw_spin_unlock(&sched_lock);
        return;
    }
    t->state = THREAD_BLOCKED;
    schedule();
}

void sched_block_until(uint64_t wake_ns) {
    if (read_rflags() & RFLAGS_IF) panic("sched_block_until: called with interrupts on");
    struct thread *t = thread_current();
    raw_spin_lock(&sched_lock);
    t->blocking = false;
    t->timed_block = true;
    if (t->wake_pending) {
        t->wake_pending = false;
        raw_spin_unlock(&sched_lock);
        return;
    }
    t->wake_ns = wake_ns;
    t->state = THREAD_BLOCKED;
    t->next = sleepers;
    sleepers = t;
    schedule();
}

void thread_wake(struct thread *t) {
    uint64_t flags = lock_sched();
    if (t->state == THREAD_BLOCKED) {
        if (t->timed_block) remove_sleeper(t);
        enqueue(t);
    } else if (t->blocking) {
        t->wake_pending = true; // still on its way to sched_block, on another CPU
    } else if (!t->timed_block) {
        // (Timed: its deadline already made it ready, sched_block_until.)
        raw_spin_unlock(&sched_lock);
        panic("thread_wake: thread %lu (%s) is not blocked", t->id, t->name);
    }
    unlock_sched(flags);
}

size_t sched_thread_count(void) { return __atomic_load_n(&thread_count, __ATOMIC_ACQUIRE); }

size_t sched_snapshot(struct thread *out, size_t max) {
    uint64_t flags = lock_sched();
    size_t n = 0;
    for (struct thread *t = all_threads; t && n < max; t = t->all_next) out[n++] = *t;
    unlock_sched(flags);
    return n;
}

void preempt_disable(void) {
    struct thread *t = thread_current();
    if (t) t->preempt_count++;
    __asm__ volatile("" ::: "memory");
}

void preempt_enable(void) {
    __asm__ volatile("" ::: "memory");
    struct thread *t = thread_current();
    if (t == NULL) return;
    if (t->preempt_count == 0) panic("preempt_enable without preempt_disable");
    t->preempt_count--;
    // A tick wanted to switch meanwhile. With interrupts off (in an
    // interrupt handler, or the caller turned them off) the next tick does.
    if (t->preempt_count == 0 && t->need_resched && (read_rflags() & RFLAGS_IF)) thread_yield();
}

void sched_tick(void) {
    if (!started) return;
    struct cpu_local *c = this_cpu();
    struct thread *t = c->current;
    if (t == NULL) return; // an AP that hasn't joined yet
    t->ticks++;
    if (t->slice_left) t->slice_left--;

    // Only the BSP wakes sleepers, so the other CPUs only need the lock
    // when they may switch.
    bool bsp = c->index == 0;
    bool want = t == c->idle ? __atomic_load_n(&run_head, __ATOMIC_RELAXED) != NULL : t->slice_left == 0;
    if (!want && !(bsp && __atomic_load_n(&sleepers, __ATOMIC_RELAXED))) return;

    raw_spin_lock(&sched_lock);
    if (bsp && sleepers) {
        uint64_t now = clock_monotonic_ns();
        for (struct thread **p = &sleepers; *p;) {
            struct thread *s = *p;
            if (s->wake_ns <= now) {
                *p = s->next;
                enqueue(s);
            } else {
                p = &s->next;
            }
        }
    }
    if (!(t == c->idle ? run_head != NULL : t->slice_left == 0)) {
        raw_spin_unlock(&sched_lock);
        return;
    }
    if (t->preempt_count) {
        t->need_resched = true;
        raw_spin_unlock(&sched_lock);
        return;
    }
    schedule();
}

// ---------------------------------------------------------------------------
// Boot self-test
// ---------------------------------------------------------------------------

#define TEST_SPINNERS 4
#define TEST_SPIN_MS  200
#define TEST_SLEEP_MS 50
#define TEST_TIMEOUT_MS 5000

static volatile int last_spinner;
static volatile uint64_t stints[TEST_SPINNERS]; // times each spinner got the CPU back from another one
static volatile uint64_t spinner_cpus;          // the CPUs spinners ran on (bit i: cpus[i])
static volatile uint64_t slept_ns;

// Spin for TEST_SPIN_MS without ever yielding or sleeping. Only the timer
// tick can take the CPU away.
static void test_spinner(void *arg) {
    int me = (int)(uintptr_t)arg;
    uint64_t end = clock_monotonic_ns() + TEST_SPIN_MS * NS_PER_MS;
    while (clock_monotonic_ns() < end) {
        if (last_spinner != me) {
            last_spinner = me;
            stints[me]++;
        }
        uint64_t cpu = 1ull << thread_current()->cpu;
        if (!(spinner_cpus & cpu)) __atomic_or_fetch(&spinner_cpus, cpu, __ATOMIC_RELAXED);
    }
}

static void test_sleeper(void *arg) {
    uint64_t t0 = clock_monotonic_ns();
    thread_sleep(TEST_SLEEP_MS);
    slept_ns = clock_monotonic_ns() - t0;
}

void sched_self_test(char *line, size_t size) {
    size_t threads0 = sched_thread_count();
    uint64_t allocs0 = kheap_get_stats().allocations;
    last_spinner = -1;
    spinner_cpus = 0;
    slept_ns = 0;
    for (int i = 0; i < TEST_SPINNERS; i++) {
        stints[i] = 0;
        char name[16];
        ksnprintf(name, sizeof name, "spinner%d", i);
        if (thread_create(name, test_spinner, (void *)(uintptr_t)i) == NULL) panic("sched self-test: no memory");
    }
    if (thread_create("sleeper", test_sleeper, NULL) == NULL) panic("sched self-test: no memory");

    // Wait for them all to exit and be freed.
    uint64_t t0 = clock_monotonic_ns();
    while (sched_thread_count() > threads0) {
        if (clock_monotonic_ns() - t0 > TEST_TIMEOUT_MS * NS_PER_MS)
            panic("sched self-test: %lu threads still there after %d ms", sched_thread_count() - threads0,
                  TEST_TIMEOUT_MS);
        thread_sleep(10);
    }

    uint64_t total = 0, least = UINT64_MAX;
    for (int i = 0; i < TEST_SPINNERS; i++) {
        total += stints[i];
        if (stints[i] < least) least = stints[i];
    }
    // Each spinner wants the CPU for the whole TEST_SPIN_MS, so with
    // SCHED_QUANTUM_MS slices each must have had it more than once.
    if (least < 2)
        panic("sched self-test: a spinner got the CPU only %lu times (%lu, %lu, %lu, %lu)", least, stints[0],
              stints[1], stints[2], stints[3]);
    // CPU-bound threads spread over the CPUs (all of them, when there are
    // as many spinners).
    unsigned cpus = smp_cpu_count(), spread = __builtin_popcountll(spinner_cpus);
    if (spread < (cpus < TEST_SPINNERS ? cpus : TEST_SPINNERS))
        panic("sched self-test: %d spinners ran on %u of %u CPUs (%lx)", TEST_SPINNERS, spread, cpus, spinner_cpus);
    if (slept_ns < TEST_SLEEP_MS * NS_PER_MS)
        panic("sched self-test: thread_sleep(%d) returned after %lu us", TEST_SLEEP_MS, slept_ns / 1000);
    uint64_t allocs = kheap_get_stats().allocations;
    if (allocs != allocs0) panic("sched self-test: %lu heap allocations before, %lu after", allocs0, allocs);

    ksnprintf(line, size,
              "scheduler self-test passed (%d threads that never yield: %lu turns, %lu+ each, spread over %u "
              "CPUs; sleep(%d): %lu ms)",
              TEST_SPINNERS, total, least, spread, TEST_SLEEP_MS, (uint64_t)(slept_ns / NS_PER_MS));
}

// ---------------------------------------------------------------------------
// Shell commands
// ---------------------------------------------------------------------------

#define PS_MAX 64

SHELL_COMMAND(ps, "", "list the threads");
static void cmd_ps(int argc, char **argv) {
    static struct thread snap[PS_MAX]; // too big for a 16 KiB stack
    static const char *const states[] = {"running", "ready", "blocked", "dead"};
    size_t n = sched_snapshot(snap, PS_MAX);
    kprintf("  ID  STATE     CPU ms  SWITCHES  CPU  NAME\n"); // CPU: the one it is on, or was on last
    for (size_t i = n; i-- > 0;) // oldest first
        kprintf("%4lu  %-8s %7lu  %8lu  %3u  %s\n", snap[i].id, states[snap[i].state], snap[i].ticks * 1000 / TIMER_HZ,
                snap[i].switches, snap[i].cpu, snap[i].name);
    if (sched_thread_count() > n) kprintf("  ... and %lu more\n", sched_thread_count() - n);
}

// `threads`: printers that take turns printing, and a spinner that never
// yields, all while the shell waits for them.
#define DEMO_PRINTERS 3
#define DEMO_LINES    5
#define DEMO_SPIN_MS  300

static volatile unsigned demo_running;

static void demo_printer(void *arg) {
    int n = (int)(uintptr_t)arg;
    for (int i = 1; i <= DEMO_LINES; i++) {
        kprintf("  printer %d: line %d of %d\n", n, i, DEMO_LINES);
        thread_sleep(50 + 20 * n);
    }
    __atomic_fetch_sub(&demo_running, 1, __ATOMIC_RELAXED);
}

static void demo_spinner(void *arg) {
    uint64_t end = clock_monotonic_ns() + DEMO_SPIN_MS * NS_PER_MS;
    while (clock_monotonic_ns() < end) pause();
    kprintf("  spinner: spun %d ms without yielding; it was put back on the CPU %lu times\n", DEMO_SPIN_MS,
            thread_current()->switches);
    __atomic_fetch_sub(&demo_running, 1, __ATOMIC_RELAXED);
}

SHELL_COMMAND(threads, "", "run threads that print in turn, and one that never yields");
static void cmd_threads(int argc, char **argv) {
    demo_running = DEMO_PRINTERS + 1;
    unsigned started_now = 0;
    for (int i = 1; i <= DEMO_PRINTERS; i++) {
        char name[16];
        ksnprintf(name, sizeof name, "printer%d", i);
        if (thread_create(name, demo_printer, (void *)(uintptr_t)i)) started_now++;
    }
    if (thread_create("spinner", demo_spinner, NULL)) started_now++;
    if (started_now < DEMO_PRINTERS + 1) {
        kprintf("threads: out of memory\n");
        __atomic_fetch_sub(&demo_running, DEMO_PRINTERS + 1 - started_now, __ATOMIC_RELAXED);
    }
    while (demo_running) thread_sleep(10);
    kprintf("threads: %u threads ran and exited\n", started_now);
}
