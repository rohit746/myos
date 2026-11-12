#pragma once
// Locks for state that threads and interrupt handlers share.
//
// Spinlocks keep other CPUs out (they spin until it is free) and other
// threads off this CPU (preempt_disable) while held, and spin_lock_irqsave()
// also turns interrupts off, for data an interrupt handler touches too. A
// spinlock holder must not sleep.
//
// Everything that sleeps is built on wait queues: a thread puts itself on
// one and blocks until another thread (or an interrupt handler) wakes it.
// Mutexes, semaphores and condition variables are wait queues with a little
// state, guarded by an internal spinlock. A mutex and a semaphore hand
// themselves straight to the first waiter, so waiters get them in FIFO order.
//
// A thread that blocks does so with interrupts off between putting itself
// on the queue and switching away, so nothing on its own CPU can wake it in
// between. Another CPU can, as soon as the queue's lock is let go: the
// scheduler then remembers the wake-up and the thread doesn't block at all
// (sched_block in sched.h), so no wake-up is missed.
//
// Debug checks, always on; each one panics:
//   - sleeping (mutex_lock, sem_down, cond_wait, thread_sleep) while holding
//     a spinlock, or with interrupts off
//   - unlocking a spinlock or mutex the thread does not hold, and taking a
//     spinlock or mutex the thread already holds
//   - lock order: if lock B was ever taken while holding A, taking A while
//     holding B is a possible deadlock. Locks are grouped into classes by
//     name, so every lock made with the same name counts as one. Two locks
//     of one class held together are not checked against each other.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

// What the debug checks know about a lock. `name` NULL: not order-checked
// (the guards inside mutexes, semaphores and condition variables).
struct lock_dep {
    const char *name;
    uint8_t class_id; // lock class + 1; 0 until the first time it is taken
    bool spin;
};

// ---------------------------------------------------------------------------
// Spinlocks
// ---------------------------------------------------------------------------

struct spinlock {
    volatile uint32_t locked;
    struct thread *owner; // in an interrupt handler: the thread it interrupted
    struct lock_dep dep;
};

#define SPINLOCK_INIT(n) {.dep = {.name = (n), .spin = true}}

void spin_init(struct spinlock *l, const char *name);
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

// Also turn interrupts off, and give back what they were.
uint64_t spin_lock_irqsave(struct spinlock *l);
void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);

// ---------------------------------------------------------------------------
// Wait queues
// ---------------------------------------------------------------------------

struct wait_queue {
    struct thread *head, *tail; // FIFO, linked through thread.wait_next
};

#define WAIT_QUEUE_INIT {0}

// Put the running thread at the back of `wq` and block until it is woken.
// `lock` protects `wq` and must be held, taken with spin_lock_irqsave; it
// is let go while the thread sleeps and taken again before this returns.
// The caller checks its condition again afterwards.
void wait_queue_sleep(struct wait_queue *wq, struct spinlock *lock);

// Like wait_queue_sleep, but give up at `deadline_ns` (clock_monotonic_ns()).
// True if a wake-up came, false if the deadline passed first; either way
// the thread is off `wq` and holds `lock` again.
bool wait_queue_sleep_until(struct wait_queue *wq, struct spinlock *lock, uint64_t deadline_ns);

// Wake the first thread on `wq`, or every one. Hold the lock that protects
// `wq`. wait_queue_wake_one() returns the thread it woke, or NULL.
struct thread *wait_queue_wake_one(struct wait_queue *wq);
size_t wait_queue_wake_all(struct wait_queue *wq);

// ---------------------------------------------------------------------------
// Mutexes: one holder at a time; waiters sleep. Threads only.
// ---------------------------------------------------------------------------

struct mutex {
    struct spinlock guard;
    bool locked;
    struct thread *owner;
    struct wait_queue waiters;
    struct lock_dep dep;
};

void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(const struct mutex *m); // by the running thread

// ---------------------------------------------------------------------------
// Counting semaphores. sem_up() never sleeps, so interrupt handlers can call
// it.
// ---------------------------------------------------------------------------

struct semaphore {
    struct spinlock guard;
    uint64_t count;
    struct wait_queue waiters;
};

void sem_init(struct semaphore *s, uint64_t count);
void sem_down(struct semaphore *s); // wait until count > 0, then take one
void sem_up(struct semaphore *s);

// ---------------------------------------------------------------------------
// Condition variables, used with a mutex.
// ---------------------------------------------------------------------------

struct condvar {
    struct spinlock guard;
    struct wait_queue waiters;
};

void cond_init(struct condvar *c);

// Unlock `m` (which the caller holds), sleep until signalled, lock `m`
// again. Wake-ups can be spurious: wait in a loop that checks the condition.
void cond_wait(struct condvar *c, struct mutex *m);
// The same, but give up at `deadline_ns` (clock_monotonic_ns()): false if
// the deadline passed before a signal came. `m` is held again either way.
bool cond_wait_until(struct condvar *c, struct mutex *m, uint64_t deadline_ns);
void cond_signal(struct condvar *c);    // wake one waiter
void cond_broadcast(struct condvar *c); // wake them all

// ---------------------------------------------------------------------------

// Panic if the running thread may not sleep here: it holds a spinlock, or
// interrupts are off. `what` names the caller in the message.
void might_sleep(const char *what);

// Boot self-test: threads contending on counters under each kind of lock
// always get exact totals, and each debug check fires. Writes a line for
// the boot log. Panics on failure.
void sync_self_test(char *line, size_t size);
