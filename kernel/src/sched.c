// Kernel threads and the round-robin scheduler. See sched.h.
//
// All of the scheduler's state is touched with interrupts off: from the
// timer tick (an interrupt gate, so IF is clear) or between cli and the
// restore of the caller's flags. On one CPU that is all the locking needed.

#include <arch.h>
#include <cpu.h>
#include <kheap.h>
#include <kprintf.h>
#include <sched.h>
#include <shell.h>
#include <string.h>
#include <sync.h>
#include <time.h>
#include <vmm.h>

#define STACK_CANARY 0x57ac4ca7a12ee5edull // at the bottom of every thread stack

// switch.S
void switch_context(uint64_t *save_rsp, uint64_t new_rsp);
void thread_start(void);
__attribute__((noreturn)) void thread_main(void (*entry)(void *), void *arg);

static bool started;
static struct thread boot_thread; // kmain, on Limine's stack
static struct thread *current, *idle;
static struct thread *run_head, *run_tail; // ready threads, FIFO
static struct thread *sleepers;            // BLOCKED in thread_sleep or sched_block_until, unsorted
static struct thread *all_threads;
static struct thread *zombie;              // dead, to free after the switch away from it
static size_t thread_count;
static uint64_t next_id;
static unsigned slice_left; // ticks left in the running thread's quantum

static volatile unsigned preempt_count;
static volatile bool need_resched; // a switch was due while preemption was off

static uint64_t irq_save(void) {
    uint64_t flags = read_rflags();
    cli();
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & RFLAGS_IF) sti();
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

static void check_stack(struct thread *t) {
    if (t->stack && *(uint64_t *)t->stack != STACK_CANARY)
        panic("thread %lu (%s) overflowed its %u-byte stack", t->id, t->name, THREAD_STACK_SIZE);
}

// Free the thread that just died, now that its stack is not in use. Runs on
// the thread that was switched to, still with interrupts off. Nobody was in
// the heap when the switch happened: switches never happen with preemption
// disabled, and the heap disables it.
static void finish_switch(void) {
    struct thread *z = zombie;
    if (z == NULL) return;
    zombie = NULL;
    for (struct thread **p = &all_threads; *p; p = &(*p)->all_next) {
        if (*p == z) {
            *p = z->all_next;
            break;
        }
    }
    thread_count--;
    kfree(z->stack);
    if (z != &boot_thread) kfree(z);
}

// Put the running thread where its state says it goes (back of the run
// queue if it is still RUNNING), then run the first ready thread, or the
// idle thread. Interrupts must be off. Returns when the caller next runs.
static void schedule(void) {
    if (preempt_count) panic("schedule: called with preemption disabled (%s)", current->name);
    struct thread *prev = current;
    check_stack(prev);
    if (prev->state == THREAD_RUNNING) {
        if (prev == idle) prev->state = THREAD_READY; // idle is never in the queue
        else enqueue(prev);
    }
    struct thread *next = dequeue();
    if (next == NULL) next = idle;

    slice_left = SCHED_QUANTUM_MS * TIMER_HZ / 1000;
    need_resched = false;
    next->state = THREAD_RUNNING;
    if (next == prev) return;

    if (prev->state == THREAD_DEAD) zombie = prev;
    next->switches++;
    current = next;
    tss_set_rsp0(next->stack_top);
    if (read_cr3() != next->cr3) write_cr3(next->cr3);
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

// A new thread whose first switch_context runs thread_start(entry, arg).
static struct thread *thread_alloc(const char *name, void (*entry)(void *), void *arg) {
    struct thread *t = kcalloc(1, sizeof *t);
    uint8_t *stack = kmalloc(THREAD_STACK_SIZE);
    if (t == NULL || stack == NULL) {
        kfree(t);
        kfree(stack);
        return NULL;
    }
    size_t n = strlen(name);
    if (n >= THREAD_NAME_MAX) n = THREAD_NAME_MAX - 1;
    memcpy(t->name, name, n);
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

    uint64_t flags = irq_save();
    t->id = next_id++;
    t->all_next = all_threads;
    all_threads = t;
    thread_count++;
    irq_restore(flags);
    return t;
}

static void idle_main(void *arg) {
    for (;;) hlt(); // interrupts are on: the next tick wakes it
}

void sched_init(void) {
    uint64_t flags = irq_save();
    boot_thread = (struct thread){.state = THREAD_RUNNING, .stack_top = tss_get_rsp0(), .cr3 = kernel_space.pml4};
    memcpy(boot_thread.name, "main", 5);
    current = &boot_thread;
    all_threads = &boot_thread;
    thread_count = 1;
    next_id = 1;
    irq_restore(flags);

    idle = thread_alloc("idle", idle_main, NULL);
    if (idle == NULL) panic("sched: no memory for the idle thread");
    idle->state = THREAD_READY;

    flags = irq_save();
    slice_left = SCHED_QUANTUM_MS * TIMER_HZ / 1000;
    started = true;
    irq_restore(flags);
}

struct thread *thread_create(const char *name, void (*entry)(void *arg), void *arg) {
    if (!started) panic("thread_create: called before sched_init()");
    struct thread *t = thread_alloc(name, entry, arg);
    if (t == NULL) return NULL;
    uint64_t flags = irq_save();
    enqueue(t);
    irq_restore(flags);
    return t;
}

void thread_exit(void) {
    cli();
    current->state = THREAD_DEAD;
    schedule();
    panic("thread_exit: a dead thread ran again");
}

void thread_yield(void) {
    if (!started) return;
    uint64_t flags = irq_save();
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
    uint64_t flags = irq_save();
    current->wake_ns = clock_monotonic_ns() + ms * NS_PER_MS;
    current->state = THREAD_BLOCKED;
    current->next = sleepers;
    sleepers = current;
    schedule();
    irq_restore(flags);
}

struct thread *thread_current(void) { return current; }

void sched_block(void) {
    if (read_rflags() & RFLAGS_IF) panic("sched_block: called with interrupts on");
    current->state = THREAD_BLOCKED;
    schedule();
}

void sched_block_until(uint64_t wake_ns) {
    if (read_rflags() & RFLAGS_IF) panic("sched_block_until: called with interrupts on");
    current->wake_ns = wake_ns;
    current->timed_block = true;
    current->state = THREAD_BLOCKED;
    current->next = sleepers;
    sleepers = current;
    schedule();
}

void thread_wake(struct thread *t) {
    uint64_t flags = irq_save();
    if (t->state != THREAD_BLOCKED) {
        // Its deadline already made it ready (sched_block_until).
        if (t->timed_block) {
            irq_restore(flags);
            return;
        }
        panic("thread_wake: thread %lu (%s) is not blocked", t->id, t->name);
    }
    if (t->timed_block) {
        for (struct thread **p = &sleepers; *p; p = &(*p)->next) {
            if (*p == t) {
                *p = t->next;
                break;
            }
        }
    }
    enqueue(t);
    irq_restore(flags);
}

size_t sched_thread_count(void) { return thread_count; }

size_t sched_snapshot(struct thread *out, size_t max) {
    uint64_t flags = irq_save();
    size_t n = 0;
    for (struct thread *t = all_threads; t && n < max; t = t->all_next) out[n++] = *t;
    irq_restore(flags);
    return n;
}

void preempt_disable(void) {
    preempt_count++;
    __asm__ volatile("" ::: "memory");
}

void preempt_enable(void) {
    __asm__ volatile("" ::: "memory");
    if (preempt_count == 0) panic("preempt_enable without preempt_disable");
    preempt_count--;
    // A tick wanted to switch meanwhile. With interrupts off (in an
    // interrupt handler, or the caller turned them off) the next tick does.
    if (preempt_count == 0 && need_resched && (read_rflags() & RFLAGS_IF)) thread_yield();
}

void sched_tick(void) {
    if (!started) return;
    current->ticks++;

    if (sleepers) {
        uint64_t now = clock_monotonic_ns();
        for (struct thread **p = &sleepers; *p;) {
            struct thread *t = *p;
            if (t->wake_ns <= now) {
                *p = t->next;
                enqueue(t);
            } else {
                p = &t->next;
            }
        }
    }

    if (slice_left) slice_left--;
    if (current == idle ? run_head == NULL : slice_left > 0) return;
    if (preempt_count) {
        need_resched = true;
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
    if (slept_ns < TEST_SLEEP_MS * NS_PER_MS)
        panic("sched self-test: thread_sleep(%d) returned after %lu us", TEST_SLEEP_MS, slept_ns / 1000);
    uint64_t allocs = kheap_get_stats().allocations;
    if (allocs != allocs0) panic("sched self-test: %lu heap allocations before, %lu after", allocs0, allocs);

    ksnprintf(line, size,
              "scheduler self-test passed (%d threads that never yield: %lu turns, %lu+ each; sleep(%d): %lu ms)",
              TEST_SPINNERS, total, least, TEST_SLEEP_MS, (uint64_t)(slept_ns / NS_PER_MS));
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
    kprintf("  ID  STATE     CPU ms  SWITCHES  NAME\n");
    for (size_t i = n; i-- > 0;) // oldest first
        kprintf("%4lu  %-8s %7lu  %8lu  %s\n", snap[i].id, states[snap[i].state], snap[i].ticks * 1000 / TIMER_HZ,
                snap[i].switches, snap[i].name);
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
