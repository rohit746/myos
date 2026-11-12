#pragma once
// Kernel threads and a preemptive round-robin scheduler, on every CPU.
//
// Every thread has its own kernel stack. A switch saves the callee-saved
// registers on the old thread's stack, stores its stack pointer and loads
// the new one's (switch_context in switch.S). Everything else the old thread
// had in registers was already saved by the C code that called the switch,
// or by the interrupt entry stub when the timer tick preempted it.
//
// A thread that runs a user process has that process's page tables
// (thread.cr3); a switch loads them, or the kernel's for a kernel thread.
//
// Ready threads wait in one FIFO run queue that every CPU takes from, so
// work spreads over the CPUs by itself: a CPU whose thread blocks, yields or
// uses up its slice takes the next one. Each runs for up to SCHED_QUANTUM_MS
// of its CPU's timer ticks before the tick puts it at the back of the queue,
// whether it yields or not. A CPU with nothing to run runs its own idle
// thread, `hlt`, and looks at the queue again on its next tick.
//
// One spinlock, taken with interrupts off, covers the queue, the sleepers
// and every thread's state. A switch holds it from the old thread to the new
// one: the thread switched away from is in the queue already, and no other
// CPU may pick it up before its registers are saved.
//
// Switches only happen with interrupts off, and never while preemption is
// disabled (preempt_disable): spinlock holders disable it. A tick that
// lands meanwhile is remembered, and preempt_enable() yields afterwards.
//
// FPU/SSE state: the kernel is built without x87/SSE (-mno-sse ...), so a
// switch only has general-purpose registers to save. User threads will use
// SSE, and then each thread gets an FXSAVE area (512 bytes, 16-byte aligned)
// or an XSAVE area (size from CPUID leaf 0xd), saved and restored in
// sched_switch around switch_context; CR4.OSFXSR/OSXSAVE get set at boot.
// Saving every time is simplest and, with XSAVEOPT, cheap; lazy switching
// (CR0.TS and the #NM fault) is not worth its complexity on modern CPUs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHED_QUANTUM_MS   10
#define THREAD_STACK_SIZE  (16 * 1024)
#define THREAD_NAME_MAX    24
#define THREAD_LOCKS_MAX   8  // locks one thread may hold at once (sync.h)

struct lock_dep;
struct process;

enum thread_state {
    THREAD_RUNNING, // on the CPU now
    THREAD_READY,   // in the run queue
    THREAD_BLOCKED, // waiting (sleeping) and off the run queue
    THREAD_DEAD,    // exited; freed once the CPU has switched away from it
};

struct thread {
    uint64_t rsp;            // saved stack pointer while not running
    uint64_t id;
    enum thread_state state;
    unsigned cpu;            // the CPU it is on, or was on last
    unsigned preempt_count;  // preempt_disable() nesting
    bool need_resched;       // a tick wanted to switch while preemption was off
    bool blocking;           // on a wait queue, on its way to sched_block()
    bool wake_pending;       // thread_wake() came before it blocked: don't
    unsigned slice_left;     // ticks left in its quantum while running
    char name[THREAD_NAME_MAX];
    uint8_t *stack;          // THREAD_STACK_SIZE bytes from kmalloc; NULL for the boot thread
    uint64_t stack_top;      // goes into TSS.rsp0 while this thread runs
    uint64_t wake_ns;        // BLOCKED in thread_sleep or sched_block_until: when to wake up
    bool timed_block;        // in sched_block_until, until wait_queue_sleep_until clears it
    uint64_t ticks;          // timer ticks it was running for
    uint64_t switches;       // times it was put on the CPU
    struct thread *next;     // in the run queue or the sleep list
    struct thread *all_next; // in the list of every thread
    struct thread *wait_next; // in a wait queue (sync.h)
    unsigned spins_held;     // spinlocks held: it must not sleep
    unsigned locks_held;     // entries in `held`
    const struct lock_dep *held[THREAD_LOCKS_MAX]; // spinlocks and mutexes, oldest first (sync.c)
    uint64_t cr3;            // its page tables: kernel_space's, or its process's
    struct process *proc;    // the user process it runs, or NULL (proc.h)
};

// Turn the code that is running (kmain, on Limine's stack) into the thread
// "main", and create the idle thread. Needs the heap and time_init(); the
// timer tick starts preempting once this returns.
void sched_init(void);

// Start `entry(arg)` in a new thread, at the back of the run queue. Returns
// NULL if there is no memory. The pointer is only good until the thread
// exits. Returning from `entry` is the same as calling thread_exit().
struct thread *thread_create(const char *name, void (*entry)(void *arg), void *arg);

// End the calling thread. Its stack is freed after the switch away from it.
__attribute__((noreturn)) void thread_exit(void);

// Let the next ready thread run; the caller goes to the back of the queue.
// Returns at once if no other thread is ready.
void thread_yield(void);

// Block for at least `ms` milliseconds; other threads run meanwhile. The
// timer tick wakes it, so it can sleep up to a tick longer. 0 just yields.
void thread_sleep(uint64_t ms);

// The thread that is running. NULL before sched_init().
struct thread *thread_current(void);

// For wait queues (sync.c). sched_block() takes the running thread off the
// CPU until thread_wake() makes it ready again; interrupts must be off.
// Another CPU can wake the thread as soon as it is on the wait queue, before
// it gets here: the wait queue sets thread.blocking (under the queue's lock)
// first, so thread_wake() then only notes the wake-up and sched_block()
// returns at once. thread_wake() is only for a thread blocked in
// sched_block(), not one in thread_sleep().
void sched_block(void);
void thread_wake(struct thread *t);

// Like sched_block(), but the timer tick also makes the thread ready again
// at `wake_ns` (clock_monotonic_ns()) if nothing woke it first. It sets
// thread.timed_block, which the caller clears (interrupts still off) once it
// knows which happened: until then thread_wake() on it is harmless even after
// the deadline woke it.
void sched_block_until(uint64_t wake_ns);

// Threads that exist (running, ready, blocked, or dead and not freed yet),
// every CPU's idle thread included.
size_t sched_thread_count(void);

// Copy up to `max` threads' details into `out` (the pointers inside are not
// safe to follow). Returns how many were copied.
size_t sched_snapshot(struct thread *out, size_t max);

// Keep other threads off the CPU until the matching preempt_enable().
// Nests. Interrupts still come in. Fine to call before sched_init().
// Every spinlock holds this count (sync.h).
void preempt_disable(void);
void preempt_enable(void);

// Called from every CPU's timer tick, interrupts off: counts the tick,
// wakes sleepers whose time has come (on the BSP), and switches threads when
// the running one's quantum is up (or the idle thread is running and another
// is ready).
void sched_tick(void);

// For smp.c: the idle thread for CPU `cpu` (not started; NULL if there is no
// memory), and the end of an application processor's start-up, which turns
// the code running into this CPU's idle thread.
struct thread *sched_create_idle(unsigned cpu);
__attribute__((noreturn)) void sched_ap_enter(void);

// Load `cr3` as the running thread's page tables (for its process), on this
// CPU and in thread.cr3, at once.
void sched_set_cr3(uint64_t cr3);

// Boot self-test: threads that spin without ever yielding all get CPU time,
// get preempted and spread over the CPUs, thread_sleep sleeps long enough,
// and exited threads' memory comes back. Writes a line for the boot log.
// Panics on failure.
void sched_self_test(char *line, size_t size);
